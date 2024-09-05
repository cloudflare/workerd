// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include <kj/test.h>
#include <kj/debug.h>
#include "io-gate.h"
#include <workerd/util/capnp-mock.h>
#include <workerd/util/test.h>

namespace workerd {
namespace {

static constexpr kj::Date oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date twoMs = 2 * kj::MILLISECONDS + kj::UNIX_EPOCH;

template <typename T>
kj::Promise<T> eagerlyReportExceptions(kj::Promise<T> promise, kj::SourceLocation location = {}) {
  return promise.eagerlyEvaluate([location](kj::Exception&& e) -> T {
    KJ_LOG_AT(ERROR, location, e);
    kj::throwFatalException(kj::mv(e));
  });
}

// Expect that a synchronous result is returned.
template <typename T>
T expectSync(kj::OneOf<T, kj::Promise<T>> result, kj::SourceLocation location = {}) {
  KJ_SWITCH_ONEOF(result) {
    KJ_CASE_ONEOF(promise, kj::Promise<T>) {
      KJ_FAIL_ASSERT_AT(location, "result was unexpectedly asynchronous");
    }
    KJ_CASE_ONEOF(value, T) {
      return kj::mv(value);
    }
  }
  KJ_UNREACHABLE;
}

struct ActorSqliteTestOptions final {
  bool monitorOutputGate = true;
};

class ActorSqliteTestHooks;

struct ActorSqliteTest final {
  kj::EventLoop loop;
  kj::WaitScope ws;

  OutputGate gate;
  kj::Own<const kj::Directory> vfsDir;
  SqliteDatabase::Vfs vfs;
  SqliteDatabase db;

  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> commitFulfillers;
  kj::Vector<kj::String> calls;

  class ActorSqliteTestHooks final: public ActorSqlite::Hooks {
  public:
    ActorSqliteTestHooks(ActorSqliteTest& parent): parent(parent) {}

    kj::Promise<kj::Maybe<kj::Date>> getAlarm() override {
      parent.calls.add(kj::str("getAlarm"));
      return kj::Maybe<kj::Date>(kj::none);
    }

    kj::Promise<void> setAlarm(kj::Maybe<kj::Date> newAlarmTime) override {
      auto time = newAlarmTime.map([](auto& t) { return kj::str(t); }).orDefault(kj::str("none"));
      parent.calls.add(kj::str("setAlarm(", time, ")"));
      return kj::READY_NOW;
    }

  private:
    ActorSqliteTest& parent;
  };
  ActorSqliteTestHooks hooks = ActorSqliteTestHooks(*this);

  ActorSqlite actor;

  kj::Promise<void> gateBrokenPromise;
  kj::UnwindDetector unwindDetector;

  ActorSqliteTest(ActorSqliteTestOptions options = {})
      : ws(loop),
        vfsDir(kj::newInMemoryDirectory(kj::nullClock())),
        vfs(*vfsDir),
        db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY),
        actor(kj::attachRef(db), gate, KJ_BIND_METHOD(*this, commitCallback), hooks),
        gateBrokenPromise(options.monitorOutputGate ? eagerlyReportExceptions(gate.onBroken())
                                                    : kj::Promise<void>(kj::READY_NOW)) {}

  ~ActorSqliteTest() noexcept(false) {
    // Make sure if the output gate has been broken, the exception was reported. This is important
    // to report errors thrown inside flush(), since those won't otherwise propagate into the test
    // body.
    if (!unwindDetector.isUnwinding()) {
      gateBrokenPromise.poll(ws);
      expectCalls({}, "unexpected calls at end of test");
    }
  }

  kj::Promise<void> commitCallback() {
    auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
    calls.add(kj::str("commit"));
    commitFulfillers.add(kj::mv(fulfiller));
    return kj::mv(promise);
  }

  void resolveCommits(int expectedCount, kj::SourceLocation location = {}) {
    ws.poll();
    KJ_ASSERT_AT(expectedCount == commitFulfillers.size(), location);
    auto fulfillers = kj::mv(commitFulfillers);
    for (auto& f: fulfillers) {
      f->fulfill();
    }
  }

  void rejectCommits(int expectedCount, kj::SourceLocation location = {}) {
    ws.poll();
    KJ_ASSERT_AT(expectedCount == commitFulfillers.size(), location);
    auto fulfillers = kj::mv(commitFulfillers);
    for (auto& f: fulfillers) {
      f->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));
    }
  }

  void expectCalls(std::initializer_list<kj::StringPtr> expCalls,
      kj::StringPtr message = ""_kj,
      kj::SourceLocation location = {}) {
    KJ_ASSERT_AT(calls == heapArray(expCalls), location, kj::str(message));
    calls.clear();
  }

  // A few driver methods for convenience.
  auto getAlarm(ActorCache::ReadOptions options = {}) {
    return actor.getAlarm(options);
  }
  auto put(kj::StringPtr key, kj::StringPtr value, ActorCache::WriteOptions options = {}) {
    return actor.put(kj::str(key), kj::heapArray(value.asBytes()), options);
  }
  auto setAlarm(kj::Maybe<kj::Date> newTime, ActorCache::WriteOptions options = {}) {
    return actor.setAlarm(newTime, options);
  }
};

KJ_TEST("initial alarm value is unset") {
  ActorSqliteTest test;

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == kj::none);
  test.resolveCommits(0);
}

KJ_TEST("can set and get alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == oneMs);
  test.resolveCommits(0);
}

KJ_TEST("alarm write happens transactionally with storage ops") {
  ActorSqliteTest test;

  // TODO(test): This probably isn't actually testing transactionality yet?  But pretty sure it's
  // still transactional under the hood:
  test.setAlarm(oneMs);
  test.put("foo", "bar");
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == oneMs);
  test.resolveCommits(0);
}

KJ_TEST("can clear alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  auto initTime = expectSync(test.getAlarm());
  KJ_ASSERT(initTime == oneMs);
  test.resolveCommits(0);

  test.setAlarm(kj::none);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(none)", "commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == kj::none);
}

KJ_TEST("can set alarm twice") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.setAlarm(twoMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(2ms)", "commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == twoMs);
  test.resolveCommits(0);
}

KJ_TEST("setting duplicate alarm is no-op") {
  ActorSqliteTest test;

  test.setAlarm(kj::none);
  test.resolveCommits(0);

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  test.setAlarm(oneMs);
  test.resolveCommits(0);
}

KJ_TEST("tells alarm handler to cancel when committed alarm is empty") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  test.setAlarm(kj::none);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(none)", "commit"});
  test.ws.poll();  // needs additional poll?

  KJ_ASSERT(test.actor.armAlarmHandler(oneMs, false) == kj::none);
  test.resolveCommits(0);
}

KJ_TEST("tells alarm handler to cancel when committed alarm does not match requested alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});
  test.ws.poll();  // needs additional poll?

  KJ_ASSERT(test.actor.armAlarmHandler(twoMs, false) == kj::none);
  test.resolveCommits(0);
}

KJ_TEST("dirty alarm during handler does not cancel alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});
  test.setAlarm(twoMs);
  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(2ms)", "commit"});
}

KJ_TEST("getAlarm() returns null during handler") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.resolveCommits(0);

    auto time = expectSync(test.getAlarm());
    KJ_ASSERT(time == kj::none);
  }
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(none)", "commit"});
}

KJ_TEST("alarm handler handle clears alarm when dropped with no writes") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(none)", "commit"});
  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == kj::none);
}

KJ_TEST("alarm handler handle does not clear alarm when dropped with writes") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.setAlarm(twoMs);
  }
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(2ms)", "commit"});
  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == twoMs);
}

KJ_TEST("can cancel deferred alarm deletion during handler") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.actor.cancelDeferredAlarmDeletion();
  }
  test.resolveCommits(0);

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == oneMs);
}

KJ_TEST("canceling deferred alarm deletion outside handler has no effect") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.resolveCommits(1);
  test.actor.cancelDeferredAlarmDeletion();
  test.expectCalls({"setAlarm(none)", "commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == kj::none);
}

KJ_TEST("canceling deferred alarm deletion outside handler edge case") {
  // Presumably harmless to cancel deletion if the client requests it after the handler ends but
  // before the event loop runs the commit code?  Trying to cancel deletion outside the handler is
  // a bit of a contract violation anyway -- maybe we should just assert against it?
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  { auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false)); }
  test.actor.cancelDeferredAlarmDeletion();
  test.resolveCommits(1);
  test.expectCalls({"commit"});

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == kj::none);
}

KJ_TEST("canceling deferred alarm deletion is idempotent") {
  ActorSqliteTest test;

  // Not sure if important, but matches ActorCache behavior.
  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});

  {
    auto maybeWrite = KJ_ASSERT_NONNULL(test.actor.armAlarmHandler(oneMs, false));
    test.actor.cancelDeferredAlarmDeletion();
    test.actor.cancelDeferredAlarmDeletion();
  }
  test.resolveCommits(0);

  auto time = expectSync(test.getAlarm());
  KJ_ASSERT(time == oneMs);
}

KJ_TEST("handler alarm is not deleted when commit fails") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  test.setAlarm(oneMs);
  test.resolveCommits(1);
  test.expectCalls({"setAlarm(1ms)", "commit"});
  {
    auto time = expectSync(test.getAlarm());
    KJ_ASSERT(time == oneMs);
  }

  {
    auto handle = test.actor.armAlarmHandler(oneMs, false);

    auto time = expectSync(test.getAlarm());
    KJ_ASSERT(time == kj::none);
  }
  // TODO(soon): shouldn't call setAlarm to clear on rejected commit?  Or OK to assume client will
  // detect and call cancelDeferredAlarmDeletion() on failure?
  test.rejectCommits(1);
  test.expectCalls({"setAlarm(none)", "commit"});

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));
}

KJ_TEST("getAlarm/setAlarm check for brokenness") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Break gate
  test.put("foo", "bar");
  test.expectCalls({});
  test.rejectCommits(1);
  test.expectCalls({"commit"});

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));

  // Apparently we don't actually set brokenness until the taskFailed handler runs, but presumably
  // this is OK?
  test.getAlarm();

  // Ensure taskFailed handler runs and notices brokenness:
  test.ws.poll();

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.getAlarm());
  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.setAlarm(kj::none));
  test.expectCalls({});
}

}  // namespace
}  // namespace workerd
