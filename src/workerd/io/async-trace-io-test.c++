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

// Labels resources by name ("setTimeout#2"; the request is "request"), so expected sequences can
// be written without knowing IDs.
class LabelingListener final: public AsyncTraceListener {
 public:
  explicit LabelingListener(kj::Vector<kj::String>& events): events(events) {}

  void onInit(uint64_t ctx, const AsyncInitEvent& e) override {
    kj::String label;
    if (e.kind == AsyncKind::REQUEST) {
      label = kj::str("request");
    } else {
      auto name = kj::str(e.name);
      auto& count = counts.findOrCreate(
          name, [&]() -> decltype(counts)::Entry { return {kj::str(name), 0}; });
      label = kj::str(name, "#", ++count);
    }
    events.add(
        kj::str("init ", label, " trigger=", labelOf(e.trigger), " exec=", labelOf(e.execution)));
    labels.insert(e.id, kj::mv(label));
  }
  void onSettle(uint64_t ctx, AsyncId id, AsyncOutcome outcome, uint64_t atNs) override {
    events.add(kj::str("settle ", labelOf(id), outcome == AsyncOutcome::OK ? "" : " (not ok)"));
  }
  void onBefore(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("before ", labelOf(id)));
  }
  void onAfter(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("after ", labelOf(id)));
  }
  void onDestroy(uint64_t ctx, AsyncId id, uint64_t atNs) override {
    events.add(kj::str("destroy ", labelOf(id)));
  }
  void onAnnotate(uint64_t ctx,
      AsyncId id,
      kj::ArrayPtr<const char> key,
      kj::ArrayPtr<const char> value) override {
    events.add(kj::str("annotate ", labelOf(id), " ", key, "=", value));
  }
  void onTurn(uint64_t ctx, const AsyncTurn& turn) override {
    events.add(kj::str("turn cause=", labelOf(turn.cause)));
  }
  void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& s) override {
    events.add(kj::str("context_end unknown=", s.unknown, " unbalanced=", s.unbalanced,
        " ambiguous=", s.ambiguousBindings, " foreign=", s.foreignThread));
  }

 private:
  kj::Vector<kj::String>& events;
  kj::HashMap<AsyncId, kj::String> labels;
  kj::HashMap<kj::String, uint> counts;

  kj::String labelOf(AsyncId id) {
    if (id == 0) return kj::str("-");
    KJ_IF_SOME(label, labels.find(id)) {
      return kj::str(label);
    }
    return kj::str("?", id);
  }
};

class LabelingObserver final: public IsolateObserver {
 public:
  explicit LabelingObserver(kj::Vector<kj::String>& events): events(events) {}

  kj::Maybe<AsyncTraceConfig> getAsyncTraceConfig() const override {
    return AsyncTraceConfig{};
  }
  void addAsyncTraceSinks(AsyncTraceSinks& sinks,
      kj::StringPtr worker,
      kj::Maybe<kj::StringPtr> actorId) const override {
    sinks.add(kj::heap<LabelingListener>(events));
  }

 private:
  kj::Vector<kj::String>& events;
};

// Runs `fetch` as the module's fetch handler and returns the trace events.
kj::Array<kj::String> traceFetch(kj::StringPtr fetch) {
  kj::Vector<kj::String> events;
  auto source = kj::str("export default { async fetch(request) {\n", fetch, "\n} };");
  {
    TestFixture fixture({
      .mainModuleSource = source.asPtr(),
      .useRealTimers = true,
      .isolateObserver = kj::atomicRefcounted<LabelingObserver>(events),
    });
    auto response = fixture.runRequest(kj::HttpMethod::GET, "http://www.example.com"_kj, ""_kj);
    KJ_EXPECT(response.statusCode == 200, response.body);
    KJ_EXPECT(response.body == "ok", response.body);
  }
  return events.releaseAsArray();
}

// Expects `expected` to appear in `events` in order, not necessarily adjacent.
void expectInOrder(
    kj::ArrayPtr<const kj::String> events, std::initializer_list<kj::StringPtr> expected) {
  size_t next = 0;
  for (auto& e: expected) {
    while (next < events.size() && events[next] != e) ++next;
    if (next == events.size()) {
      KJ_FAIL_EXPECT("missing or out of order", e, joined(events));
      return;
    }
    ++next;
  }
}

uint count(kj::ArrayPtr<const kj::String> events, kj::StringPtr event) {
  uint n = 0;
  for (auto& e: events) {
    if (e == event) ++n;
  }
  return n;
}

void expectComplete(kj::ArrayPtr<const kj::String> events) {
  KJ_EXPECT(count(events, "context_end unknown=0 unbalanced=0 ambiguous=0 foreign=0"_kj) == 1,
      joined(events));
}

KJ_TEST("code after an await on a timer is triggered by that timer") {
  auto events = traceFetch(R"(
    await new Promise(resolve => setTimeout(resolve, 1));
    await new Promise(resolve => setTimeout(resolve, 1));
    return new Response("ok");
  )"_kj);
  expectInOrder(events,
      {
        "init setTimeout#1 trigger=request exec=request"_kj,
        "settle setTimeout#1"_kj,
        "before setTimeout#1"_kj,
        "init setTimeout#2 trigger=setTimeout#1 exec=setTimeout#1"_kj,
        "after setTimeout#1"_kj,
        "turn cause=setTimeout#1"_kj,
        "settle setTimeout#2"_kj,
        "turn cause=setTimeout#2"_kj,
      });
  // The handler's returned promise is awaited from C++.
  expectInOrder(events, {"init awaitJs#1 trigger=request exec=request"_kj, "settle awaitJs#1"_kj});
  expectComplete(events);
}

KJ_TEST("cleared timers and intervals are destroyed; each firing is a turn") {
  auto events = traceFetch(R"(
    clearTimeout(setTimeout(() => {}, 1000000));
    let n = 0;
    await new Promise(resolve => {
      const interval = setInterval(() => {
        if (++n == 3) {
          clearInterval(interval);
          resolve();
        }
      }, 1);
    });
    return new Response("ok");
  )"_kj);
  expectInOrder(events,
      {
        "init setTimeout#1 trigger=request exec=request"_kj,
        "destroy setTimeout#1"_kj,
        "init setInterval#1 trigger=request exec=request"_kj,
        "turn cause=setInterval#1"_kj,
        "turn cause=setInterval#1"_kj,
        "destroy setInterval#1"_kj,
        "turn cause=setInterval#1"_kj,
      });
  KJ_EXPECT(count(events, "turn cause=setInterval#1"_kj) == 3, joined(events));
  KJ_EXPECT(count(events, "settle setInterval#1"_kj) == 0, joined(events));
  KJ_EXPECT(count(events, "settle setTimeout#1"_kj) == 0, joined(events));
  expectComplete(events);
}

KJ_TEST("microtasks nest inside the turn that runs them") {
  auto events = traceFetch(R"(
    await new Promise(resolve => queueMicrotask(resolve));
    queueMicrotask(() => {});
    return new Response("ok");
  )"_kj);
  expectInOrder(events,
      {
        "before request"_kj,
        "init queueMicrotask#1 trigger=request exec=request"_kj,
        "settle queueMicrotask#1"_kj,
        "before queueMicrotask#1"_kj,
        "after queueMicrotask#1"_kj,
        // The code after `await` is a separate promise reaction, not inside the microtask.
        "init queueMicrotask#2 trigger=request exec=request"_kj,
        "before queueMicrotask#2"_kj,
        "after queueMicrotask#2"_kj,
        "after request"_kj,
        "turn cause=request"_kj,
      });
  expectComplete(events);
}

KJ_TEST("code after awaiting I/O is triggered by the awaitIo bridge") {
  auto events = traceFetch(R"(
    await scheduler.wait(1);
    setTimeout(() => {}, 0);
    return new Response("ok");
  )"_kj);
  expectInOrder(events,
      {
        "init scheduler.wait#1 trigger=request exec=request"_kj,
        "init awaitIo#1 trigger=request exec=request"_kj,
        "turn cause=request"_kj,
        "settle scheduler.wait#1"_kj,
        "turn cause=scheduler.wait#1"_kj,
        "settle awaitIo#1"_kj,
        "before awaitIo#1"_kj,
        "init setTimeout#1 trigger=awaitIo#1 exec=awaitIo#1"_kj,
        "after awaitIo#1"_kj,
        "turn cause=awaitIo#1"_kj,
      });
  expectComplete(events);
}

KJ_TEST("a rejected awaitIo settles with an error") {
  auto events = traceFetch(R"(
    const controller = new AbortController();
    const wait = scheduler.wait(1000000, {signal: controller.signal});
    controller.abort();
    try { await wait; } catch {}
    return new Response("ok");
  )"_kj);
  expectInOrder(events,
      {
        "init scheduler.wait#1 trigger=request exec=request"_kj,
        "init awaitIo#1 trigger=request exec=request"_kj,
        "destroy scheduler.wait#1"_kj,
      });
  expectInOrder(events, {"settle awaitIo#1 (not ok)"_kj, "turn cause=awaitIo#1"_kj});
  expectComplete(events);
}

// Runs `callback` in a request's IoContext and returns the trace events.
kj::Array<kj::String> traceInContext(
    kj::Function<kj::Promise<void>(const TestFixture::Environment&)> callback) {
  kj::Vector<kj::String> events;
  {
    TestFixture fixture({
      .useRealTimers = true,
      .isolateObserver = kj::atomicRefcounted<LabelingObserver>(events),
    });
    fixture.runInIoContext([&](const TestFixture::Environment& env) { return callback(env); });
  }
  return events.releaseAsArray();
}

// Awaits `promise` from JavaScript (awaitIo) and back (awaitJs), as a binding call does.
kj::Promise<void> roundTrip(const TestFixture::Environment& env, kj::Promise<void> promise) {
  return env.context.awaitJs(env.js, env.context.awaitIo(env.js, kj::mv(promise)));
}

KJ_TEST("a context's span is recorded by its tracker, whatever turn is innermost") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    TraceContext traceContext;
    {
      // Another (untraced) context's turn, as when a context delivers to another synchronously.
      AsyncTracker::TurnScope other(kj::none);
      KJ_EXPECT(AsyncTracker::inTurn() == kj::none);
      traceContext = env.context.makeUserTraceSpan("kv_get"_kjc);
    }
    return roundTrip(env, kj::evalLater([]() {}).attach(kj::mv(traceContext)));
  });
  expectInOrder(events,
      {
        "init kv_get#1 trigger=request exec=request"_kj,
        "turn cause=kv_get#1"_kj,
      });
  expectComplete(events);
}

KJ_TEST("awaitIo adopts the span's operation") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    auto traceContext = env.context.makeUserTraceSpan("kv_get"_kjc);
    traceContext.setTag("db.key"_kjc, "k"_kjc);
    return roundTrip(env, kj::evalLater([]() {}).attach(kj::mv(traceContext)));
  });
  expectInOrder(events,
      {
        "init kv_get#1 trigger=request exec=request"_kj,
        "annotate kv_get#1 db.key=k"_kj,
        // The span ends with the KJ promise, before JavaScript resumes.
        "settle kv_get#1"_kj,
        "before kv_get#1"_kj,
        "after kv_get#1"_kj,
        "turn cause=kv_get#1"_kj,
      });
  KJ_EXPECT(count(events, "init awaitIo#1 trigger=request exec=request"_kj) == 0, joined(events));
  expectComplete(events);
}

KJ_TEST("a detached span is not adopted") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    auto traceContext = env.context.makeUserTraceSpan("kv_get"_kjc);
    traceContext.detachAsync();
    return roundTrip(env, kj::evalLater([]() {}).attach(kj::mv(traceContext)));
  });
  expectInOrder(events,
      {
        "init kv_get#1 trigger=request exec=request"_kj,
        "init awaitIo#1 trigger=request exec=request"_kj,
        "settle kv_get#1"_kj,
        "settle awaitIo#1"_kj,
        "turn cause=awaitIo#1"_kj,
      });
  expectComplete(events);
}

KJ_TEST("a span that ended synchronously is not adopted") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    { auto phase = env.context.makeUserTraceSpan("fetch_setup"_kjc); }
    return roundTrip(env, kj::evalLater([]() {}));
  });
  expectInOrder(events,
      {
        "init fetch_setup#1 trigger=request exec=request"_kj,
        "settle fetch_setup#1"_kj,
        "init awaitIo#1 trigger=request exec=request"_kj,
        "turn cause=awaitIo#1"_kj,
      });
  expectComplete(events);
}

KJ_TEST("spans attached with attachSpans() are not adopted") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    auto traceContext = env.context.makeUserTraceSpan("durable_object_storage_get"_kjc);
    env.context.attachSpans(env.js, env.js.resolvedPromise(), kj::mv(traceContext));
    return roundTrip(env, kj::evalLater([]() {}));
  });
  expectInOrder(events,
      {
        "init durable_object_storage_get#1 trigger=request exec=request"_kj,
        "init awaitIo#1 trigger=request exec=request"_kj,
        "turn cause=awaitIo#1"_kj,
      });
  expectComplete(events);
}

KJ_TEST("adopting with several eligible spans takes the latest and is counted") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    auto first = env.context.makeUserTraceSpan("first"_kjc);
    auto second = env.context.makeUserTraceSpan("second"_kjc);
    return roundTrip(env, kj::evalLater([]() {}).attach(kj::mv(first), kj::mv(second)));
  });
  expectInOrder(events, {"before second#1"_kj, "turn cause=second#1"_kj});
  KJ_EXPECT(count(events, "context_end unknown=0 unbalanced=0 ambiguous=1 foreign=0"_kj) == 1,
      joined(events));
}

KJ_TEST("overwriting a span settles its operation") {
  auto events = traceInContext([](const TestFixture::Environment& env) {
    auto traceContext = env.context.makeUserTraceSpan("first"_kjc);
    traceContext = env.context.makeUserTraceSpan("second"_kjc);
    return roundTrip(env, kj::evalLater([]() {}).attach(kj::mv(traceContext)));
  });
  expectInOrder(events,
      {
        "init first#1 trigger=request exec=request"_kj,
        "init second#1 trigger=request exec=request"_kj,
        "settle first#1"_kj,
        "turn cause=second#1"_kj,
      });
  KJ_EXPECT(count(events, "destroy first#1"_kj) == 0, joined(events));
  expectComplete(events);
}

}  // namespace
}  // namespace workerd
