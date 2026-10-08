// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Async tracing wired into IoContext: the IsolateObserver seam, REQUEST roots and turns.

#include <workerd/io/async-trace.h>
#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

using kj::uint;

class RecordingListener final: public AsyncTraceListener {
 public:
  explicit RecordingListener(kj::Vector<kj::String>& events): events(events) {}
  ~RecordingListener() noexcept(false) {
    events.add(kj::str("listener destroyed"));
  }

  void onContextBegin(uint64_t ctx, uint64_t isolate) override {
    events.add(kj::str("context_begin"));
  }
  void onInit(uint64_t ctx, const AsyncInitEvent& e) override {
    events.add(kj::str("init ", e.id, " kind=", static_cast<uint>(e.kind), " name=", e.name,
        " trigger=", e.trigger));
  }
  void onSettle(uint64_t ctx, AsyncId id, AsyncOutcome outcome, uint64_t atNs) override {
    events.add(kj::str("settle ", id));
  }
  void onBefore(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("before ", id));
  }
  void onAfter(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("after ", id));
  }
  void onDestroy(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("destroy ", id));
  }
  void onTurn(uint64_t ctx, const AsyncTurn& turn) override {
    KJ_EXPECT(turn.startNs <= KJ_ASSERT_NONNULL(turn.lockedNs));
    KJ_EXPECT(KJ_ASSERT_NONNULL(turn.lockedNs) <= turn.endNs);
    events.add(kj::str("turn cause=", turn.cause));
  }
  void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& s) override {
    events.add(kj::str("context_end created=", s.created, " unknown=", s.unknown,
        " unbalanced=", s.unbalanced, " foreign=", s.foreignThread));
  }

 private:
  kj::Vector<kj::String>& events;
};

// Enables async tracing and records into `events`, if `addSink`.
class TracingObserver final: public IsolateObserver {
 public:
  TracingObserver(kj::Vector<kj::String>& events, bool addSink): events(events), addSink(addSink) {}

  kj::Maybe<AsyncTraceConfig> getAsyncTraceConfig() const override {
    return AsyncTraceConfig{};
  }

  void addAsyncTraceSinks(AsyncTraceSinks& sinks,
      kj::StringPtr worker,
      kj::Maybe<kj::StringPtr> actorId) const override {
    events.add(kj::str("addAsyncTraceSinks worker=", worker, " actor=", actorId.orDefault("-"_kj)));
    if (addSink) {
      sinks.add(kj::heap<RecordingListener>(events));
    }
  }

 private:
  kj::Vector<kj::String>& events;
  bool addSink;
};

kj::String joined(kj::ArrayPtr<const kj::String> events) {
  return kj::strArray(events, "\n");
}

bool has(const kj::Vector<kj::String>& events, kj::StringPtr event) {
  for (auto& e: events) {
    if (e == event) return true;
  }
  return false;
}

KJ_TEST("no tracker unless the isolate observer enables tracing") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.context.tryGetAsyncTracker() == kj::none);
  });
}

KJ_TEST("no tracker if no sink is added") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<TracingObserver>(events, false),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.context.tryGetAsyncTracker() == kj::none);
  });
  KJ_EXPECT(events.size() == 1, joined(events));
  KJ_EXPECT(events[0].startsWith("addAsyncTraceSinks worker="), events[0]);
  KJ_EXPECT(events[0].endsWith(" actor=-"), events[0]);
}

KJ_TEST("a request's turns are attributed to its REQUEST resource") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<TracingObserver>(events, true),
  });

  AsyncId requestId = 0;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& tracker = KJ_ASSERT_NONNULL(env.context.tryGetAsyncTracker());
    requestId = tracker.current();
    KJ_EXPECT(requestId != 0);
  });

  // The fixture's newIncomingRequest() delivers the request, so it names the REQUEST resource.
  auto expected =
      kj::strArray(kj::arr(kj::str("context_begin"),
                       kj::str("init ", requestId, " kind=0 name=newIncomingRequest", " trigger=0"),
                       kj::str("turn cause=", requestId), kj::str("settle ", requestId),
                       kj::str("context_end created=1 unknown=0 unbalanced=0 foreign=0"),
                       kj::str("listener destroyed")),
          "\n");
  KJ_EXPECT(joined(events.asPtr().slice(1)) == expected, joined(events));
}

KJ_TEST("resources created in a turn are triggered by the turn's request") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<TracingObserver>(events, true),
  });

  AsyncId requestId = 0;
  AsyncId timerId = 0;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& tracker = KJ_ASSERT_NONNULL(env.context.tryGetAsyncTracker());
    requestId = tracker.current();
    auto timer = tracker.create(AsyncKind::TIMER, "setTimeout"_kj);
    timerId = timer.getId();
  });
  KJ_EXPECT(has(events, kj::str("init ", timerId, " kind=3 name=setTimeout trigger=", requestId)),
      joined(events));
  KJ_EXPECT(has(events, kj::str("destroy ", timerId)), joined(events));
}

KJ_TEST("each request on a shared context causes its own turns") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<TracingObserver>(events, true),
  });

  auto context = fixture.newIoContext();
  AsyncId first = 0;
  AsyncId second = 0;
  {
    auto request = fixture.newIncomingRequest(*context);
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      first = KJ_ASSERT_NONNULL(env.context.tryGetAsyncTracker()).current();
    });
    KJ_EXPECT(request->getAsyncTraceId() == first);
  }
  {
    auto request = fixture.newIncomingRequest(*context);
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      second = KJ_ASSERT_NONNULL(env.context.tryGetAsyncTracker()).current();
    });
  }
  KJ_EXPECT(first != 0);
  KJ_EXPECT(second != 0);
  KJ_EXPECT(first != second);
  KJ_EXPECT(has(events, kj::str("turn cause=", first)), joined(events));
  KJ_EXPECT(has(events, kj::str("turn cause=", second)), joined(events));

  // The context outlives both requests; closing it reports the stats.
  KJ_EXPECT(!has(events, kj::str("listener destroyed")));
  context = nullptr;
  KJ_EXPECT(has(events, kj::str("context_end created=2 unknown=0 unbalanced=0 foreign=0")),
      joined(events));
}

KJ_TEST("an actor's context is reported with its ID") {
  kj::Vector<kj::String> events;
  TestFixture fixture({
    .actorId = Worker::Actor::Id(kj::str("my-actor")),
    .useRealTimers = false,
    .isolateObserver = kj::atomicRefcounted<TracingObserver>(events, true),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    KJ_EXPECT(env.context.tryGetAsyncTracker() != kj::none);
  });
  KJ_EXPECT(events.size() > 0);
  KJ_EXPECT(events[0].endsWith(" actor=my-actor"), events[0]);
}

}  // namespace
}  // namespace workerd
