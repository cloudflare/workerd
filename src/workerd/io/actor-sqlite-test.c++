// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include "io-gate.h"

#include <workerd/util/capnp-mock.h>
#include <workerd/util/test.h>

#include <kj/debug.h>
#include <kj/test.h>

namespace workerd {
namespace {

static constexpr kj::Date oneMs = 1 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date twoMs = 2 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date threeMs = 3 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date fourMs = 4 * kj::MILLISECONDS + kj::UNIX_EPOCH;
static constexpr kj::Date fiveMs = 5 * kj::MILLISECONDS + kj::UNIX_EPOCH;

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

struct ActorSqliteTest final {
  kj::EventLoop loop;
  kj::WaitScope ws;

  OutputGate gate;
  kj::Own<const kj::Directory> vfsDir;
  SqliteDatabase::Vfs vfs;
  SqliteDatabase db;

  struct Call final {
    kj::String desc;
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  };
  kj::Vector<Call> calls;

  struct ActorSqliteTestHooks final: public ActorSqlite::Hooks {
  public:
    explicit ActorSqliteTestHooks(ActorSqliteTest& parent): parent(parent) {}

    kj::Promise<void> scheduleRun(kj::Maybe<kj::Date> newAlarmTime) override {
      KJ_IF_SOME(h, parent.scheduleRunHandler) {
        return h(newAlarmTime);
      }
      auto desc = newAlarmTime.map([](auto& t) {
        return kj::str("scheduleRun(", t, ")");
      }).orDefault(kj::str("scheduleRun(none)"));
      auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
      parent.calls.add(Call{kj::mv(desc), kj::mv(fulfiller)});
      return kj::mv(promise);
    }

    ActorSqliteTest& parent;
  };
  kj::Maybe<kj::Function<kj::Promise<void>(kj::Maybe<kj::Date>)>> scheduleRunHandler;
  ActorSqliteTestHooks hooks = ActorSqliteTestHooks(*this);

  ActorSqlite actor;

  kj::Promise<void> gateBrokenPromise;
  kj::UnwindDetector unwindDetector;

  explicit ActorSqliteTest(ActorSqliteTestOptions options = {})
      : ws(loop),
        vfsDir(kj::newInMemoryDirectory(kj::nullClock())),
        vfs(*vfsDir),
        db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY),
        actor(kj::attachRef(db), gate, KJ_BIND_METHOD(*this, commitCallback), hooks),
        gateBrokenPromise(options.monitorOutputGate ? eagerlyReportExceptions(gate.onBroken())
                                                    : kj::Promise<void>(kj::READY_NOW)) {}

  ~ActorSqliteTest() noexcept(false) {
    if (!unwindDetector.isUnwinding()) {
      // Make sure if the output gate has been broken, the exception was reported. This is
      // important to report errors thrown inside flush(), since those won't otherwise propagate
      // into the test body.
      gateBrokenPromise.poll(ws);

      // Make sure there's no outstanding async work we haven't considered:
      pollAndExpectCalls({}, "unexpected calls at end of test");
    }
  }

  kj::Promise<void> commitCallback() {
    auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
    calls.add(Call{kj::str("commit"), kj::mv(fulfiller)});
    return kj::mv(promise);
  }

  // Polls the event loop, then asserts that the description of calls up to this point match the
  // expectation and returns their fulfillers.  Also clears the call log.
  //
  // TODO(cleanup): Is there a better way to do mocks?  capnp-mock looks nice, but seems a bit
  // heavyweight for this test.
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> pollAndExpectCalls(
      std::initializer_list<kj::StringPtr> expCallDescs,
      kj::StringPtr message = ""_kj,
      kj::SourceLocation location = {}) {
    ws.poll();
    auto callDescs = KJ_MAP(c, calls) { return kj::str(c.desc); };
    KJ_ASSERT_AT(callDescs == heapArray(expCallDescs), location, kj::str(message));
    auto fulfillers = KJ_MAP(c, calls) { return kj::mv(c.fulfiller); };
    calls.clear();
    return kj::mv(fulfillers);
  }

  // A few driver methods for convenience.
  auto get(kj::StringPtr key, ActorCache::ReadOptions options = {}) {
    return actor.get(kj::str(key), options);
  }
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

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("can set and get alarm") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm write happens transactionally with storage ops") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.put("foo", "bar");
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
  KJ_ASSERT(KJ_ASSERT_NONNULL(expectSync(test.get("foo"))) == kj::str("bar").asBytes());
}

KJ_TEST("storage op without alarm change does not wait on scheduler") {
  ActorSqliteTest test;

  test.put("foo", "bar");
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(KJ_ASSERT_NONNULL(expectSync(test.get("foo"))) == kj::str("bar").asBytes());
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("alarm scheduling starts synchronously before implicit local db commit") {
  ActorSqliteTest test;

  // In workerd (unlike edgeworker), there is no remote storage, so there is no work done in
  // commitCallback(); the local db is considered durably stored after the synchronous sqlite
  // commit() call returns.  If a commit includes an alarm state change that requires scheduling
  // before the commit call, it needs to happen synchronously.  Since workerd synchronously
  // schedules alarms, we just need to ensure that the database is in a pre-commit state when
  // scheduleRun() is called.

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;

    KJ_EXPECT_THROW_MESSAGE(
        "cannot start a transaction within a transaction", test.db.run("BEGIN TRANSACTION"));

    return kj::READY_NOW;
  };

  test.setAlarm(oneMs);
  KJ_ASSERT(!startedScheduleRun);
  test.ws.poll();
  KJ_ASSERT(startedScheduleRun);

  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm scheduling starts synchronously before explicit local db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;

    // Not sure if there is a good way to detect savepoint presence without mutating the db state,
    // but this is sufficient to verify the test properties:

    // Verify that we are not within a nested savepoint.
    KJ_EXPECT_THROW_MESSAGE(
        "no such savepoint: _cf_savepoint_1", test.db.run("RELEASE _cf_savepoint_1"));

    // Verify that we are within the root savepoint.
    test.db.run("RELEASE _cf_savepoint_0");
    KJ_EXPECT_THROW_MESSAGE(
        "no such savepoint: _cf_savepoint_0", test.db.run("RELEASE _cf_savepoint_0"));

    // We don't actually care what happens in the test after this point, but it's slightly simpler
    // to readd the savepoint to allow the test to complete cleanly:
    test.db.run("SAVEPOINT _cf_savepoint_0");

    return kj::READY_NOW;
  };

  {
    auto txn = test.actor.startTransaction();
    txn->setAlarm(oneMs, {});

    KJ_ASSERT(!startedScheduleRun);
    txn->commit();
    KJ_ASSERT(startedScheduleRun);

    test.pollAndExpectCalls({"commit"})[0]->fulfill();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("alarm scheduling does not start synchronously before nested explicit local db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;
    return kj::READY_NOW;
  };

  {
    auto txn1 = test.actor.startTransaction();

    {
      auto txn2 = test.actor.startTransaction();
      txn2->setAlarm(oneMs, {});

      txn2->commit();
      KJ_ASSERT(!startedScheduleRun);
    }

    txn1->commit();
    KJ_ASSERT(startedScheduleRun);

    test.pollAndExpectCalls({"commit"})[0]->fulfill();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("synchronous alarm scheduling failure causes local db commit to throw synchronously") {
  ActorSqliteTest test({.monitorOutputGate = false});
  auto promise = test.gate.onBroken();

  auto getLocalAlarm = [&]() -> kj::Maybe<kj::Date> {
    auto query = test.db.run("SELECT value FROM _cf_METADATA WHERE key = 1");
    if (query.isDone() || query.isNull(0)) {
      return kj::none;
    } else {
      return kj::UNIX_EPOCH + query.getInt64(0) * kj::NANOSECONDS;
    }
  };

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});

  // Override scheduleRun handler with one that throws synchronously.
  bool startedScheduleRun = false;
  test.scheduleRunHandler = [&](kj::Maybe<kj::Date>) -> kj::Promise<void> {
    startedScheduleRun = true;
    // Must throw synchronously; returning an exception is insufficient.
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "a_sync_fail"));
  };

  KJ_ASSERT(!promise.poll(test.ws));
  test.setAlarm(oneMs);

  // Expect that polling will attempt to commit the implicit transaction, which should
  // synchronously fail when attempting to call scheduleRun() before the db commit, and roll back the
  // local db state to the 2ms alarm.
  KJ_ASSERT(!startedScheduleRun);
  KJ_ASSERT(KJ_REQUIRE_NONNULL(getLocalAlarm()) == oneMs);
  test.ws.poll();
  KJ_ASSERT(startedScheduleRun);
  KJ_ASSERT(KJ_REQUIRE_NONNULL(getLocalAlarm()) == twoMs);

  KJ_ASSERT(promise.poll(test.ws));
  KJ_EXPECT_THROW_MESSAGE("a_sync_fail", promise.wait(test.ws));
}

KJ_TEST("can clear alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("can set alarm twice") {
  ActorSqliteTest test;

  test.setAlarm(oneMs);
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("setting duplicate alarm is no-op") {
  ActorSqliteTest test;

  test.setAlarm(kj::none);
  test.pollAndExpectCalls({});

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({});
}

KJ_TEST("tells alarm handler to cancel when committed alarm is empty") {
  ActorSqliteTest test;

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorCache::CancelAlarmHandler>());
    auto waitPromise = kj::mv(armResult.get<ActorCache::CancelAlarmHandler>().waitBeforeCancel);
    KJ_ASSERT(waitPromise.poll(test.ws));
    waitPromise.wait(test.ws);
  }
}

KJ_TEST("tells alarm handler to cancel when handler alarm is later than committed alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Request handler run at 2ms.  Expect cancellation without rescheduling.
  auto armResult = test.actor.armAlarmHandler(twoMs, false);
  KJ_ASSERT(armResult.is<ActorSqlite::CancelAlarmHandler>());
  auto cancelResult = kj::mv(armResult.get<ActorSqlite::CancelAlarmHandler>());
  KJ_ASSERT(cancelResult.waitBeforeCancel.poll(test.ws));
  cancelResult.waitBeforeCancel.wait(test.ws);
}

KJ_TEST("tells alarm handler to reschedule when handler alarm is earlier than committed alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  // Expect that armAlarmHandler() tells caller to cancel after rescheduling completes.
  auto armResult = test.actor.armAlarmHandler(oneMs, false);
  KJ_ASSERT(armResult.is<ActorSqlite::CancelAlarmHandler>());
  auto cancelResult = kj::mv(armResult.get<ActorSqlite::CancelAlarmHandler>());

  // Expect rescheduling was requested and that returned promise resolves after fulfillment.
  auto waitBeforeCancel = kj::mv(cancelResult.waitBeforeCancel);
  auto rescheduleFulfiller = kj::mv(test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]);
  KJ_ASSERT(!waitBeforeCancel.poll(test.ws));
  rescheduleFulfiller->fulfill();
  KJ_ASSERT(waitBeforeCancel.poll(test.ws));
  waitBeforeCancel.wait(test.ws);
}

KJ_TEST("does not cancel handler when local db alarm state is later than scheduled alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.setAlarm(twoMs);
  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
}

KJ_TEST("does not cancel handler when local db alarm state is earlier than scheduled alarm") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  test.setAlarm(oneMs);
  {
    auto armResult = test.actor.armAlarmHandler(twoMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
  }
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
}

KJ_TEST("getAlarm() returns null during handler") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
    test.pollAndExpectCalls({});

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
}

KJ_TEST("alarm handler handle clears alarm when dropped with no writes") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("alarm deleter does not clear alarm when dropped with writes") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
    test.setAlarm(twoMs);
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("can cancel deferred alarm deletion during handler") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
    test.actor.cancelDeferredAlarmDeletion();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("canceling deferred alarm deletion outside handler has no effect") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
  }
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  test.actor.cancelDeferredAlarmDeletion();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion outside handler edge case") {
  // Presumably harmless to cancel deletion if the client requests it after the handler ends but
  // before the event loop runs the commit code?  Trying to cancel deletion outside the handler is
  // a bit of a contract violation anyway -- maybe we should just assert against it?
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
  }
  test.actor.cancelDeferredAlarmDeletion();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("canceling deferred alarm deletion is idempotent") {
  // Not sure if important, but matches ActorCache behavior.
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());
    test.actor.cancelDeferredAlarmDeletion();
    test.actor.cancelDeferredAlarmDeletion();
  }

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("handler alarm is not deleted when commit fails") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  {
    auto armResult = test.actor.armAlarmHandler(oneMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());

    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
  }
  test.pollAndExpectCalls({"commit"})[0]->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));
}

KJ_TEST("setting earlier alarm persists alarm scheduling before db") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  // Update alarm to be earlier.  We expect the alarm scheduling to be persisted before the db.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("setting later alarm persists db before alarm scheduling") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Update alarm to be later.  We expect the db to be persisted before the alarm scheduling.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();

  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("multiple set-earlier in-flight alarms wait for earliest before committing db") {
  ActorSqliteTest test;

  // Initialize alarm state to 5ms.
  test.setAlarm(fiveMs);
  test.pollAndExpectCalls({"scheduleRun(5ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == fiveMs);

  // Gate is not blocked.
  auto gateWaitBefore = test.gate.wait();
  KJ_ASSERT(gateWaitBefore.poll(test.ws));

  // Update alarm to be earlier (4ms).  We expect the alarm scheduling to start.
  test.setAlarm(fourMs);
  auto fulfiller4Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(4ms)"})[0]);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == fourMs);

  // Gate as-of 4ms update is blocked.
  auto gateWait4ms = test.gate.wait();
  KJ_ASSERT(!gateWait4ms.poll(test.ws));

  // While 4ms scheduling request is in-flight, update alarm to be even earlier (3ms).  We expect
  // the 4ms request to block the 3ms scheduling request.
  test.setAlarm(threeMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == threeMs);

  // Gate as-of 3ms update is blocked.
  auto gateWait3ms = test.gate.wait();
  KJ_ASSERT(!gateWait3ms.poll(test.ws));

  // Update alarm to be even earlier (2ms).  We expect scheduling requests to still be blocked.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  // Gate as-of 2ms update is blocked.
  auto gateWait2ms = test.gate.wait();
  KJ_ASSERT(!gateWait2ms.poll(test.ws));

  // Fulfill the 4ms request.  We expect the 2ms scheduling to start, because that is the current
  // alarm value.
  fulfiller4Ms->fulfill();
  auto fulfiller2Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]);
  test.pollAndExpectCalls({});

  // While waiting for 2ms request, update alarm time to be 1ms.  Expect scheduling to be blocked.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Gate as-of 1ms update is blocked.
  auto gateWait1ms = test.gate.wait();
  KJ_ASSERT(!gateWait1ms.poll(test.ws));

  // Fulfill the 2ms request.  We expect the 1ms scheduling to start.
  fulfiller2Ms->fulfill();
  auto fulfiller1Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]);
  test.pollAndExpectCalls({});

  // Fulfill the 1ms request.  We expect a single db commit to start (coalescing all previous db
  // commits together).
  fulfiller1Ms->fulfill();
  auto commitFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // We expect all earlier gates to be blocked until commit completes.
  KJ_ASSERT(!gateWait4ms.poll(test.ws));
  KJ_ASSERT(!gateWait3ms.poll(test.ws));
  KJ_ASSERT(!gateWait2ms.poll(test.ws));
  KJ_ASSERT(!gateWait1ms.poll(test.ws));
  commitFulfiller->fulfill();
  KJ_ASSERT(gateWait4ms.poll(test.ws));
  KJ_ASSERT(gateWait3ms.poll(test.ws));
  KJ_ASSERT(gateWait2ms.poll(test.ws));
  KJ_ASSERT(gateWait1ms.poll(test.ws));

  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("setting later alarm times does scheduling after db commit") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Gate is not blocked.
  auto gateWaitBefore = test.gate.wait();
  KJ_ASSERT(gateWaitBefore.poll(test.ws));

  // Set alarm to 2ms.  Expect 2ms db commit to start.
  test.setAlarm(twoMs);
  auto commit2MsFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // Gate as-of 2ms update is blocked.
  auto gateWait2Ms = test.gate.wait();
  KJ_ASSERT(!gateWait2Ms.poll(test.ws));

  // Set alarm to 3ms.  Expect 3ms db commit to start.
  test.setAlarm(threeMs);
  auto commit3MsFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  test.pollAndExpectCalls({});

  // Gate as-of 3ms update is blocked.
  auto gateWait3Ms = test.gate.wait();
  KJ_ASSERT(!gateWait3Ms.poll(test.ws));

  // Fulfill 2ms db commit.  Expect 2ms alarm to be scheduled and 2ms gate to be unblocked.
  KJ_ASSERT(!gateWait2Ms.poll(test.ws));
  commit2MsFulfiller->fulfill();
  KJ_ASSERT(gateWait2Ms.poll(test.ws));
  auto fulfiller2Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]);
  test.pollAndExpectCalls({});

  // Fulfill 3ms db commit.  Expect 3ms alarm to be scheduled and 3ms gate to be unblocked.
  KJ_ASSERT(!gateWait3Ms.poll(test.ws));
  commit3MsFulfiller->fulfill();
  KJ_ASSERT(gateWait3Ms.poll(test.ws));
  auto fulfiller3Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(3ms)"})[0]);
  test.pollAndExpectCalls({});

  // Outstanding alarm scheduling can complete asynchronously.
  fulfiller2Ms->fulfill();
  fulfiller3Ms->fulfill();
}

KJ_TEST("in-flight later alarm times don't affect subsequent commits") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  // Set alarm to 5ms.  Expect 5ms db commit and scheduling to start.
  test.setAlarm(fiveMs);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  auto fulfiller5Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(5ms)"})[0]);

  // While 5ms scheduling is still in-flight, set alarm to 2ms.  Even though the last-confirmed
  // alarm value was 1ms, we expect that setting the alarm to 2ms will be interpreted as setting
  // the alarm earlier, so it will issue the schedule request before the commit request.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  auto commit2MsFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);

  fulfiller5Ms->fulfill();
  commit2MsFulfiller->fulfill();
}

KJ_TEST("rejected alarm scheduling request breaks gate") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->reject(
      KJ_EXCEPTION(FAILED, "a_rejected_scheduleRun"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_scheduleRun", promise.wait(test.ws));
}

KJ_TEST("an exception thrown during merged commits does not hang") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Initialize alarm state to 5ms.
  test.setAlarm(fiveMs);
  test.pollAndExpectCalls({"scheduleRun(5ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == fiveMs);

  // Update alarm to be earlier (4ms).  We expect the alarm scheduling to start.
  test.setAlarm(fourMs);
  auto fulfiller4Ms = kj::mv(test.pollAndExpectCalls({"scheduleRun(4ms)"})[0]);
  auto gateWait4ms = test.gate.wait();

  // While 4ms scheduling request is in-flight, update alarm to be earlier (3ms).  We expect
  // the two commit requests to merge and be blocked on the alarm scheduling request.
  test.setAlarm(threeMs);
  test.pollAndExpectCalls({});
  auto gateWait3ms = test.gate.wait();

  // Reject the 4ms request.  We expect both gate waiting promises to unblock with exceptions.
  KJ_ASSERT(!gateWait4ms.poll(test.ws));
  KJ_ASSERT(!gateWait3ms.poll(test.ws));
  fulfiller4Ms->reject(KJ_EXCEPTION(FAILED, "a_rejected_scheduleRun"));
  KJ_ASSERT(gateWait4ms.poll(test.ws));
  KJ_ASSERT(gateWait3ms.poll(test.ws));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_scheduleRun", gateWait4ms.wait(test.ws));
  KJ_EXPECT_THROW_MESSAGE("a_rejected_scheduleRun", gateWait3ms.wait(test.ws));
  KJ_EXPECT_THROW_MESSAGE("a_rejected_scheduleRun", promise.wait(test.ws));
}

KJ_TEST("getAlarm/setAlarm check for brokenness") {
  ActorSqliteTest test({.monitorOutputGate = false});

  auto promise = test.gate.onBroken();

  // Break gate
  test.put("foo", "bar");
  test.pollAndExpectCalls({"commit"})[0]->reject(KJ_EXCEPTION(FAILED, "a_rejected_commit"));

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", promise.wait(test.ws));

  // Apparently we don't actually set brokenness until the taskFailed handler runs, but presumably
  // this is OK?
  test.getAlarm();

  // Ensure taskFailed handler runs and notices brokenness:
  test.ws.poll();

  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.getAlarm());
  KJ_EXPECT_THROW_MESSAGE("a_rejected_commit", test.setAlarm(kj::none));
  test.pollAndExpectCalls({});
}

KJ_TEST("calling deleteAll() preserves alarm state if alarm is set") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  ActorCache::DeleteAllResults results = test.actor.deleteAll({});
  KJ_ASSERT(results.backpressure == kj::none);
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  auto commitFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  KJ_ASSERT(results.count.wait(test.ws) == 0);
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  commitFulfiller->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("calling deleteAll() preserves alarm state if alarm is not set") {
  ActorSqliteTest test;

  // Initialize alarm state to empty value in metadata table.
  test.setAlarm(oneMs);
  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
  test.setAlarm(kj::none);
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

  ActorCache::DeleteAllResults results = test.actor.deleteAll({});
  KJ_ASSERT(results.backpressure == kj::none);
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

  auto commitFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  KJ_ASSERT(results.count.wait(test.ws) == 0);
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

  commitFulfiller->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

  // We can also assert that we leave the database empty, in case that turns out to be useful later:
  auto q = test.db.run("SELECT name FROM sqlite_master WHERE type='table' AND name='_cf_METADATA'");
  KJ_ASSERT(q.isDone());
}

KJ_TEST("calling deleteAll() during an implicit transaction preserves alarm state") {
  ActorSqliteTest test;

  // Initialize alarm state to 1ms.
  test.setAlarm(oneMs);

  ActorCache::DeleteAllResults results = test.actor.deleteAll({});
  KJ_ASSERT(results.backpressure == kj::none);
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();

  auto commitFulfiller = kj::mv(test.pollAndExpectCalls({"commit"})[0]);
  KJ_ASSERT(results.count.wait(test.ws) == 0);
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  commitFulfiller->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);

  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("rolling back transaction leaves alarm in expected state") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  {
    auto txn = test.actor.startTransaction();
    KJ_ASSERT(expectSync(txn->getAlarm({})) == twoMs);
    txn->setAlarm(oneMs, {});
    KJ_ASSERT(expectSync(txn->getAlarm({})) == oneMs);
    // Dropping transaction without committing; should roll back.
  }
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);
}

KJ_TEST("rolling back transaction leaves deferred alarm deletion in expected state") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  {
    auto armResult = test.actor.armAlarmHandler(twoMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());

    auto txn = test.actor.startTransaction();
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
    test.setAlarm(oneMs);
    KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    txn->rollback().wait(test.ws);

    // After rollback, getAlarm() still returns the deferred deletion result.
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

    // After rollback, no changes committed, no change in scheduled alarm.
    test.pollAndExpectCalls({});
  }

  // After handler, 2ms alarm is deleted.
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

KJ_TEST("committing transaction leaves deferred alarm deletion in expected state") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  {
    auto armResult = test.actor.armAlarmHandler(twoMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());

    auto txn = test.actor.startTransaction();
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
    test.setAlarm(oneMs);
    KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    txn->commit();

    // After commit, getAlarm() returns the committed value.
    KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    test.pollAndExpectCalls({"scheduleRun(1ms)"})[0]->fulfill();
    test.pollAndExpectCalls({"commit"})[0]->fulfill();
    test.pollAndExpectCalls({});
  }

  // Alarm not deleted
  KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
}

KJ_TEST("rolling back nested transaction leaves deferred alarm deletion in expected state") {
  ActorSqliteTest test;

  // Initialize alarm state to 2ms.
  test.setAlarm(twoMs);
  test.pollAndExpectCalls({"scheduleRun(2ms)"})[0]->fulfill();
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({});
  KJ_ASSERT(expectSync(test.getAlarm()) == twoMs);

  {
    auto armResult = test.actor.armAlarmHandler(twoMs, false);
    KJ_ASSERT(armResult.is<ActorSqlite::RunAlarmHandler>());

    auto txn1 = test.actor.startTransaction();
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
    {
      // Rolling back nested transaction change leaves deferred deletion in place.
      auto txn2 = test.actor.startTransaction();
      KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
      test.setAlarm(oneMs);
      KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
      txn2->rollback().wait(test.ws);
      KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
    }
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
    {
      // Committing nested transaction changes parent transaction state to dirty.
      auto txn3 = test.actor.startTransaction();
      KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
      test.setAlarm(oneMs);
      KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
      txn3->commit();
      KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    }
    KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    {
      // Nested transaction of dirty transaction is dirty, rollback has no effect.
      auto txn4 = test.actor.startTransaction();
      KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
      txn4->rollback().wait(test.ws);
      KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    }
    KJ_ASSERT(expectSync(test.getAlarm()) == oneMs);
    txn1->rollback().wait(test.ws);

    // After root transaction rollback, getAlarm() still returns the deferred deletion result.
    KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);

    // After rollback, no changes committed, no change in scheduled alarm.
    test.pollAndExpectCalls({});
  }

  // After handler, 2ms alarm is deleted.
  test.pollAndExpectCalls({"commit"})[0]->fulfill();
  test.pollAndExpectCalls({"scheduleRun(none)"})[0]->fulfill();
  KJ_ASSERT(expectSync(test.getAlarm()) == kj::none);
}

}  // namespace
}  // namespace workerd
