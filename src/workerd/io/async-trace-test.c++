// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "async-trace.h"

#include <kj/filesystem.h>
#include <kj/test.h>
#include <kj/thread.h>

#include <cstdlib>

namespace workerd {
namespace {

using kj::uint;

// Records events as strings, without timestamps.
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
        " trigger=", e.trigger, " exec=", e.execution,
        e.stack == 0 ? kj::str() : kj::str(" stack=", e.stack)));
  }
  void onStack(uint64_t isolate, uint32_t id, kj::ArrayPtr<const AsyncStackFrame> frames) override {
    auto lines = KJ_MAP(f, frames) {
      return kj::str(f.function, " ", f.script, "#", f.scriptId, ":", f.line, ":", f.column);
    };
    events.add(kj::str("stack ", id, ": ", kj::strArray(lines, " | ")));
  }
  void onSettle(uint64_t ctx, AsyncId id, AsyncOutcome outcome, uint64_t atNs) override {
    events.add(kj::str("settle ", id, " ", static_cast<uint>(outcome)));
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
  void onRelease(uint64_t ctx, AsyncId id) override {
    events.add(kj::str("release ", id));
  }
  void onAnnotate(uint64_t ctx,
      AsyncId id,
      kj::ArrayPtr<const char> key,
      kj::ArrayPtr<const char> value) override {
    events.add(kj::str("annotate ", id, " ", key, "=", value));
  }
  void onLink(
      uint64_t ctx, uint64_t id, uint64_t fromIsolate, uint64_t fromCtx, uint64_t fromId) override {
    events.add(kj::str("link ", id, " from=", fromId));
  }
  void onTurn(uint64_t ctx, const AsyncTurn& turn) override {
    events.add(kj::str("turn cause=", turn.cause, " locked=", turn.lockedNs != kj::none));
  }
  void onContextEnd(uint64_t ctx, uint64_t atNs, const AsyncContextStats& s) override {
    events.add(kj::str("context_end created=", s.created, " dropped=", s.dropped,
        " unknown=", s.unknown, " unbalanced=", s.unbalanced, " ambiguous=", s.ambiguousBindings,
        " foreign=", s.foreignThread));
  }

 private:
  kj::Vector<kj::String>& events;
};

class ThrowingListener final: public AsyncTraceListener {
 public:
  explicit ThrowingListener(uint& calls): calls(calls) {}
  void onInit(uint64_t ctx, const AsyncInitEvent& e) override {
    ++calls;
    KJ_FAIL_ASSERT("listener failure (expected by test)");
  }

 private:
  uint& calls;
};

struct Fixture {
  AsyncTraceIsolate isolate;
  kj::Vector<kj::String> events;
  kj::Own<OwnedAsyncTracker> owned;

  Fixture(): owned(kj::heap<OwnedAsyncTracker>(makeTracker())) {
    takeEvents();  // Discard context_begin.
  }

  kj::Maybe<kj::Arc<AsyncTracker>> makeTracker() {
    AsyncTraceSinks sinks;
    sinks.add(kj::heap<RecordingListener>(events));
    return AsyncTracker::tryCreate(isolate, kj::mv(sinks), "worker"_kj, kj::none);
  }

  const AsyncTracker& tracker() {
    return KJ_ASSERT_NONNULL(owned->get());
  }

  // As when the IoContext is destroyed.
  void closeTracker() {
    owned = kj::heap<OwnedAsyncTracker>();
  }

  kj::Array<kj::String> takeEvents() {
    auto result = events.releaseAsArray();
    events = kj::Vector<kj::String>();
    return result;
  }
};

kj::String joined(kj::ArrayPtr<const kj::String> events) {
  return kj::strArray(events, "\n");
}

#define KJ_EXPECT_EVENTS(fixture, ...)                                                             \
  do {                                                                                             \
    auto actual_ = (fixture).takeEvents();                                                         \
    kj::String expected_[] = {__VA_ARGS__};                                                        \
    auto expectedStr_ = kj::strArray(kj::arrayPtr(expected_), "\n");                               \
    KJ_EXPECT(joined(actual_) == expectedStr_, joined(actual_), expectedStr_);                     \
  } while (false)

KJ_TEST("inert AsyncResource does nothing") {
  AsyncResource resource;
  KJ_EXPECT(resource.getId() == 0);
  resource.settle(AsyncOutcome::OK);
  resource.annotate("k"_kj, "v"_kj);
  resource.enterAsTurnCause();
  resource.release();
  AsyncTracker::CallbackScope scope(resource);
  AsyncTracker::TurnScope turn(kj::none, 0);
  turn.locked();
}

KJ_TEST("no sinks means no tracker") {
  AsyncTraceIsolate isolate;
  KJ_EXPECT(AsyncTracker::tryCreate(isolate, AsyncTraceSinks(), "w"_kj, kj::none) == kj::none);
}

KJ_TEST("context begin is reported when the tracker is created") {
  AsyncTraceIsolate isolate;
  kj::Vector<kj::String> events;
  AsyncTraceSinks sinks;
  sinks.add(kj::heap<RecordingListener>(events));
  OwnedAsyncTracker owned(AsyncTracker::tryCreate(isolate, kj::mv(sinks), "w"_kj, "actor"_kj));
  KJ_EXPECT(events.size() == 1);
  KJ_EXPECT(events[0] == "context_begin");
}

KJ_TEST("resource lifecycle") {
  Fixture f;
  auto timer = f.tracker().create(AsyncKind::TIMER, "setTimeout"_kj);
  auto bridge = f.tracker().create(AsyncKind::KJ_TO_JS, "bridge"_kj, timer.getId());
  KJ_EXPECT(timer.getId() != 0);
  KJ_EXPECT(bridge.getId() > timer.getId());

  bridge.annotate("url"_kj, "https://example.com/"_kj);
  bridge.settle(AsyncOutcome::CANCELED);
  auto timerId = timer.getId();
  auto bridgeId = bridge.getId();
  // Dropping a settled resource reports only its release; dropping an unsettled one reports
  // destroy first.
  { auto dropped = kj::mv(bridge); }
  timer.release();
  KJ_EXPECT(timer.getId() == 0);

  KJ_EXPECT_EVENTS(f, kj::str("init ", timerId, " kind=3 name=setTimeout trigger=0 exec=0"),
      kj::str("init ", bridgeId, " kind=1 name=bridge trigger=", timerId, " exec=0"),
      kj::str("annotate ", bridgeId, " url=https://example.com/"),
      kj::str("settle ", bridgeId, " 2"), kj::str("release ", bridgeId),
      kj::str("destroy ", timerId), kj::str("release ", timerId));
}

KJ_TEST("moving a resource transfers it") {
  Fixture f;
  auto a = f.tracker().create(AsyncKind::TIMER, "a"_kj);
  auto b = f.tracker().create(AsyncKind::TIMER, "b"_kj);
  auto aId = a.getId();
  auto bId = b.getId();
  f.takeEvents();

  AsyncResource moved = kj::mv(a);
  KJ_EXPECT(a.getId() == 0);  // NOLINT(workerd-use-after-move)
  KJ_EXPECT(moved.getId() == aId);
  a.settle(AsyncOutcome::OK);  // NOLINT(workerd-use-after-move): inert.
  KJ_EXPECT(f.takeEvents().size() == 0);

  // Assigning over a live resource releases it first.
  moved = kj::mv(b);
  KJ_EXPECT(moved.getId() == bId);
  KJ_EXPECT_EVENTS(f, kj::str("destroy ", aId), kj::str("release ", aId));
}

KJ_TEST("turns: default cause, explicit cause, callback scopes") {
  Fixture f;
  auto request = f.tracker().create(AsyncKind::REQUEST, "request"_kj);
  auto bridge = f.tracker().create(AsyncKind::KJ_TO_JS, "bridge"_kj);
  auto r = request.getId();
  auto b = bridge.getId();
  f.takeEvents();

  {
    AsyncTracker::TurnScope turn(f.tracker(), r);
    turn.locked();
    KJ_EXPECT(f.tracker().current() == r);
  }
  // Nothing in the turn used the default cause, so only the turn reports it.
  KJ_EXPECT_EVENTS(f, kj::str("turn cause=", r, " locked=true"));

  AsyncResource microtask;
  {
    AsyncTracker::TurnScope turn(f.tracker(), r);
    bridge.enterAsTurnCause();
    KJ_EXPECT(f.tracker().current() == b);
    microtask = f.tracker().create(AsyncKind::MICROTASK, "queueMicrotask"_kj);
    {
      AsyncTracker::CallbackScope scope(microtask);
      KJ_EXPECT(f.tracker().current() == microtask.getId());
    }
    KJ_EXPECT(f.tracker().current() == b);
  }
  auto m = microtask.getId();
  KJ_EXPECT_EVENTS(f, kj::str("before ", b),
      kj::str("init ", m, " kind=4 name=queueMicrotask trigger=", b, " exec=", b),
      kj::str("before ", m), kj::str("after ", m), kj::str("after ", b),
      kj::str("turn cause=", b, " locked=false"));
}

KJ_TEST("a bridge adopts a pending operation from the same turn") {
  Fixture f;
  AsyncResource op;
  AsyncResource bridge;
  AsyncResource unrelated;
  {
    AsyncTracker::TurnScope turn(f.tracker(), 0);
    op = f.tracker().create(AsyncKind::OPERATION, "kv_get"_kj);
    bridge = f.tracker().adoptOrCreate(AsyncKind::KJ_TO_JS, "awaitIo"_kj);
    // Nothing left to adopt: a new bridge resource.
    unrelated = f.tracker().adoptOrCreate(AsyncKind::KJ_TO_JS, "awaitIo"_kj);
  }
  KJ_EXPECT(bridge.getId() == op.getId());
  KJ_EXPECT(unrelated.getId() != op.getId());
  auto id = op.getId();
  auto other = unrelated.getId();
  f.takeEvents();

  // The span ends first; the bridge still reports under the operation.
  op.settle(AsyncOutcome::OK);
  op.release();
  bridge.settle(AsyncOutcome::OK);
  {
    AsyncTracker::TurnScope turn(f.tracker(), 0);
    bridge.enterAsTurnCause();
  }
  bridge.release();
  unrelated.release();
  KJ_EXPECT_EVENTS(f, kj::str("settle ", id, " 0"), kj::str("before ", id), kj::str("after ", id),
      kj::str("turn cause=", id, " locked=false"), kj::str("release ", id),
      kj::str("destroy ", other), kj::str("release ", other));
}

KJ_TEST("inTurn() is the tracker of the innermost turn") {
  Fixture f;
  KJ_EXPECT(AsyncTracker::inTurn() == kj::none);
  {
    AsyncTracker::TurnScope outer(f.tracker(), 0);
    KJ_EXPECT(&KJ_ASSERT_NONNULL(AsyncTracker::inTurn()) == &f.tracker());
    {
      // A nested untraced turn (another context) hides the outer tracker.
      AsyncTracker::TurnScope inner(kj::none, 0);
      KJ_EXPECT(AsyncTracker::inTurn() == kj::none);
    }
    KJ_EXPECT(&KJ_ASSERT_NONNULL(AsyncTracker::inTurn()) == &f.tracker());
  }
  KJ_EXPECT(AsyncTracker::inTurn() == kj::none);
}

// Reports a fixed two-frame stack, counting captures.
class FakeStackCapturer final: public AsyncStackCapturer {
 public:
  explicit FakeStackCapturer(uint& captures): captures(captures) {}

  void capture(AsyncStackBuilder& builder) const override {
    ++captures;
    builder.addFrame("inner"_kj.asArray(), "worker.js"_kj.asArray(), 7, 3, 5);
    builder.addFrame(""_kj.asArray(), "worker.js"_kj.asArray(), 7, 10, 1);
  }

 private:
  uint& captures;
};

KJ_TEST("creation stacks are captured, deduplicated, and reported once per tracker") {
  uint captures = 0;
  AsyncTraceIsolate isolate(kj::arc<FakeStackCapturer>(captures));
  kj::Vector<kj::String> events;
  auto makeTracker = [&]() {
    AsyncTraceSinks sinks;
    sinks.add(kj::heap<RecordingListener>(events));
    return KJ_ASSERT_NONNULL(
        AsyncTracker::tryCreate(isolate, kj::mv(sinks), "worker"_kj, kj::none));
  };
  auto takeEvents = [&]() {
    auto result = events.releaseAsArray();
    events = kj::Vector<kj::String>();
    return result;
  };

  auto first = makeTracker();
  auto keep = first.addRef();
  auto owned = kj::heap<OwnedAsyncTracker>(kj::mv(first));
  auto a = keep->create(AsyncKind::TIMER, "setTimeout"_kj);
  auto b = keep->create(AsyncKind::TIMER, "setTimeout"_kj);
  auto sameStack = kj::str("stack 1: inner worker.js#7:3:5 |  worker.js#7:10:1");
  {
    auto actual = takeEvents();
    kj::String expected[] = {kj::str("context_begin"), kj::str(sameStack),
      kj::str("init ", a.getId(), " kind=3 name=setTimeout trigger=0 exec=0 stack=1"),
      kj::str("init ", b.getId(), " kind=3 name=setTimeout trigger=0 exec=0 stack=1")};
    KJ_EXPECT(joined(actual) == joined(expected), joined(actual));
  }
  KJ_EXPECT(captures == 2);

  // Another tracker on the same isolate shares the stack's ID, and reports it to its own sinks.
  auto second = makeTracker();
  auto c = second->create(AsyncKind::MICROTASK, "queueMicrotask"_kj);
  {
    auto actual = takeEvents();
    kj::String expected[] = {kj::str("context_begin"), kj::str(sameStack),
      kj::str("init ", c.getId(), " kind=4 name=queueMicrotask trigger=0 exec=0 stack=1")};
    KJ_EXPECT(joined(actual) == joined(expected), joined(actual));
  }
  KJ_EXPECT(captures == 3);

  // A closed tracker records nothing, so it doesn't capture.
  owned = nullptr;
  auto late = keep->create(AsyncKind::TIMER, "setTimeout"_kj);
  KJ_EXPECT(late.getId() == 0);
  KJ_EXPECT(captures == 3);
}

// Reports an `outer` frame on its first capture and a `nested` frame after that, so that a test
// can tell the stacks of nested captures apart.
class SequenceStackCapturer final: public AsyncStackCapturer {
 public:
  void capture(AsyncStackBuilder& builder) const override {
    if (captures++ == 0) {
      builder.addFrame("outer"_kj.asArray(), "outer.js"_kj.asArray(), 7, 3, 5);
    } else {
      builder.addFrame("nested"_kj.asArray(), "nested.js"_kj.asArray(), 8, 3, 5);
    }
  }

 private:
  mutable uint captures = 0;
};

// Creates a resource in another tracker before recording the stack it was given. With stack
// capture on, the other tracker reports its own stacks in the middle of this callback.
class NestingStackListener final: public AsyncTraceListener {
 public:
  NestingStackListener(kj::Vector<kj::String>& events, kj::Maybe<kj::Arc<AsyncTracker>>& other)
      : events(events),
        other(other) {}

  void onInit(uint64_t ctx, const AsyncInitEvent& e) override {}
  void onStack(uint64_t isolate, uint32_t id, kj::ArrayPtr<const AsyncStackFrame> frames) override {
    KJ_IF_SOME(tracker, other) {
      tracker->create(AsyncKind::TIMER, "nested"_kj);
    }
    auto lines = KJ_MAP(f, frames) {
      return kj::str(f.function, " ", f.script, "#", f.scriptId, ":", f.line, ":", f.column);
    };
    events.add(kj::str("stack ", id, ": ", kj::strArray(lines, " | ")));
  }

 private:
  kj::Vector<kj::String>& events;
  kj::Maybe<kj::Arc<AsyncTracker>>& other;
};

KJ_TEST("a stack stays valid while its listener causes another tracker to report one") {
  AsyncTraceIsolate isolate(kj::arc<SequenceStackCapturer>());
  kj::Vector<kj::String> outerEvents;
  kj::Vector<kj::String> innerEvents;

  AsyncTraceSinks innerSinks;
  innerSinks.add(kj::heap<RecordingListener>(innerEvents));
  kj::Maybe<kj::Arc<AsyncTracker>> inner =
      AsyncTracker::tryCreate(isolate, kj::mv(innerSinks), "inner"_kj, kj::none);
  OwnedAsyncTracker ownedInner(KJ_ASSERT_NONNULL(inner).addRef());

  AsyncTraceSinks outerSinks;
  outerSinks.add(kj::heap<NestingStackListener>(outerEvents, inner));
  OwnedAsyncTracker outer(
      AsyncTracker::tryCreate(isolate, kj::mv(outerSinks), "outer"_kj, kj::none));

  auto timer = KJ_ASSERT_NONNULL(outer.get()).create(AsyncKind::TIMER, "setTimeout"_kj);
  // The outer listener reads its frames after the nested report, and still sees its own.
  KJ_EXPECT(joined(outerEvents) == "stack 1: outer outer.js#7:3:5", joined(outerEvents));
  KJ_EXPECT(joined(innerEvents).contains("stack 2: nested nested.js#8:3:5"), joined(innerEvents));
  KJ_EXPECT(!joined(innerEvents).contains("outer"), joined(innerEvents));
}

KJ_TEST("a resource created during another context's turn links to it") {
  Fixture caller;
  Fixture callee;
  auto request = callee.tracker().create(AsyncKind::REQUEST, "fetch"_kj);
  callee.takeEvents();

  // Outside any turn: nothing to link to.
  callee.tracker().linkFromCallerTurn(request.getId());
  KJ_EXPECT(callee.takeEvents().size() == 0);

  {
    AsyncTracker::TurnScope turn(caller.tracker());
    auto timer = caller.tracker().create(AsyncKind::TIMER, "setTimeout"_kj);
    auto fetch = caller.tracker().create(AsyncKind::OPERATION, "fetch"_kj);
    callee.tracker().linkFromCallerTurn(request.getId());
    KJ_EXPECT_EVENTS(callee, kj::str("link ", request.getId(), " from=", fetch.getId()));

    // A context's own turn is not a caller.
    caller.tracker().linkFromCallerTurn(timer.getId());
    KJ_EXPECT(!joined(caller.takeEvents()).contains("link"_kj));
  }
}

KJ_TEST("closing reports stats and makes later events no-ops") {
  AsyncTraceIsolate isolate;
  kj::Vector<kj::String> events;
  AsyncResource survivor;
  {
    AsyncTraceSinks sinks;
    sinks.add(kj::heap<RecordingListener>(events));
    OwnedAsyncTracker owned(AsyncTracker::tryCreate(isolate, kj::mv(sinks), "w"_kj, kj::none));
    survivor = KJ_ASSERT_NONNULL(owned.get()).create(AsyncKind::TIMER, "t"_kj);
    events.clear();
  }
  // The handle outlives the IoContext's reference. The tracker closed, destroying its sinks.
  KJ_EXPECT(events.size() == 2, kj::strArray(events, "\n"));
  KJ_EXPECT(
      events[0] == "context_end created=1 dropped=0 unknown=0 unbalanced=0 ambiguous=0 foreign=0");
  KJ_EXPECT(events[1] == "listener destroyed");

  survivor.settle(AsyncOutcome::OK);
  survivor.release();
  KJ_EXPECT(events.size() == 2);
}

KJ_TEST("calls from another thread are dropped and counted") {
  Fixture f;
  auto resource = f.tracker().create(AsyncKind::TIMER, "t"_kj);
  f.takeEvents();
  auto& tracker = f.tracker();
  AsyncId fromOtherThread = 1;
  {
    kj::Thread thread([&]() {
      fromOtherThread = tracker.create(AsyncKind::TIMER, "elsewhere"_kj).getId();
      resource.settle(AsyncOutcome::OK);
      tracker.current();
    });
  }
  KJ_EXPECT(fromOtherThread == 0);
  KJ_EXPECT(f.takeEvents().size() == 0);
  resource.release();
  f.closeTracker();
  auto events = f.takeEvents();
  // destroy, release, context_end, listener destroyed.
  KJ_EXPECT(events.size() == 4, joined(events));
  KJ_EXPECT(
      events[2] == "context_end created=1 dropped=0 unknown=0 unbalanced=0 ambiguous=0 foreign=3");
}

KJ_TEST("a throwing listener does not stop other sinks") {
  AsyncTraceIsolate isolate;
  kj::Vector<kj::String> events;
  uint throwingCalls = 0;
  AsyncTraceSinks sinks;
  sinks.add(kj::heap<ThrowingListener>(throwingCalls));
  sinks.add(kj::heap<RecordingListener>(events));
  OwnedAsyncTracker owned(AsyncTracker::tryCreate(isolate, kj::mv(sinks), "w"_kj, kj::none));
  auto& tracker = KJ_ASSERT_NONNULL(owned.get());
  events.clear();

  KJ_EXPECT_LOG(ERROR, "listener failure (expected by test)");
  auto a = tracker.create(AsyncKind::TIMER, "a"_kj);
  KJ_EXPECT(a.getId() != 0);
  KJ_EXPECT(throwingCalls == 1);
  KJ_EXPECT(events.size() == 1);
}

KJ_TEST("invalid UTF-8 is replaced, not thrown") {
  Fixture f;
  auto r = f.tracker().create(AsyncKind::OPERATION, "kv\xff"_kj);
  r.annotate("k"_kj, "\xfe"_kj);
  auto events = f.takeEvents();
  KJ_EXPECT(events.size() == 2, joined(events));
  KJ_EXPECT(events[0].contains("name=kv\xef\xbf\xbd"), events[0]);
  KJ_EXPECT(events[1].endsWith("k=\xef\xbf\xbd"), events[1]);
}

// A native path (such as C:/... on Windows) in the test's temporary directory.
kj::String tempPath(kj::StringPtr name) {
  const char* dir = getenv("TEST_TMPDIR");
  KJ_REQUIRE(dir != nullptr, "TEST_TMPDIR not set");
  return kj::str(dir, "/", name);
}

KJ_TEST("NDJSON sink writes the event log") {
  auto pathStr = tempPath("async-trace-test.ndjson");
  {
    auto writer = AsyncTraceWriter::open(pathStr, "test-version"_kj);
    AsyncTraceIsolate isolate;
    AsyncTraceSinks sinks;
    sinks.addNdjson(*writer);
    OwnedAsyncTracker owned(AsyncTracker::tryCreate(isolate, kj::mv(sinks), "w"_kj, kj::none));
    auto& tracker = KJ_ASSERT_NONNULL(owned.get());
    {
      AsyncTracker::TurnScope turn(tracker, 0);
      auto timer = tracker.create(AsyncKind::TIMER, "setTimeout"_kj);
    }
    KJ_EXPECT(!writer->failed());
  }

  auto fs = kj::newDiskFilesystem();
  auto path = fs->getCurrentPath().evalNative(pathStr);
  auto text = fs->getRoot().openFile(path)->readAllText();
  KJ_EXPECT(text.startsWith("{\"e\":\"header\",\"v\":1,\"producer\":\"workerd\","), text);
  KJ_EXPECT(text.contains("\"version\":\"test-version\""), text);
  KJ_EXPECT(text.contains("\"e\":\"ctx\""), text);
  KJ_EXPECT(text.contains("\"kind\":\"timer\",\"name\":\"setTimeout\""), text);
  KJ_EXPECT(text.contains("\"e\":\"destroy\""), text);
  KJ_EXPECT(text.contains("\"e\":\"turn\""), text);
  KJ_EXPECT(text.contains("\"e\":\"ctx_end\""), text);
  KJ_EXPECT(text.endsWith("\n"), text);
}

KJ_TEST("opening an NDJSON writer at a bad path throws") {
  // The OS's wording differs by platform; the message names the path on all of them.
  KJ_EXPECT_THROW_MESSAGE("/nonexistent-dir/x/y.ndjson",
      AsyncTraceWriter::open("/nonexistent-dir/x/y.ndjson"_kj, "v"_kj));
}

}  // namespace
}  // namespace workerd
