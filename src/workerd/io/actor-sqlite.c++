// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"

#include "io-gate.h"

#include <workerd/jsg/exception.h>
#include <workerd/util/sentry.h>

#include <algorithm>

namespace workerd {

namespace {

// Returns true if a given (set or unset) alarm will fire earlier than another.
static bool willFireEarlier(kj::Maybe<kj::Date> alarm1, kj::Maybe<kj::Date> alarm2) {
  // Intuitively, an unset alarm is effectively indistinguishable from an alarm set at infinity.
  return alarm1.orDefault(kj::maxValue) < alarm2.orDefault(kj::maxValue);
}

}  // namespace

ActorSqlite::ActorSqlite(kj::Own<SqliteDatabase> dbParam,
    OutputGate& outputGate,
    kj::Function<kj::Promise<void>()> commitCallback,
    Hooks& hooks)
    : db(kj::mv(dbParam)),
      outputGate(outputGate),
      commitCallback(kj::mv(commitCallback)),
      hooks(hooks),
      kv(*db),
      metadata(*db),
      commitTasks(*this) {
  db->onWrite(KJ_BIND_METHOD(*this, onWrite));
  lastConfirmedAlarmDbState = metadata.getAlarm();

  // Because we preserve an invariant that scheduled alarms are always at or earlier than
  // persisted db alarm state, it should be OK to populate our idea of the latest scheduled alarm
  // using the current db alarm state.  At worst, it may perform one unnecessary scheduling
  // request in cases where a previous alarm-state-altering transaction failed.
  alarmScheduledNoLaterThan = metadata.getAlarm();
}

ActorSqlite::ImplicitTxn::ImplicitTxn(ActorSqlite& parent): parent(parent) {
  KJ_REQUIRE(parent.currentTxn.is<NoTxn>());
  parent.beginTxn.run();
  parent.currentTxn = this;
}
ActorSqlite::ImplicitTxn::~ImplicitTxn() noexcept(false) {
  KJ_IF_SOME(c, parent.currentTxn.tryGet<ImplicitTxn*>()) {
    if (c == this) {
      parent.currentTxn.init<NoTxn>();
    }
  }
  if (!committed && parent.broken == kj::none) {
    // Failed to commit, so roll back.
    //
    // This should only happen in cases of catastrophic error. Since this is rarely actually
    // executed, we don't prepare a statement for it.
    parent.db->run("ROLLBACK TRANSACTION");
    parent.metadata.invalidate();
  }
}

void ActorSqlite::ImplicitTxn::commit() {
  // Ignore redundant commit()s.
  if (!committed) {
    parent.commitTxn.run();
    committed = true;
  }
}

void ActorSqlite::ImplicitTxn::rollback() {
  // Ignore redundant commit()s.
  if (!committed) {
    // As of this writing, rollback() is only called when the database is about to be reset.
    // Preparing a statement for it would be a waste since that statement would never be executed
    // more than once, since resetting requires repreparing all statements anyway. So we don't
    // bother.
    parent.db->run("ROLLBACK TRANSACTION");
    committed = true;
    parent.metadata.invalidate();
  }
}

ActorSqlite::ExplicitTxn::ExplicitTxn(ActorSqlite& actorSqlite): actorSqlite(actorSqlite) {
  KJ_SWITCH_ONEOF(actorSqlite.currentTxn) {
    KJ_CASE_ONEOF(_, NoTxn) {}
    KJ_CASE_ONEOF(implicit, ImplicitTxn*) {
      // An implicit transaction is open, commit it now because it would be weird if writes
      // performed before the explicit transaction started were postponed until the transaction
      // completes. Note that this isn't violating any atomicity guarantees because the transaction
      // API is async, and atomicity is only guaranteed over synchronous code.
      implicit->commit();
    }
    KJ_CASE_ONEOF(exp, ExplicitTxn*) {
      KJ_REQUIRE(!exp->hasChild,
          "critical section should have blocked creation of more than one child at a time");
      parent = kj::addRef(*exp);
      exp->hasChild = true;
      depth = exp->depth + 1;
      alarmDirty = exp->alarmDirty;
    }
  }
  actorSqlite.currentTxn = this;

  // To support nested transactions, we assign each savepoint a name based on its nesting depth.
  // Unfortunately this means we cannot prepare the statement, unless we prepare a series of
  // statements for each depth. (Actually, it could be reasonable to prepare statements for
  // depth 0 specifically, but I'm not going to try it for now.)
  actorSqlite.db->run(SqliteDatabase::TRUSTED, kj::str("SAVEPOINT _cf_savepoint_", depth));
}
ActorSqlite::ExplicitTxn::~ExplicitTxn() noexcept(false) {
  [&]() noexcept {
    // We'd better crash if any of this state update fails, otherwise dangling pointers.

    KJ_ASSERT(!hasChild);
    auto cur = KJ_ASSERT_NONNULL(actorSqlite.currentTxn.tryGet<ExplicitTxn*>());
    KJ_ASSERT(cur == this);
    KJ_IF_SOME(p, parent) {
      p.get()->hasChild = false;
      actorSqlite.currentTxn = p.get();
    } else {
      actorSqlite.currentTxn.init<NoTxn>();
    }
  }();

  if (!committed) {
    // Assume rollback if not committed.
    rollbackImpl();
  }
}

bool ActorSqlite::ExplicitTxn::getAlarmDirty() {
  return alarmDirty;
}

void ActorSqlite::ExplicitTxn::setAlarmDirty() {
  alarmDirty = true;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::commit() {
  KJ_REQUIRE(!hasChild,
      "critical sections should have prevented committing transaction while "
      "nested txn is outstanding");

  // Start the schedule request before root transaction commit(), for correctness in workerd.
  kj::Maybe<PrecommitAlarmState> precommitAlarmState;
  if (parent == kj::none) {
    precommitAlarmState = actorSqlite.startPrecommitAlarmScheduling();
  }

  actorSqlite.db->run(SqliteDatabase::TRUSTED, kj::str("RELEASE _cf_savepoint_", depth));
  committed = true;

  KJ_IF_SOME(p, parent) {
    if (alarmDirty) {
      p->alarmDirty = true;
    }
  } else {
    if (alarmDirty) {
      actorSqlite.haveDeferredDelete = false;
    }

    // We committed the root transaction, so it's time to signal any replication layer and lock
    // the output gate in the meantime.
    actorSqlite.commitTasks.add(actorSqlite.outputGate.lockWhile(
        actorSqlite.commitImpl(kj::mv(KJ_ASSERT_NONNULL(precommitAlarmState)))));
  }

  // No backpressure for SQLite.
  return kj::none;
}

kj::Promise<void> ActorSqlite::ExplicitTxn::rollback() {
  JSG_REQUIRE(!hasChild, Error,
      "Cannot roll back an outer transaction while a nested transaction is still running.");
  if (!committed) {
    rollbackImpl();
    committed = true;
  }
  return kj::READY_NOW;
}

void ActorSqlite::ExplicitTxn::rollbackImpl() noexcept(false) {
  actorSqlite.db->run(SqliteDatabase::TRUSTED, kj::str("ROLLBACK TO _cf_savepoint_", depth));
  actorSqlite.db->run(SqliteDatabase::TRUSTED, kj::str("RELEASE _cf_savepoint_", depth));
  actorSqlite.metadata.invalidate();
  KJ_IF_SOME(p, parent) {
    alarmDirty = p->alarmDirty;
  } else {
    alarmDirty = false;
  }
}

void ActorSqlite::onWrite() {
  if (currentTxn.is<NoTxn>()) {
    auto txn = kj::heap<ImplicitTxn>(*this);

    commitTasks.add(outputGate.lockWhile(
        kj::evalLater([this, txn = kj::mv(txn)]() mutable -> kj::Promise<void> {
      // Don't commit if shutdown() has been called.
      requireNotBroken();

      // Start the schedule request before commit(), for correctness in workerd.
      auto precommitAlarmState = startPrecommitAlarmScheduling();

      try {
        txn->commit();
      } catch (...) {
        // HACK: If we became broken during `COMMIT TRANSACTION` then throw the broken exception
        // instead of whatever SQLite threw.
        requireNotBroken();

        // No, we're not broken, so propagate the exception as-is.
        throw;
      }

      // The callback is only expected to commit writes up until this point. Any new writes that
      // occur while the callback is in progress are NOT included, therefore require a new commit
      // to be scheduled. So, we should drop `txn` to cause `currentTxn` to become NoTxn now,
      // rather than after the callback.
      { auto drop = kj::mv(txn); }

      return commitImpl(kj::mv(precommitAlarmState));
    })));
  }
}

kj::Promise<void> ActorSqlite::requestScheduledAlarm(kj::Maybe<kj::Date> requestedTime) {
  // Not using coroutines here, because it's important for correctness in workerd that a
  // synchronously thrown exception in scheduleRun() can escape synchronously to the caller.

  bool movingAlarmLater = willFireEarlier(alarmScheduledNoLaterThan, requestedTime);
  if (movingAlarmLater) {
    // Since we are setting the alarm to be later, we can update alarmScheduledNoLaterThan
    // immediately and still preserve the invariant that the scheduled alarm time is equal to or
    // earlier than the persisted db alarm value.  Doing the immediate update ensures that
    // subsequent invocations of commitImpl() will compare against the correct value in their
    // precommit alarm checks, even if other later-setting requests are still in-flight, without
    // needing to wait for them to complete.
    alarmScheduledNoLaterThan = requestedTime;
  }

  return hooks.scheduleRun(requestedTime).then([this, movingAlarmLater, requestedTime]() {
    if (!movingAlarmLater) {
      alarmScheduledNoLaterThan = requestedTime;
    }
  });
}

ActorSqlite::PrecommitAlarmState ActorSqlite::startPrecommitAlarmScheduling() {
  PrecommitAlarmState state;
  if (pendingCommit == kj::none &&
      willFireEarlier(metadata.getAlarm(), alarmScheduledNoLaterThan)) {
    // Basically, this is the first scheduling request that commitImpl() would make prior to
    // commitCallback().  We start the request separately, ahead of calling sqlite functions that
    // commit to local disk, for correctness in workerd, where alarm scheduling and db commits are
    // both synchronous.
    state.schedulingPromise = requestScheduledAlarm(metadata.getAlarm());
  }
  return kj::mv(state);
}

kj::Promise<void> ActorSqlite::commitImpl(ActorSqlite::PrecommitAlarmState precommitAlarmState) {
  // We assume that exceptions thrown during commit will propagate to the caller, such that they
  // will ensure cancelDeferredAlarmDeletion() is called, if necessary.

  KJ_IF_SOME(pending, pendingCommit) {
    // If an earlier commitImpl() invocation is already in the process of updating precommit
    // alarms but has not yet made the commitCallback() call, it should be OK to wait on it to
    // perform the precommit alarm update and db commit for this invocation, too.
    co_await pending.addBranch();
    co_return;
  }

  // There are no pending commits in-flight, so we set up a forked promise that other callers can
  // wait on, to perform the alarm scheduling and database persistence work for all of them.  Note
  // that the fulfiller is owned by this coroutine context, so if an exception is thrown below,
  // the fulfiller's destructor will detect that the stack is unwinding and will automatically
  // propagate the thrown exception to the other waiters.
  auto [promise, fulfiller] = kj::newPromiseAndFulfiller<void>();
  pendingCommit = promise.fork();

  // Wait for the first precommit alarm scheduling request to complete, if any.  This was set up
  // in startPrecommitAlarmScheduling() and is essentially the first iteration of the below
  // while() loop, but needed to be initiated synchronously before the local database commit to
  // ensure correctness in workerd.
  KJ_IF_SOME(p, precommitAlarmState.schedulingPromise) {
    co_await p;
  }

  // While the local db state requires an earlier alarm than is known might be scheduled, issue an
  // alarm update request for the earlier time and wait for it to complete.  This helps ensure
  // that the successfully scheduled alarm time is always earlier or equal to the alarm state in
  // the successfully persisted db.
  while (willFireEarlier(metadata.getAlarm(), alarmScheduledNoLaterThan)) {
    co_await requestScheduledAlarm(metadata.getAlarm());
  }

  // Issue the commitCallback() request to persist the db state, then synchronously clear the
  // pending commit so that the next commitImpl() invocation starts its own set of precommit alarm
  // updates and db commit.
  auto alarmStateForCommit = metadata.getAlarm();
  auto commitCallbackPromise = commitCallback();
  pendingCommit = kj::none;

  // Wait for the db to persist.
  co_await commitCallbackPromise;
  lastConfirmedAlarmDbState = alarmStateForCommit;

  // Notify any merged commitImpl() requests that the db persistence completed.
  fulfiller->fulfill();

  // If the db state is now later than the in-flight scheduled alarms, issue a request to update
  // it to match the db state.  We don't need to hold open the output gate, so we add the
  // scheduling request to commitTasks.
  if (willFireEarlier(alarmScheduledNoLaterThan, alarmStateForCommit)) {
    commitTasks.add(requestScheduledAlarm(alarmStateForCommit));
  }
}

void ActorSqlite::taskFailed(kj::Exception&& exception) {
  // The output gate should already have been broken since it wraps all commits tasks. So, we
  // don't have to report anything here, the exception will already propagate elsewhere. We
  // should block further operations, though.
  if (broken == kj::none) {
    broken = kj::mv(exception);
  }
}

void ActorSqlite::requireNotBroken() {
  KJ_IF_SOME(e, broken) {
    kj::throwFatalException(kj::cp(e));
  }
}

void ActorSqlite::maybeDeleteDeferredAlarm() {
  if (!inAlarmHandler) {
    // Pretty sure this can't happen.
    LOG_WARNING_ONCE("expected to be in alarm handler when trying to delete alarm");
  }
  inAlarmHandler = false;

  if (haveDeferredDelete) {
    metadata.setAlarm(kj::none);
    haveDeferredDelete = false;
  }
}

// =======================================================================================
// ActorCacheInterface implementation

kj::OneOf<kj::Maybe<ActorCacheOps::Value>, kj::Promise<kj::Maybe<ActorCacheOps::Value>>>
ActorSqlite::get(Key key, ReadOptions options) {
  requireNotBroken();

  kj::Maybe<ActorCacheOps::Value> result;
  kv.get(key, [&](ValuePtr value) { result = kj::heapArray(value); });
  return result;
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::get(
    kj::Array<Key> keys, ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  for (auto& key: keys) {
    kv.get(
        key, [&](ValuePtr value) { results.add(KeyValuePair{kj::mv(key), kj::heapArray(value)}); });
  }
  std::sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.key < b.key; });
  return GetResultList(kj::mv(results));
}

kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorSqlite::getAlarm(
    ReadOptions options) {
  requireNotBroken();

  bool transactionAlarmDirty = false;
  KJ_IF_SOME(exp, currentTxn.tryGet<ExplicitTxn*>()) {
    transactionAlarmDirty = exp->getAlarmDirty();
  }

  if (haveDeferredDelete && !transactionAlarmDirty) {
    // If an alarm handler is currently running, and a new alarm time has not been set yet, We
    // need to return that there is no alarm.
    return kj::Maybe<kj::Date>(kj::none);
  } else {
    return metadata.getAlarm();
  }
  KJ_UNREACHABLE;
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::
    list(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::FORWARD, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair{kj::str(key), kj::heapArray(value)});
  });

  // Already guaranteed sorted.
  return GetResultList(kj::mv(results));
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::
    listReverse(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::REVERSE, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair{kj::str(key), kj::heapArray(value)});
  });

  // Already guaranteed sorted (reversed).
  return GetResultList(kj::mv(results));
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(Key key, Value value, WriteOptions options) {
  requireNotBroken();

  kv.put(key, value);
  return kj::none;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(kj::Array<KeyValuePair> pairs, WriteOptions options) {
  requireNotBroken();

  for (auto& pair: pairs) {
    kv.put(pair.key, pair.value);
  }
  return kj::none;
}

kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::delete_(Key key, WriteOptions options) {
  requireNotBroken();

  return kv.delete_(key);
}

kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::delete_(kj::Array<Key> keys, WriteOptions options) {
  requireNotBroken();

  uint count = 0;
  for (auto& key: keys) {
    count += kv.delete_(key);
  }
  return count;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) {
  requireNotBroken();

  // TODO(someday): When deleting alarm data in an otherwise empty database, clear the database to
  // free up resources?

  metadata.setAlarm(newAlarmTime);

  KJ_IF_SOME(exp, currentTxn.tryGet<ExplicitTxn*>()) {
    exp->setAlarmDirty();
  } else {
    haveDeferredDelete = false;
  }

  return kj::none;
}

kj::Own<ActorCacheInterface::Transaction> ActorSqlite::startTransaction() {
  requireNotBroken();

  return kj::refcounted<ExplicitTxn>(*this);
}

ActorCacheInterface::DeleteAllResults ActorSqlite::deleteAll(WriteOptions options) {
  requireNotBroken();

  // kv.deleteAll() clears the database, so we need to save and possibly restore alarm state in
  // the metadata table, to try to match the behavior of ActorCache, which preserves the set alarm
  // when running deleteAll().
  auto localAlarmState = metadata.getAlarm();

  // deleteAll() cannot be part of a transaction because it deletes the database altogether. So,
  // we have to close our transactions or fail.
  KJ_SWITCH_ONEOF(currentTxn) {
    KJ_CASE_ONEOF(_, NoTxn) {
      // good
    }
    KJ_CASE_ONEOF(implicit, ImplicitTxn*) {
      // Whatever the implicit transaction did, it's about to be blown away anyway. Roll it back
      // so we don't waste time flushing these writes anywhere.
      implicit->rollback();
      currentTxn = NoTxn();
    }
    KJ_CASE_ONEOF(exp, ExplicitTxn*) {
      // Keep in mind:
      //
      //   ctx.storage.transaction(txn => {
      //     txn.deleteAll();          // calls `DurableObjectTransaction::deleteAll()`
      //     ctx.storage.deleteAll();  // calls this method, `ActorSqlite::deleteAll()`
      //   });
      //
      // `DurableObjectTransaction::deleteAll()` throws this exception, since `deleteAll()` is not
      // supported inside a transaction. Under the new SQLite-backed storage system, directly
      // calling `cxt.storage` inside a transaction (as opposed to using the `txn` object) should
      // still be treated as part of the transaction, and so should throw the same thing.
      JSG_FAIL_REQUIRE(Error, "Cannot call deleteAll() within a transaction");
    }
  }

  if (localAlarmState == kj::none && !deleteAllCommitScheduled) {
    // If we're not going to perform a write to restore alarm state, we'll want to make sure the
    // commit callback is called for the deleteAll().
    commitTasks.add(outputGate.lockWhile(kj::evalLater([this]() mutable -> kj::Promise<void> {
      // Don't commit if shutdown() has been called.
      requireNotBroken();

      deleteAllCommitScheduled = false;
      return commitCallback();
    })));
    deleteAllCommitScheduled = true;
  }

  uint count = kv.deleteAll();

  // TODO(correctness): Since workerd doesn't have a separate durability step, in the unlikely
  // event of a failure here, between deleteAll() and setAlarm(), we could theoretically lose the
  // current alarm state when running under workerd.  Not sure if there's a practical way to avoid
  // this.

  // Reset alarm state, if necessary.  If no alarm is set, OK to just leave metadata table
  // uninitialized.
  if (localAlarmState != kj::none) {
    metadata.setAlarm(localAlarmState);
  }

  return {
    .backpressure = kj::none,
    .count = count,
  };
}

kj::Maybe<kj::Promise<void>> ActorSqlite::evictStale(kj::Date now) {
  // This implementation never needs to apply backpressure.
  return kj::none;
}

void ActorSqlite::shutdown(kj::Maybe<const kj::Exception&> maybeException) {
  // TODO(cleanup): Logic copied from ActorCache::shutdown(). Should they share somehow?

  if (broken == kj::none) {
    auto exception = [&]() {
      KJ_IF_SOME(e, maybeException) {
        // We were given an exception, use it.
        return kj::cp(e);
      }

      // Use the direct constructor so that we can reuse the constexpr message variable for testing.
      auto exception = kj::Exception(kj::Exception::Type::DISCONNECTED, __FILE__, __LINE__,
          kj::heapString(ActorCache::SHUTDOWN_ERROR_MESSAGE));

      // Add trace info sufficient to tell us which operation caused the failure.
      exception.addTraceHere();
      exception.addTrace(__builtin_return_address(0));
      return exception;
    }();

    // Any scheduled flushes will fail once `flushImpl()` is invoked and notices that
    // `maybeTerminalException` has a value. Any in-flight flushes will continue to run in the
    // background. Remember that these in-flight flushes may or may not be awaited by the worker,
    // but they still hold the output lock as long as `allowUnconfirmed` wasn't used.
    broken.emplace(kj::mv(exception));

    // We explicitly do not schedule a flush to break the output gate. This means that if a request
    // is ongoing after the actor cache is shutting down, the output gate is only broken if they
    // had to send a flush after shutdown, either from a scheduled flush or a retry after failure.
  } else {
    // We've already experienced a terminal exception either from shutdown or oom, there should
    // already be a flush scheduled that will break the output gate.
  }
}

kj::OneOf<ActorSqlite::CancelAlarmHandler, ActorSqlite::RunAlarmHandler> ActorSqlite::
    armAlarmHandler(kj::Date scheduledTime, bool noCache) {
  KJ_ASSERT(!inAlarmHandler);

  if (haveDeferredDelete) {
    // Unlikely to happen, unless caller is starting new alarm handler before previous alarm
    // handler cleanup has completed.
    LOG_WARNING_ONCE("expected previous alarm handler to be cleaned up");
  }

  auto localAlarmState = metadata.getAlarm();
  if (localAlarmState != scheduledTime) {
    if (localAlarmState == lastConfirmedAlarmDbState) {
      // If there's a clean db time that differs from the requested handler's scheduled time, this
      // run should be canceled.
      if (willFireEarlier(scheduledTime, localAlarmState)) {
        // If the handler's scheduled time is earlier than the clean scheduled time, we may be
        // recovering from a failed db commit or scheduling request, so we need to request that
        // the alarm be rescheduled for the current db time, and tell the caller to wait for
        // successful rescheduling before cancelling the current handler invocation.
        //
        // TODO(perf): If we already have such a rescheduling request in-flight, might want to
        // coalesce with the existing request?
        if (localAlarmState == kj::none) {
          // If clean scheduled time is unset, don't need to reschedule; just cancel the alarm.
          return CancelAlarmHandler{.waitBeforeCancel = kj::READY_NOW};
        } else {
          return CancelAlarmHandler{.waitBeforeCancel = requestScheduledAlarm(localAlarmState)};
        }
      } else {
        return CancelAlarmHandler{.waitBeforeCancel = kj::READY_NOW};
      }
    } else {
      // There's a alarm write that hasn't been set yet pending for a time different than ours --
      // We won't cancel the alarm because it hasn't been confirmed, but we shouldn't delete
      // the pending write.
      haveDeferredDelete = false;
    }
  } else {
    haveDeferredDelete = true;
  }
  inAlarmHandler = true;

  static const DeferredAlarmDeleter disposer;
  return RunAlarmHandler{.deferredDelete = kj::Own<void>(this, disposer)};
}

void ActorSqlite::cancelDeferredAlarmDeletion() {
  if (!inAlarmHandler) {
    // Pretty sure this can't happen.
    LOG_WARNING_ONCE("expected to be in alarm handler when trying to cancel deleted alarm");
  }
  haveDeferredDelete = false;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::onNoPendingFlush() {
  // This implements sync().
  //
  // TODO(sqlite): When we implement `allowUnconfirmed`, this implementation becomes incorrect
  //   because sync() should wait on all writes, even ones with that flag, whereas the output
  //   gate is not blocked by `allowUnconfirmed` writes. At present we haven't actually
  //   implemented `allowUnconfirmed` yet.
  return outputGate.wait();
}

const ActorSqlite::Hooks ActorSqlite::Hooks::DEFAULT = ActorSqlite::Hooks{};

kj::Promise<void> ActorSqlite::Hooks::scheduleRun(kj::Maybe<kj::Date> newAlarmTime) {
  JSG_FAIL_REQUIRE(Error, "alarms are not yet implemented for SQLite-backed Durable Objects");
}

kj::OneOf<kj::Maybe<ActorCacheOps::Value>, kj::Promise<kj::Maybe<ActorCacheOps::Value>>>
ActorSqlite::ExplicitTxn::get(Key key, ReadOptions options) {
  return actorSqlite.get(kj::mv(key), options);
}
kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::
    ExplicitTxn::get(kj::Array<Key> keys, ReadOptions options) {
  return actorSqlite.get(kj::mv(keys), options);
}
kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorSqlite::ExplicitTxn::getAlarm(
    ReadOptions options) {
  return actorSqlite.getAlarm(options);
}
kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::
    ExplicitTxn::list(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  return actorSqlite.list(kj::mv(begin), kj::mv(end), limit, options);
}
kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>> ActorSqlite::
    ExplicitTxn::listReverse(
        Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  return actorSqlite.listReverse(kj::mv(begin), kj::mv(end), limit, options);
}
kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::put(
    Key key, Value value, WriteOptions options) {
  return actorSqlite.put(kj::mv(key), kj::mv(value), options);
}
kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options) {
  return actorSqlite.put(kj::mv(pairs), options);
}
kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::ExplicitTxn::delete_(
    Key key, WriteOptions options) {
  return actorSqlite.delete_(kj::mv(key), options);
}
kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::ExplicitTxn::delete_(
    kj::Array<Key> keys, WriteOptions options) {
  return actorSqlite.delete_(kj::mv(keys), options);
}
kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) {
  return actorSqlite.setAlarm(newAlarmTime, options);
}

}  // namespace workerd
