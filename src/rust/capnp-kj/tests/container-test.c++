// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// C++ uses a Rust `rpc::Container` server the way `api/container.c++` uses the container
// runtime: through `rpc::Container::Client`, with `ByteStream`s from a `capnp::ByteStreamFactory`.

#include <workerd/io/container.capnp.h>
#include <workerd/rust/capnp-kj/ffi.h>
#include <workerd/rust/capnp-kj/tests/ffi.rs.h>

#include <kj-rs/kj-rs.h>

#include <capnp/compat/byte-stream.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd::rust::capnp_kj::test {
namespace {

struct Fixture {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  capnp::ByteStreamFactory byteStreamFactory;
  rpc::Container::Client container =
      capnp_kj::clientFromRust<rpc::Container>(new_container_server());

  bool isRunning() {
    return container.statusRequest().send().wait(waitScope).getRunning();
  }
};

KJ_TEST("Rust Container: status, start and signal") {
  Fixture f;
  KJ_EXPECT(!f.isRunning());

  f.container.startRequest().send().wait(f.waitScope);
  KJ_EXPECT(f.isRunning());
  KJ_EXPECT_THROW_MESSAGE(
      "container is already running", f.container.startRequest().send().wait(f.waitScope));

  auto signal = f.container.signalRequest();
  signal.setSigno(9);
  signal.send().wait(f.waitScope);
  KJ_EXPECT(!f.isRunning());
}

KJ_TEST("Rust Container: connect to a pipelined port and echo through ByteStreams") {
  Fixture f;
  auto pipe = kj::newOneWayPipe();

  auto portRequest = f.container.getTcpPortRequest();
  portRequest.setPort(8080);
  auto port = portRequest.send().getPort();

  auto connect = port.connectRequest();
  connect.setDown(f.byteStreamFactory.kjToCapnp(kj::mv(pipe.out)));
  auto up = f.byteStreamFactory.capnpToKjExplicitEnd(connect.send().getUp());

  auto received = pipe.in->readAllText();
  up->write("hello, "_kj.asBytes()).wait(f.waitScope);
  up->write("container"_kj.asBytes()).wait(f.waitScope);
  up->end().wait(f.waitScope);
  KJ_EXPECT(received.wait(f.waitScope) == "8080:hello, container");
}

KJ_TEST("Rust Container: exec writes to the stdout ByteStream and returns a ProcessHandle") {
  Fixture f;
  auto pipe = kj::newOneWayPipe();

  auto exec = f.container.execRequest();
  auto cmd = exec.initCmd(2);
  cmd.set(0, "echo");
  cmd.set(1, "hi");
  exec.setStdoutWriter(f.byteStreamFactory.kjToCapnp(kj::mv(pipe.out)));
  auto response = exec.send();

  auto output = pipe.in->readAllText().wait(f.waitScope);
  KJ_EXPECT(output == "echo hi\n");

  auto results = response.wait(f.waitScope);
  auto process = results.getProcess();
  KJ_EXPECT(process.getPid() == 42);
  KJ_EXPECT(process.getHandle().waitRequest().send().wait(f.waitScope).getExitCode() == 0);
}

KJ_TEST("Rust Container: a method the server does not implement fails as unimplemented") {
  Fixture f;
  try {
    f.container.destroyRequest().send().wait(f.waitScope);
    KJ_FAIL_EXPECT("destroy() should have failed");
  } catch (kj::Exception& e) {
    KJ_EXPECT(e.getType() == kj::Exception::Type::UNIMPLEMENTED, e);
  }
}

}  // namespace
}  // namespace workerd::rust::capnp_kj::test
