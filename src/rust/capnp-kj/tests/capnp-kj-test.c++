// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Calls between C++ and Rust over capnp-kj, in both directions, on one KJ event loop.

#include <workerd/rust/capnp-kj/ffi.h>
#include <workerd/rust/capnp-kj/tests/ffi.rs.h>

#include <kj-rs/kj-rs.h>
#include <rust/capnp-kj/tests/test.capnp.h>

#include <capnp/capability.h>
#include <capnp/membrane.h>
#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd::rust::capnp_kj::test {
namespace {

using capnp_kj::clientFromRust;
using capnp_kj::clientToRust;

// A C++ TestEcho server, the counterpart of EchoServer in lib.rs.
class CxxEcho final: public TestEcho::Server {
 public:
  CxxEcho(kj::String prefix, kj::uint& hangStarts, kj::uint& hangDrops)
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

  kj::Promise<void> noop(NoopContext context) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> tailEcho(TailEchoContext context) override {
    auto params = context.getParams();
    auto request = params.getCallee().echoRequest();
    request.setText(params.getText());
    // echo()'s results have the same layout as tailEcho()'s, so they can answer for it.
    return capnp::CallContextHook::from(context).tailCall(
        capnp::RequestHook::from(kj::mv(request)));
  }

  kj::Promise<void> hang(HangContext context) override {
    ++hangStarts;
    struct Guard {
      kj::uint& drops;
      ~Guard() {
        ++drops;
      }
    };
    return kj::Promise<void>(kj::NEVER_DONE).attach(kj::heap<Guard>(hangDrops));
  }

 private:
  kj::String prefix;
  kj::uint& hangStarts;
  kj::uint& hangDrops;
  kj::Vector<kj::byte> writtenData;
};

constexpr kj::Exception::DetailTypeId TEST_DETAIL_ID = 0x5d0e'9b31'c4a2'7f68ull;

// A TestEcho server whose echo() fails with an exception that carries a detail entry, as
// workerd's exceptions do.
class DetailedFailureEcho final: public TestEcho::Server {
 public:
  kj::Promise<void> echo(EchoContext context) override {
    auto exception = KJ_EXCEPTION(DISCONNECTED, "detailed failure");
    exception.setDetail(TEST_DETAIL_ID, kj::heapArray("payload"_kjb));
    return kj::mv(exception);
  }
};

// Waits for `promise` to fail, and checks that its exception is DetailedFailureEcho's, whole.
template <typename T>
void expectDetailedFailure(kj::Promise<T> promise, kj::WaitScope& waitScope) {
  auto exception = promise
                       .then([](auto&&) -> kj::Maybe<kj::Exception> { return kj::none; },
                           [](kj::Exception&& e) -> kj::Maybe<kj::Exception> {
    return kj::mv(e);
  }).wait(waitScope);
  auto& e = KJ_ASSERT_NONNULL(exception, "the call didn't fail");
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED);
  KJ_EXPECT(e.getDescription() == "detailed failure", e.getDescription());
  KJ_IF_SOME(detail, e.getDetail(TEST_DETAIL_ID)) {
    KJ_EXPECT(detail == "payload"_kjb);
  } else {
    KJ_FAIL_EXPECT("the exception lost its detail");
  }
}

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
  kj::uint cxxHangStarts = 0;
  kj::uint cxxHangDrops = 0;

  TestEcho::Client rustEcho(kj::StringPtr prefix) {
    return clientFromRust<TestEcho>(new_echo_server(::rust::Str(prefix.begin(), prefix.size())));
  }
  TestEcho::Client cxxEcho(kj::StringPtr prefix) {
    return kj::heap<CxxEcho>(kj::str(prefix), cxxHangStarts, cxxHangDrops);
  }
};

// A two-party RPC connection over an in-memory pipe, whose server end serves `bootstrap`.
struct RpcConnection {
  explicit RpcConnection(capnp::Capability::Client bootstrap)
      : server(*pipe.ends[1], kj::mv(bootstrap), capnp::rpc::twoparty::Side::SERVER),
        client(*pipe.ends[0]) {}

  TestEcho::Client echo() {
    return client.bootstrap().castAs<TestEcho>();
  }

  kj::TwoWayPipe pipe = kj::newTwoWayPipe();
  capnp::TwoPartyClient server;
  capnp::TwoPartyClient client;
};

// A membrane that lets every call through, so that calls through it carry its own call contexts.
class OpenMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }
  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }
  kj::Own<capnp::MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }
};

TestEcho::Client throughMembrane(TestEcho::Client client) {
  return capnp::membrane(kj::mv(client), kj::refcounted<OpenMembrane>());
}

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

KJ_TEST("Rust pipelines on a capability a Rust server returns") {
  Fixture f;
  auto result = rust_child_echo(clientToRust(f.rustEcho("rust:")), rs("child:"), rs("hello"))
                    .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "rust:child:hello");
}

KJ_TEST("a Rust server calls a capability that Rust passed in params") {
  Fixture f;
  auto viaCxx = rust_call_back(
      clientToRust(f.rustEcho("rust:")), clientToRust(f.cxxEcho("cxx:")), rs("hello"))
                    .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaCxx)) == "cxx:hello");
  auto viaRust = rust_call_back(
      clientToRust(f.rustEcho("rust:")), clientToRust(f.rustEcho("callee:")), rs("hello"))
                     .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaRust)) == "callee:hello");
}

KJ_TEST("Rust reads the empty results of a server that writes none") {
  Fixture f;
  rust_noop(clientToRust(f.cxxEcho("cxx:"))).wait(f.waitScope);
  rust_noop(clientToRust(f.rustEcho("rust:"))).wait(f.waitScope);
}

KJ_TEST("C++ reads the empty results of a Rust server that writes none") {
  Fixture f;
  f.rustEcho("rust:").noopRequest().send().wait(f.waitScope);
}

KJ_TEST("a C++ server answers a call from Rust with a tail call") {
  Fixture f;
  auto toCxx = rust_tail_echo(
      clientToRust(f.cxxEcho("outer:")), clientToRust(f.cxxEcho("cxx:")), rs("hello"))
                   .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(toCxx)) == "cxx:hello");
  auto toRust = rust_tail_echo(
      clientToRust(f.cxxEcho("outer:")), clientToRust(f.rustEcho("rust:")), rs("hello"))
                    .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(toRust)) == "rust:hello");
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
  for (const auto& chunk: {"one,"_kj, "two,"_kj, "three"_kj}) {
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

KJ_TEST("Rust's streaming calls reach a C++ server in the order Rust sent them") {
  Fixture f;
  auto result = rust_write_awaited_in_reverse(clientToRust(f.cxxEcho("cxx:"))).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "one,two");
}

KJ_TEST("Rust's streaming calls reach a Rust server in the order Rust sent them") {
  Fixture f;
  auto result = rust_write_awaited_in_reverse(clientToRust(f.rustEcho("rust:"))).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "one,two");
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

KJ_TEST("a C++ exception keeps its details through a Rust server that returns it") {
  Fixture f;
  auto echo = f.rustEcho("rust:");
  auto request = echo.callBackRequest();
  request.setCallee(kj::heap<DetailedFailureEcho>());
  request.setText("hello");
  expectDetailedFailure(request.send(), f.waitScope);
}

KJ_TEST("a C++ exception keeps its details through a Rust client that returns it") {
  Fixture f;
  TestEcho::Client failing = kj::heap<DetailedFailureEcho>();
  expectDetailedFailure(rust_echo(clientToRust(kj::mv(failing)), rs("hello")), f.waitScope);
}

KJ_TEST("Rust calls C++ and Rust servers over RPC") {
  Fixture f;
  RpcConnection toCxx(f.cxxEcho("cxx:"));
  auto viaCxx = rust_echo(clientToRust(toCxx.echo()), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaCxx)) == "cxx:hello");
  RpcConnection toRust(f.rustEcho("rust:"));
  auto viaRust = rust_echo(clientToRust(toRust.echo()), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaRust)) == "rust:hello");
}

KJ_TEST("C++ calls a Rust server over RPC") {
  Fixture f;
  RpcConnection connection(f.rustEcho("rust:"));
  auto echo = connection.echo();
  KJ_EXPECT(echoText(echo, "hello", f.waitScope) == "rust:hello");
}

KJ_TEST("Rust passes and pipelines on capabilities over RPC") {
  Fixture f;
  RpcConnection toRust(f.rustEcho("rust:"));
  auto calledBack =
      rust_call_back(clientToRust(toRust.echo()), clientToRust(f.cxxEcho("cxx:")), rs("hello"))
          .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(calledBack)) == "cxx:hello");
  RpcConnection toCxx(f.cxxEcho("cxx:"));
  auto child =
      rust_child_echo(clientToRust(toCxx.echo()), rs("child:"), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(child)) == "cxx:child:hello");
}

KJ_TEST("Rust streams to a Rust server over RPC") {
  Fixture f;
  RpcConnection connection(f.rustEcho("rust:"));
  ::rust::Vec<::rust::String> chunks;
  chunks.push_back(rs("one,"));
  chunks.push_back(rs("two"));
  auto result = rust_write(clientToRust(connection.echo()), kj::mv(chunks)).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(result)) == "one,two");
}

KJ_TEST("Rust calls C++ and Rust servers through a membrane") {
  Fixture f;
  auto viaCxx =
      rust_echo(clientToRust(throughMembrane(f.cxxEcho("cxx:"))), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaCxx)) == "cxx:hello");
  auto viaRust =
      rust_echo(clientToRust(throughMembrane(f.rustEcho("rust:"))), rs("hello")).wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(viaRust)) == "rust:hello");
  auto child =
      rust_child_echo(clientToRust(throughMembrane(f.rustEcho("rust:"))), rs("child:"), rs("hello"))
          .wait(f.waitScope);
  KJ_EXPECT(toString(kj::mv(child)) == "rust:child:hello");
}

KJ_TEST("C++ calls a Rust server through a membrane") {
  Fixture f;
  auto echo = throughMembrane(f.rustEcho("rust:"));
  KJ_EXPECT(echoText(echo, "hello", f.waitScope) == "rust:hello");
  auto request = echo.callBackRequest();
  request.setCallee(f.cxxEcho("cxx:"));
  request.setText("hello");
  KJ_EXPECT(request.send().wait(f.waitScope).getText() == "cxx:hello");
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

// Starts a `hang()` call to `echo`, cancels it, and returns whether that dropped the Rust server's
// future.
bool cancelingHangDropsIt(TestEcho::Client& echo, kj::WaitScope& waitScope) {
  auto startsBefore = hang_starts();
  auto dropsBefore = hang_drops();
  {
    auto promise = echo.hangRequest().send();
    waitScope.poll();
    KJ_ASSERT(hang_starts() == startsBefore + 1);
  }
  waitScope.poll();
  return hang_drops() != dropsBefore;
}

KJ_TEST("a Rust server can allow cancellation for some of its methods") {
  Fixture f;
  auto echo = clientFromRust<TestEcho>(new_per_method_echo_server("rust:", true));
  KJ_EXPECT(cancelingHangDropsIt(echo, f.waitScope));
  // The other methods still work.
  KJ_EXPECT(echoText(echo, "hello", f.waitScope) == "rust:hello");
}

KJ_TEST("a Rust server can run some of its methods to completion") {
  Fixture f;
  auto echo = clientFromRust<TestEcho>(new_per_method_echo_server("rust:", false));
  KJ_EXPECT(!cancelingHangDropsIt(echo, f.waitScope));
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
