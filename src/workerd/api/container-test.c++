// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/io/observer.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/stream-utils.h>

#include <capnp/compat/byte-stream.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr tracing::TraceId EXPECTED_TRACE_ID(0x0123456789abcdef, 0xfedcba9876543210);
constexpr tracing::SpanId EXPECTED_PARENT_SPAN_ID(0x123456789abcdef0);
constexpr tracing::TraceFlags EXPECTED_TRACE_FLAGS(0x01);

void expectSpanContext(rpc::SpanContext::Reader reader) {
  auto spanContext = tracing::SpanContext::fromCapnp(reader);
  KJ_EXPECT(spanContext.getTraceId() == EXPECTED_TRACE_ID);
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanContext.getSpanId()) == EXPECTED_PARENT_SPAN_ID);
  KJ_EXPECT(KJ_ASSERT_NONNULL(spanContext.getTraceFlags()) == EXPECTED_TRACE_FLAGS);
}

class TracingRequestObserver final: public RequestObserver {
 public:
  SpanParent getSpan() override {
    return SpanParent::fromSpanContext(
        tracing::SpanContext(EXPECTED_TRACE_ID, EXPECTED_PARENT_SPAN_ID, EXPECTED_TRACE_FLAGS));
  }
};

class MockContainerServer final: public rpc::Container::Server {
 public:
  MockContainerServer(bool& directoryCalled, bool& containerCalled)
      : directoryCalled(directoryCalled),
        containerCalled(containerCalled) {}

  kj::Promise<void> snapshotDirectory(SnapshotDirectoryContext context) override {
    auto params = context.getParams();
    KJ_EXPECT(params.hasSpanContext());
    expectSpanContext(params.getSpanContext());
    KJ_EXPECT(params.getDir() == "/data");
    KJ_EXPECT(params.getName() == "directory-snapshot");
    directoryCalled = true;

    auto snapshot = context.getResults().initSnapshot();
    snapshot.setId("directory-snapshot-id");
    snapshot.setSize(123);
    snapshot.setDir(params.getDir());
    snapshot.setName(params.getName());
    return kj::READY_NOW;
  }

  kj::Promise<void> snapshotContainer(SnapshotContainerContext context) override {
    auto params = context.getParams();
    KJ_EXPECT(params.hasSpanContext());
    expectSpanContext(params.getSpanContext());
    KJ_EXPECT(params.getName() == "container-snapshot");
    containerCalled = true;

    auto snapshot = context.getResults().initSnapshot();
    snapshot.setId("container-snapshot-id");
    snapshot.setSize(456);
    snapshot.setName(params.getName());
    return kj::READY_NOW;
  }

 private:
  bool& directoryCalled;
  bool& containerCalled;
};

// Records the arguments passed to exec()/resize() so tests can assert the pty option is piped
// through correctly.
struct ExecObservations {
  bool execCalled = false;
  bool hadPty = false;
  uint initialCols = 0;
  uint initialRows = 0;
  uint resizeCols = 0;
  uint resizeRows = 0;
};

class MockExecProcessHandle final: public rpc::Container::ProcessHandle::Server {
 public:
  MockExecProcessHandle(capnp::ByteStreamFactory& byteStreamFactory,
      ExecObservations& observations,
      kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller)
      : byteStreamFactory(byteStreamFactory),
        observations(observations),
        resizeFulfiller(kj::mv(resizeFulfiller)) {}

  kj::Promise<void> wait(WaitContext context) override {
    context.getResults().setExitCode(0);
    return kj::READY_NOW;
  }

  kj::Promise<void> stdinWriter(StdinWriterContext context) override {
    context.getResults().setWriter(byteStreamFactory.kjToCapnp(
        capnp::ExplicitEndOutputStream::wrap(newNullOutputStream(), []() {})));
    return kj::READY_NOW;
  }

  kj::Promise<void> resize(ResizeContext context) override {
    observations.resizeCols = context.getParams().getCols();
    observations.resizeRows = context.getParams().getRows();
    KJ_IF_SOME(fulfiller, resizeFulfiller) {
      fulfiller->fulfill();
    }
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ExecObservations& observations;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller;
};

class MockExecContainerServer final: public rpc::Container::Server {
 public:
  MockExecContainerServer(capnp::ByteStreamFactory& byteStreamFactory,
      ExecObservations& observations,
      kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller)
      : byteStreamFactory(byteStreamFactory),
        observations(observations),
        resizeFulfiller(kj::mv(resizeFulfiller)) {}

  kj::Promise<void> exec(ExecContext context) override {
    observations.execCalled = true;
    auto params = context.getParams().getParams();
    observations.hadPty = params.hasPty();
    if (params.hasPty()) {
      observations.initialCols = params.getPty().getCols();
      observations.initialRows = params.getPty().getRows();
    }

    auto process = context.getResults().initProcess();
    process.setPid(4321);
    process.setHandle(
        kj::heap<MockExecProcessHandle>(byteStreamFactory, observations, kj::mv(resizeFulfiller)));
    return kj::READY_NOW;
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  ExecObservations& observations;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> resizeFulfiller;
};

TestFixture makeFixture() {
  return TestFixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        []() -> kj::Own<RequestObserver> { return kj::refcounted<TracingRequestObserver>(); }),
  });
}

KJ_TEST("Container::snapshotDirectory propagates the current span context") {
  bool directoryCalled = false;
  bool containerCalled = false;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = kj::heap<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(directoryCalled, containerCalled)),
        true);
    auto promise = container->snapshotDirectory(env.js,
        Container::DirectorySnapshotOptions{
          .dir = kj::str("/data"),
          .name = kj::str("directory-snapshot"),
        });
    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(container));
  });

  KJ_EXPECT(directoryCalled);
  KJ_EXPECT(!containerCalled);
}

KJ_TEST("Container::snapshotContainer propagates the current span context") {
  bool directoryCalled = false;
  bool containerCalled = false;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto container = kj::heap<Container>(
        rpc::Container::Client(kj::heap<MockContainerServer>(directoryCalled, containerCalled)),
        true);
    auto promise = container->snapshotContainer(env.js,
        Container::SnapshotOptions{
          .name = kj::str("container-snapshot"),
        });
    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(container));
  });

  KJ_EXPECT(!directoryCalled);
  KJ_EXPECT(containerCalled);
}

KJ_TEST("Container::exec forwards pty options and resize() sends a resize RPC") {
  ExecObservations observations;
  auto fixture = makeFixture();

  auto paf = kj::newPromiseAndFulfiller<void>();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container = kj::heap<Container>(
        rpc::Container::Client(kj::heap<MockExecContainerServer>(
            env.context.getByteStreamFactory(), observations, kj::mv(paf.fulfiller))),
        true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(
        ExecPtyOptions{.cols = static_cast<uint16_t>(120), .rows = static_cast<uint16_t>(40)});

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
      process->resize(js, 100, 50);
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise))
        .then([promise = kj::mv(paf.promise)]() mutable {
      return kj::mv(promise);
    }).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  KJ_EXPECT(observations.initialCols == 120);
  KJ_EXPECT(observations.initialRows == 40);
  KJ_EXPECT(observations.resizeCols == 100);
  KJ_EXPECT(observations.resizeRows == 50);
}

KJ_TEST("Container::exec without pty leaves the process non-pty and rejects resize()") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::none)
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(!process->getIsPty());

      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 80, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should throw when the process is not a PTY");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(!observations.hadPty);
}

KJ_TEST("Container::exec with pty: true boolean shorthand enables PTY with default dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  // Boolean shorthand sends no dimensions; the server uses its defaults.
  KJ_EXPECT(observations.initialCols == 0);
  KJ_EXPECT(observations.initialRows == 0);
}

KJ_TEST("Container::exec with pty: {} empty object enables PTY with server defaults") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(ExecPtyOptions{});

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      KJ_EXPECT(process->getIsPty());
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });

  KJ_EXPECT(observations.execCalled);
  KJ_EXPECT(observations.hadPty);
  // An object with no dimensions sends no dimensions; the server uses its defaults.
  KJ_EXPECT(observations.initialCols == 0);
  KJ_EXPECT(observations.initialRows == 0);
}

KJ_TEST("Container::exec with pty rejects explicit stderr: \"pipe\"") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);
    options.$stderr.emplace(kj::str("pipe"));

    bool threw = false;
    JSG_TRY(env.js) {
      container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options));
    }
    JSG_CATCH(exception KJ_UNUSED) {
      threw = true;
    }
    KJ_EXPECT(threw, "exec() should throw when stderr is \"pipe\" with PTY enabled");

    return kj::READY_NOW;
  });
}

KJ_TEST("Container::exec resize() rejects zero dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      // Zero cols should throw
      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 0, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject zero cols");

      // Zero rows should throw
      threw = false;
      JSG_TRY(js) {
        process->resize(js, 80, 0);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject zero rows");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });
}

KJ_TEST("Container::exec resize() rejects out-of-range dimensions") {
  ExecObservations observations;
  auto fixture = makeFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto container =
        kj::heap<Container>(rpc::Container::Client(kj::heap<MockExecContainerServer>(
                                env.context.getByteStreamFactory(), observations, kj::none)),
            true);

    ExecOptions options;
    options.pty = kj::OneOf<bool, ExecPtyOptions>(true);

    auto jsPromise = container->exec(env.js, kj::arr(kj::str("/bin/sh")), kj::mv(options))
                         .then(env.js, [](jsg::Lock& js, jsg::Ref<ExecProcess> process) {
      // Cols > 65535 should throw
      bool threw = false;
      JSG_TRY(js) {
        process->resize(js, 65536, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject cols > 65535");

      // Negative cols should throw
      threw = false;
      JSG_TRY(js) {
        process->resize(js, -1, 24);
      }
      JSG_CATCH(exception KJ_UNUSED) {
        threw = true;
      }
      KJ_EXPECT(threw, "resize() should reject negative cols");
    });

    return env.context.awaitJs(env.js, kj::mv(jsPromise)).attach(kj::mv(container));
  });
}

}  // namespace
}  // namespace workerd::api
