// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/io/observer.h>
#include <workerd/tests/test-fixture.h>

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

}  // namespace
}  // namespace workerd::api
