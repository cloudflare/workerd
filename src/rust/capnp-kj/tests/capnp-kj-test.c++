// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Calls between C++ and Rust over capnp-kj, in both directions, on one KJ event loop.

#include <workerd/rust/capnp-kj/ffi.h>
#include <workerd/rust/capnp-kj/tests/ffi.rs.h>

#include <kj-rs/kj-rs.h>
#include <rust/capnp-kj/tests/test.capnp.h>

#include <capnp/capability.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd::rust::capnp_kj::test {
namespace {

using capnp_kj::clientFromRust;
using capnp_kj::clientToRust;

// A C++ TestEcho server, the counterpart of EchoServer in lib.rs.
class CxxEcho final: public TestEcho::Server {
 public:
  CxxEcho(kj::String prefix, uint& hangStarts, uint& hangDrops)
      : prefix(kj::mv(prefix)),
        hangStarts(hangStarts),
        hangDrops(hangDrops) {}

  kj::Promise<void> echo(EchoContext context) override {
    context.getResults().setText(kj::str(prefix, context.getParams().getText()));
    return kj::READY_NOW;
  }

  kj::Promise<void> fail(FailContext context) override {
    KJ_FAIL_REQUIRE(context.getParams().getDescription());
  }

  kj::Promise<void> child(ChildContext context) override {
    context.getResults().setChild(
        kj::heap<CxxEcho>(kj::str(prefix, context.getParams().getPrefix()), hangStarts, hangDrops));
    return kj::READY_NOW;
  }

  kj::Promise<void> callBack(CallBackContext context) override {
    auto params = context.getParams();
    auto request = params.getCallee().echoRequest();
    request.setText(params.getText());
    auto response = co_await request.send();
    context.getResults().setText(response.getText());
  }

  kj::Promise<void> write(WriteContext context) override {
    writtenData.addAll(context.getParams().getData());
    return kj::READY_NOW;
  }

  kj::Promise<void> written(WrittenContext context) override {
    context.getResults().setData(writtenData.asPtr());
    return kj::READY_NOW;
  }

  kj::Promise<void> hang(HangContext context) override {
    ++hangStarts;
    struct Guard {
      uint& drops;
      ~Guard() {
        ++drops;
      }
    };
    return kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<Guard>(hangDrops));
  }

 private:
  kj::String prefix;
  uint& hangStarts;
  uint& hangDrops;
  kj::Vector<kj::byte> writtenData;
};

kj::String echoText(TestEcho::Client& echo, kj::StringPtr text, kj::WaitScope& waitScope) {
  auto request = echo.echoRequest();
  request.setText(text);
  return kj::str(request.send().wait(waitScope).getText());
}

kj::String toString(::rust::String s) {
  return kj::str(kj::arrayPtr(s.data(), s.size()));
}

struct Fixture {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  uint cxxHangStarts = 0;
  uint cxxHangDrops = 0;

  TestEcho::Client rustEcho(kj::StringPtr prefix) {
    return clientFromRust<TestEcho>(new_echo_server(::rust::Str(prefix.begin(), prefix.size())));
  }
  TestEcho::Client cxxEcho(kj::StringPtr prefix) {
    return kj::heap<CxxEcho>(kj::str(prefix), cxxHangStarts, cxxHangDrops);
  }
};

::rust::String rs(kj::StringPtr s) {
  return ::rust::String(s.begin(), s.size());
}

KJ_TEST("C++ calls a Rust server") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  KJ_EXPECT(echoText(echo, "hello", f.waitScope) == "rust:hello");
}

KJ_TEST("Rust calls a C++ server") {
  Fixture f;
  auto result = rust_echo(clientToRust(f.cxxEcho("cxx:")), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "cxx:hello");
}

KJ_TEST("Rust calls a Rust server through KJ") {
  Fixture f;
  auto result = rust_echo(clientToRust(f.rustEcho("rust:")), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "rust:hello");
}

KJ_TEST("C++ pipelines on a capability a Rust server returns") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  auto childRequest = echo.childRequest();
  childRequest.setPrefix("child:");
  auto child = childRequest.send().getChild();
  KJ_EXPECT(echoText(child, "hello", f.waitScope) == "rust:child:hello");
}

KJ_TEST("Rust pipelines on a capability a C++ server returns") {
  Fixture f;
  auto result =
      rust_child_echo(clientToRust(f.cxxEcho("cxx:")), rs("child:"), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "cxx:child:hello");
}

KJ_TEST("a Rust server calls a C++ capability passed in params") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  auto request = echo.callBackRequest();
  request.setCallee(f.cxxEcho("cxx:"));
  request.setText("hello");
  KJ_EXPECT(request.send().wait(f.waitScope).getText() == "cxx:hello");
}

KJ_TEST("a C++ server calls a Rust capability that Rust passed in params") {
  Fixture f;
  auto result = rust_call_back(
      clientToRust(f.cxxEcho("cxx:")), clientToRust(f.rustEcho("rust:")), rs("hello"))
                    .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "rust:hello");
}

KJ_TEST("C++ streams to a Rust server") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  for (auto chunk: {"one,"_kj, "two,"_kj, "three"_kj}) {
    auto request = echo.writeRequest();
    request.setData(chunk.asBytes());
    request.send().wait(f.waitScope);
  }
  auto response = echo.writtenRequest().send().wait(f.waitScope);
  KJ_EXPECT(kj::str(response.getData().asChars()) == "one,two,three");
}

KJ_TEST("Rust streams to a C++ server") {
  Fixture f;
  ::rust::Vec<::rust::String> chunks;
  chunks.push_back(rs("one,"));
  chunks.push_back(rs("two,"));
  chunks.push_back(rs("three"));
  auto result = rust_write(clientToRust(f.cxxEcho("cxx:")), kj::mv(chunks)).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "one,two,three");
}

KJ_TEST("a Rust server's error reaches C++") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  auto request = echo.failRequest();
  request.setDescription("rust failure");
  KJ_EXPECT_THROW_MESSAGE("rust failure", request.send().wait(f.waitScope));
}

KJ_TEST("a C++ server's error reaches Rust, and comes back to C++") {
  Fixture f;
  KJ_EXPECT_THROW_MESSAGE("cxx failure",
      rust_fail(clientToRust(f.cxxEcho("cxx:")), rs("cxx failure")).wait(f.waitScope));
}

KJ_TEST("canceling a call to a Rust server leaves it running, as KJ does by default") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  auto startsBefore = hang_starts();
  auto dropsBefore = hang_drops();
  {
    auto promise = echo.hangRequest().send();
    f.waitScope.poll();
    KJ_ASSERT(hang_starts() == startsBefore + 1);
  }
  f.waitScope.poll();
  KJ_EXPECT(hang_drops() == dropsBefore);
}

KJ_TEST("canceling a call to a Rust server that allows cancellation drops its future") {
  Fixture f;
  auto echo = clientFromRust<TestEcho>(new_cancelable_echo_server("rust:"));
  auto startsBefore = hang_starts();
  auto dropsBefore = hang_drops();
  {
    auto promise = echo.hangRequest().send();
    f.waitScope.poll();
    KJ_ASSERT(hang_starts() == startsBefore + 1);
  }
  f.waitScope.poll();
  KJ_EXPECT(hang_drops() == dropsBefore + 1);
}

KJ_TEST("canceling a Rust call to a C++ server leaves the server running") {
  Fixture f;
  {
    // A promise runs only once something waits on it; start this one.
    auto promise = rust_hang(clientToRust(f.cxxEcho("cxx:"))).eagerlyEvaluate(nullptr);
    f.waitScope.poll();
    KJ_ASSERT(f.cxxHangStarts == 1);
  }
  f.waitScope.poll();
  KJ_EXPECT(f.cxxHangDrops == 0);
}

}  // namespace
}  // namespace workerd::rust::capnp_kj::test
