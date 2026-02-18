// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"

#include "io-gate.h"

#include <workerd/jsg/exception.h>
#include <workerd/util/autogate.h>
#include <workerd/util/sentry.h>

#include <kj/exception.h>
#include <kj/function.h>

#include <algorithm>

namespace workerd {

namespace {

// Returns true if a given (set or unset) alarm will fire earlier than another.
static bool willFireEarlier(kj::Maybe<kj::Date> alarm1, kj::Maybe<kj::Date> alarm2) {
  // Intuitively, an unset alarm is effectively indistinguishable from an alarm set at infinity.
  return alarm1.orDefault(kj::maxValue) < alarm2.orDefault(kj::maxValue);
}

// Helper to make kj::Maybe<kj::Date> loggable - returns the date or kj::maxValue for logging
static kj::Date logDate(kj::Maybe<kj::Date> maybeDate) {
  return maybeDate.orDefault(kj::maxValue);
}

// Set options.allowUnconfirmed to false and log a reason why.
void disableAllowUnconfirmed(ActorCacheOps::WriteOptions& options, kj::StringPtr reason) {
  if (options.allowUnconfirmed) {
    KJ_LOG(WARNING, "NOSENTRY allowUnconfirmed disabled", reason);
    options.allowUnconfirmed = false;
  }
}

}  // namespace

ActorSqlite::ActorSqlite(kj::Own<SqliteDatabase> dbParam,
    OutputGate& outputGate,
    kj::Function<kj::Promise<void>()> commitCallback,
    Hooks& hooks,
    bool debugAlarmSyncParam)
    : db(kj::mv(dbParam)),
      outputGate(outputGate),
      commitCallback(kj::mv(commitCallback)),
      hooks(hooks),
      kv(*db),
      metadata(*db),
      commitTasks(*this),
      debugAlarmSync(debugAlarmSyncParam) {
  db->onWrite(KJ_BIND_METHOD(*this, onWrite));
  db->onCriticalError(KJ_BIND_METHOD(*this, onCriticalError));
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
  }
}

void ActorSqlite::ImplicitTxn::setSomeWriteConfirmed(bool someWriteConfirmed) {
  this->someWriteConfirmed = someWriteConfirmed;
}

bool ActorSqlite::ImplicitTxn::isSomeWriteConfirmed() const {
  return someWriteConfirmed;
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
      someWriteConfirmed = exp->someWriteConfirmed;
    }
  }
  actorSqlite.currentTxn = this;

  // To support nested transactions, we assign each savepoint a name based on its nesting depth.
  // Unfortunately this means we cannot prepare the statement, unless we prepare a series of
  // statements for each depth. (Actually, it could be reasonable to prepare statements for
  // depth 0 specifically, but I'm not going to try it for now.)
  actorSqlite.db->run(
      {.regulator = SqliteDatabase::TRUSTED}, kj::str("SAVEPOINT _cf_savepoint_", depth));
}
ActorSqlite::ExplicitTxn::~ExplicitTxn() noexcept(false) {
  KJ_DEFER([&]() noexcept {
    // We'd better crash if any of this state update fails, otherwise dangling pointers.

    // This is wrapped in a KJ_DEFER because we want it to run no matter what *after* the rollback
    // fix and KJ_DEFER seemed like the cleanest way to do this.

    KJ_ASSERT(!hasChild);
    auto cur = KJ_ASSERT_NONNULL(actorSqlite.currentTxn.tryGet<ExplicitTxn*>());
    KJ_ASSERT(cur == this);
    KJ_IF_SOME(p, parent) {
      p.get()->hasChild = false;
      actorSqlite.currentTxn = p.get();
    } else {
      actorSqlite.currentTxn.init<NoTxn>();
    }
  }(););

  if (!committed && actorSqlite.broken == kj::none) {
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

void ActorSqlite::ExplicitTxn::setSomeWriteConfirmed(bool someWriteConfirmed) {
  this->someWriteConfirmed = someWriteConfirmed;
}

bool ActorSqlite::ExplicitTxn::isSomeWriteConfirmed() const {
  return someWriteConfirmed;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::commit() {
  actorSqlite.requireNotBroken();
  KJ_REQUIRE(!hasChild,
      "critical sections should have prevented committing transaction while "
      "nested txn is outstanding");

  // Start the schedule request before root transaction commit(), for correctness in workerd.
  kj::Maybe<PrecommitAlarmState> precommitAlarmState;
  if (parent == kj::none) {
    precommitAlarmState = actorSqlite.startPrecommitAlarmScheduling();
  }

  actorSqlite.db->run(
      {.regulator = SqliteDatabase::TRUSTED}, kj::str("RELEASE _cf_savepoint_", depth));
  committed = true;

  KJ_IF_SOME(p, parent) {
    if (alarmDirty) {
      p->alarmDirty = true;
    }
    if (someWriteConfirmed) {
      p->someWriteConfirmed = true;
    }
  } else {
    if (alarmDirty) {
      actorSqlite.haveDeferredDelete = false;
    }

    // We committed the root transaction, so it's time to signal any replication layer and lock
    // the output gate in the meantime.

    // Unlike ImplicitTxn, which locks the output gate at the start of the first write that requires
    // confirmation, ExplicitTxn only locks when we're going to confirm the commit.  I think this
    // makes since given the explicit commit call.
    auto commitPromise = kj::evalNow([this, &precommitAlarmState]() {
      return actorSqlite.commitImpl(
          kj::mv(KJ_ASSERT_NONNULL(precommitAlarmState)), actorSqlite.currentCommitSpan.addRef());
    })
                             .catch_([outputGate = &actorSqlite.outputGate,
                                         spanParent = actorSqlite.currentCommitSpan.addRef()](
                                         kj::Exception&& e) mutable {
      // Unconditionally break the output gate if commit threw an error, no matter whether the
      // commit was confirmed or unconfirmed.
      return outputGate->lockWhile(kj::Promise<void>(kj::mv(e)), kj::mv(spanParent));
    });
    if (someWriteConfirmed) {
      commitPromise = actorSqlite.outputGate.lockWhile(
          kj::mv(commitPromise), actorSqlite.currentCommitSpan.addRef());
    }
    auto forkedPromise = commitPromise.fork();
    actorSqlite.commitTasks.add(forkedPromise.addBranch());
    actorSqlite.lastCommit = kj::mv(forkedPromise);
  }

  // No backpressure for SQLite.
  return kj::none;
}

kj::Promise<void> ActorSqlite::ExplicitTxn::rollback() {
  actorSqlite.requireNotBroken();
  JSG_REQUIRE(!hasChild, Error,
      "Cannot roll back an outer transaction while a nested transaction is still running.");
  if (!committed) {
    rollbackImpl();
    committed = true;
  }
  return kj::READY_NOW;
}

void ActorSqlite::ExplicitTxn::rollbackImpl() noexcept(false) {
  actorSqlite.db->run(
      {.regulator = SqliteDatabase::TRUSTED}, kj::str("ROLLBACK TO _cf_savepoint_", depth));
  actorSqlite.db->run(
      {.regulator = SqliteDatabase::TRUSTED}, kj::str("RELEASE _cf_savepoint_", depth));
  KJ_IF_SOME(p, parent) {
    alarmDirty = p->alarmDirty;
    someWriteConfirmed = p->someWriteConfirmed;
  } else {
    alarmDirty = false;
    someWriteConfirmed = false;
  }
}

void ActorSqlite::onCriticalError(
    kj::StringPtr errorMessage, kj::Maybe<kj::Exception> maybeException) {
  // If we have already experienced a terminal exception, no need to replace it
  if (broken == kj::none) {
    kj::Exception exception = kj::mv(maybeException).orDefault([&]() {
      return JSG_KJ_EXCEPTION(FAILED, Error, errorMessage);
    });
    exception.setDescription(kj::str("broken.outputGateBroken; ", exception.getDescription()));
    broken.emplace(kj::cp(exception));

    // Also ensure output gate is explicitly broken.
    commitTasks.add(
        outputGate.lockWhile(kj::Promise<void>(kj::mv(exception)), currentCommitSpan.addRef()));
  }
}

void ActorSqlite::startImplicitTxn() {
  auto txn = kj::heap<ImplicitTxn>(*this);

  // We implement the magic of accumulating all of the writes between JavaScript awaits in one
  // transaction by evaluating by wrapping the commit function with kj::evalLater, which runs the
  // function on the next turn of the event loop
  auto commitPromise =
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

    // Move the commit span out immediately so new writes can capture a fresh span.
    return commitImpl(kj::mv(precommitAlarmState), kj::mv(currentCommitSpan));
  })
          // Unconditionally break the output gate if commit threw an error, no matter whether the
          // commit was confirmed or unconfirmed.
          .catch_([this](kj::Exception&& e) {
    return outputGate.lockWhile(kj::Promise<void>(kj::mv(e)), nullptr);
  })
          // We need to wait for this in commitTasks and in lastCommit.
          .fork();

  commitTasks.add(commitPromise.addBranch());

  // Commits must be executed in order, so we only have to track the most recent commit promise.
  lastCommit = kj::mv(commitPromise);
}

void ActorSqlite::onWrite(bool allowUnconfirmed) {
  requireNotBroken();
  if (currentTxn.is<NoTxn>()) {
    startImplicitTxn();
  }

  // Update the status of the current transaction.
  KJ_SWITCH_ONEOF(currentTxn) {
    KJ_CASE_ONEOF(_, NoTxn) {
      KJ_FAIL_REQUIRE("we must have a transaction at this point");
    }
    KJ_CASE_ONEOF(implicitTxn, ImplicitTxn*) {
      if (!implicitTxn->isSomeWriteConfirmed() && !allowUnconfirmed) {
        // This is adding a must-confirm write to the transaction, so we must ensure the outputGate
        // locks for remainder of this transaction.
        implicitTxn->setSomeWriteConfirmed(!allowUnconfirmed);
        commitTasks.add(outputGate.lockWhile(lastCommit.addBranch(), currentCommitSpan.addRef()));
      }
    }
    KJ_CASE_ONEOF(explicitTxn, ExplicitTxn*) {
      if (!explicitTxn->isSomeWriteConfirmed() && !allowUnconfirmed) {
        // This is adding a must-confirm write to the transaction, so we must ensure the outputGate
        // locks for remainder of this transaction.
        explicitTxn->setSomeWriteConfirmed(!allowUnconfirmed);
        // ExplicitTxns don't have a pending commit and don't lock the output gate during the
        // transaction, so there's nothing to do here.
      }
    }
  }
}

kj::Promise<void> ActorSqlite::requestScheduledAlarm(
    kj::Maybe<kj::Date> requestedTime, kj::Promise<void> priorTask) {
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

  return hooks.scheduleRun(requestedTime, kj::mv(priorTask))
      .then([this, movingAlarmLater, requestedTime]() {
    if (!movingAlarmLater) {
      alarmScheduledNoLaterThan = requestedTime;
    }
  });
}

ActorSqlite::PrecommitAlarmState ActorSqlite::startPrecommitAlarmScheduling() {
  PrecommitAlarmState state;
  if (pendingCommit == kj::none &&
      willFireEarlier(metadata.getAlarm(), alarmScheduledNoLaterThan)) {
    // We must wait on the `alarmLaterChain` here, otherwise, if there is a pending "move later"
    // alarm task and it fails, our "move earlier" alarm might interleave, succeed, and be followed
    // by a retry of the "move later" alarm. This happens because "move later" alarms complete after
    // we commit to local SQLite.
    //
    // By waiting on any pending "move later" alarm, we correctly serialize our `scheduleRun()`
    // calls to the alarm manager.
    state.schedulingPromise =
        requestScheduledAlarm(metadata.getAlarm(), alarmLaterChain.addBranch());
  }
  return kj::mv(state);
}

kj::Promise<void> ActorSqlite::commitImpl(
    ActorSqlite::PrecommitAlarmState precommitAlarmState, SpanParent parentSpan) {
  auto commitSpan = parentSpan.newChild("actor_sqlite_commit"_kjc);

  // We assume that exceptions thrown during commit will propagate to the caller, such that they
  // will ensure cancelDeferredAlarmDeletion() is called, if necessary.

  bool haveAlarmForDebug = false;

  if (debugAlarmSync) {
    KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: commitImpl entered", (pendingCommit != kj::none),
        alarmVersion, logDate(metadata.getAlarm()));
  }

  KJ_IF_SOME(pending, pendingCommit) {
    // If an earlier commitImpl() invocation is already in the process of updating precommit
    // alarms but has not yet made the commitCallback() call, it should be OK to wait on it to
    // perform the precommit alarm update and db commit for this invocation, too.
    kj::Maybe<kj::Date> alarmBeforeMerge;
    if (debugAlarmSync) {
      alarmBeforeMerge = metadata.getAlarm();
      KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Commit merge waiting", logDate(alarmBeforeMerge),
          alarmVersion);
    }
    co_await pending.addBranch();
    if (debugAlarmSync) {
      auto alarmAfterMerge = metadata.getAlarm();
      KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Commit merge resumed", logDate(alarmBeforeMerge),
          logDate(alarmAfterMerge), alarmVersion);
    }
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
    haveAlarmForDebug = true;
    co_await p;
  }

  // While the local db state requires an earlier alarm than is known might be scheduled, issue an
  // alarm update request for the earlier time and wait for it to complete.  This helps ensure
  // that the successfully scheduled alarm time is always earlier or equal to the alarm state in
  // the successfully persisted db.
  int syncIterations = 0;
  auto startAlarmState = metadata.getAlarm();
  while (willFireEarlier(metadata.getAlarm(), alarmScheduledNoLaterThan)) {
    if (debugAlarmSync) {
      haveAlarmForDebug = true;
      auto currentAlarmState = metadata.getAlarm();
      KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Move earlier loop iteration", syncIterations,
          logDate(currentAlarmState), logDate(alarmScheduledNoLaterThan), alarmVersion);
    }
    // Note that we do not pass alarmLaterChain here. We don't need to for the following reasons:
    //
    //  1. We already waited for the chain in the precommitAlarmState promise above.
    //  2. We set the `pendingCommit` prior to yielding to the event loop earlier, so any subsequent
    //     commits have to wait for us to fulfill the pendingCommit promise. In short, no one could
    //     have added another "move-later" alarm to the chain, not until we finish.
    //
    // While we *could* pass the alarmLaterChain promise (it wouldn't be incorrect), when calling
    // addBranch() on a resolved ForkedPromise, the continuation would be evaluated on a future turn
    // of the event loop. That means we're going to suspend, even if the promise is ready, which
    // means we'd take a performance hit.
    co_await requestScheduledAlarm(metadata.getAlarm(), kj::READY_NOW);
    syncIterations++;
  }
  if (debugAlarmSync && syncIterations > 0) {
    KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Move earlier loop complete", logDate(startAlarmState),
        "ended_with", logDate(metadata.getAlarm()), "iterations", syncIterations, alarmVersion);
  }

  // Issue the commitCallback() request to persist the db state, then synchronously clear the
  // pending commit so that the next commitImpl() invocation starts its own set of precommit alarm
  // updates and db commit.
  auto alarmStateForCommit = metadata.getAlarm();

  // Capture the alarm version before going async to detect concurrent alarm changes. If the
  // alarmVersion changes while we are in-flight, we should skip attempting any move-later alarm
  // update.
  auto alarmVersionBeforeAsync = alarmVersion;

  if (debugAlarmSync) {
    KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Captured state before persisting to SQLite async",
        logDate(alarmStateForCommit), alarmVersionBeforeAsync);
  }

  auto commitCallbackPromise = commitCallback();
  pendingCommit = kj::none;

  // Wait for the db to persist.
  co_await commitCallbackPromise;
  lastConfirmedAlarmDbState = alarmStateForCommit;

  if (debugAlarmSync && haveAlarmForDebug) {
    KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Persisted in SQLite", "sqlite_has",
        logDate(alarmStateForCommit), "alarmScheduledNoLaterThan",
        logDate(alarmScheduledNoLaterThan), alarmVersion);
  }

  // Notify any merged commitImpl() requests that the db persistence completed.
  fulfiller->fulfill();

  if (debugAlarmSync) {
    KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Version check", alarmVersionBeforeAsync, alarmVersion,
        "match", (alarmVersion == alarmVersionBeforeAsync));
  }
  // If another commit modified the alarm while we were async, skip post-commit alarm sync.
  //
  // We do this for a few reasons:
  //  1. The other commit will handle its own alarm sync
  //  2. Post-commit syncs are inherently optional (the alarm will self-correct)
  //  3. This coalesces redundant alarm updates for better performance
  //  4. This avoids race conditions where a later commit moved the alarm earlier, requiring a
  //     pre-commit alarm update, and this update may have already been made before we get here.
  if (alarmVersion == alarmVersionBeforeAsync) {
    // No intervening alarm changes, it is safe to schedule a move-later alarm update if needed.
    if (willFireEarlier(alarmScheduledNoLaterThan, alarmStateForCommit)) {
      if (debugAlarmSync) {
        KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: Moving alarm later", "sqlite_has",
            logDate(alarmStateForCommit), logDate(alarmScheduledNoLaterThan), alarmVersion);
      }
      // We need to extend our alarmLaterChain now that we're adding a new "move-later" alarm task.
      //
      // Technically, we don't need serialize our "move-later" alarms since SQLite has the later
      // time committed locally. We could just set the `alarmLaterChain` and pass a `kj::READY_NOW`
      // to requestScheduledAlarm, and so if we have a partial failure we would just recover when
      // the alarm runs early. That said, it doesn't hurt to serialize on the client-side.
      alarmLaterChain = requestScheduledAlarm(alarmStateForCommit, alarmLaterChain.addBranch())
                            .catch_([](kj::Exception&& e) {
        // If an exception occurs when scheduling the alarm later, it's OK -- the alarm will
        // eventually fire at the earlier time, and the rescheduling will be retried.
        // We catch here to prevent the chain from breaking on errors.
        LOG_WARNING_PERIODICALLY("NOSENTRY SQLite reschedule later alarm failed", e);
      }).fork();
    }
  }
}

void ActorSqlite::taskFailed(kj::Exception&& exception) {
  // The output gate should already have been broken since it wraps all commit tasks that can
  // throw. So, we don't have to report anything here, the exception will already propagate
  // elsewhere. We should block further operations, though.
  if (broken == kj::none) {
    broken = kj::mv(exception);
    if (!outputGate.isBroken()) {
      LOG_PERIODICALLY(
          ERROR, "SQLite actor recorded broken exception without breaking output gate");
    }
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
    // If we have reached this point, the client is destroying its DeferredAlarmDeleter at the end
    // of an alarm handler run, and deletion hasn't been cancelled, indicating that the handler
    // returned success.
    //
    // If the output gate has somehow broken in the interim, attempting to write the deletion here
    // will cause the DeferredAlarmDeleter destructor to throw, which the caller probably isn't
    // expecting.  So we'll skip the deletion attempt, and let the caller detect the gate
    // brokenness through other means.
    if (broken == kj::none) {
      // Use the span captured in armAlarmHandler() for this internal write, since
      // metadata.setAlarm() doesn't go through the regular write path with a traceSpan parameter.
      currentCommitSpan = kj::mv(deferredAlarmSpan);
      // the safe thing to do is to require confirmation.
      if (metadata.setAlarm(kj::none, /*allowUnconfirmed=*/false)) {
        ++alarmVersion;
        if (debugAlarmSync) {
          KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: maybeDeleteDeferredAlarm cleared alarm",
              alarmVersion);
        }
      }
    }
    haveDeferredDelete = false;
    deferredAlarmSpan = nullptr;
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

kj::Maybe<kj::Promise<void>> ActorSqlite::put(
    Key key, Value value, WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  // Capture trace span for the output gate lock hold trace.
  currentCommitSpan = kj::mv(traceSpan);
  kv.put(key, value, {.allowUnconfirmed = options.allowUnconfirmed});
  return kj::none;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  // Capture trace span for the output gate lock hold trace.
  currentCommitSpan = kj::mv(traceSpan);
  if (currentTxn.is<NoTxn>()) {
    // If we are not in a transaction, start an ImplicitTxn since that's what would happen on the
    // first write anyway.
    startImplicitTxn();
  }

  KJ_ASSERT(!currentTxn.is<NoTxn>());

  kv.put(pairs, {.allowUnconfirmed = options.allowUnconfirmed});

  return kj::none;
}

kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::delete_(
    Key key, WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  // Capture trace span for the output gate lock hold trace.
  currentCommitSpan = kj::mv(traceSpan);

  return kv.delete_(key, {.allowUnconfirmed = options.allowUnconfirmed});
}

kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::delete_(
    kj::Array<Key> keys, WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  // Capture trace span for the output gate lock hold trace.
  currentCommitSpan = kj::mv(traceSpan);

  uint count = 0;
  for (auto& key: keys) {
    count += kv.delete_(key, {.allowUnconfirmed = options.allowUnconfirmed});
  }
  return count;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  // Capture trace span for the output gate lock hold trace.
  currentCommitSpan = kj::mv(traceSpan);

  // TODO(someday): When deleting alarm data in an otherwise empty database, clear the database to
  // free up resources?

  // Only increment version counter if the alarm value actually changed. This is important because
  // if the value didn't change, no SQLite write occurs, so no implicit transaction is started,
  // and we don't want to invalidate in-flight commits without a replacement commit.
  if (metadata.setAlarm(newAlarmTime, options.allowUnconfirmed)) {
    ++alarmVersion;
    if (debugAlarmSync) {
      KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: setAlarm called", logDate(newAlarmTime), alarmVersion);
    }
  }

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

ActorCacheInterface::DeleteAllResults ActorSqlite::deleteAll(
    WriteOptions options, SpanParent traceSpan) {
  requireNotBroken();
  disableAllowUnconfirmed(options, "deleteAll is not supported");

  // Capture trace span for the output gate lock (deleteAll always requires confirmation).
  currentCommitSpan = kj::mv(traceSpan);

  // kv.deleteAll() clears the database, so we need to save and possibly restore alarm state in
  // the metadata table, to try to match the behavior of ActorCache, which preserves the set alarm
  // when running deleteAll().
  auto localAlarmState = metadata.getAlarm();
  if (localAlarmState != kj::none) {
    LOG_WARNING_PERIODICALLY("NOSENTRY deleteAll() called on ActorSqlite with an alarm still set");
  }

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

  if (!deleteAllCommitScheduled) {
    // Make sure a commit callback is queued for the deleteAll().
    commitTasks.add(outputGate.lockWhile(kj::evalLater([this]() mutable -> kj::Promise<void> {
      // Don't commit if shutdown() has been called.
      requireNotBroken();

      deleteAllCommitScheduled = false;
      if (currentTxn.is<ImplicitTxn*>()) {
        // An implicit transaction is already scheduled, so we'll count on it to perform a commit when it's
        // done. This is particularly important for the case where deleteAll() was called while an alarm
        // is outstanding; resetting the alarm state (below) starts an implicit transaction.
        // We don't want to commit the deletion without that transaction.
        return kj::READY_NOW;
      } else {
        return commitCallback();
      }
    }),
        currentCommitSpan.addRef()));
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
    if (metadata.setAlarm(localAlarmState, options.allowUnconfirmed)) {
      ++alarmVersion;
      if (debugAlarmSync) {
        KJ_LOG(WARNING, "NOSENTRY DEBUG_ALARM: deleteAll restored alarm", logDate(localAlarmState),
            alarmVersion);
      }
    }
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
    // We've already experienced a terminal exception either from shutdown or OOM, there should
    // already be a flush scheduled that will break the output gate.
  }
}

kj::OneOf<ActorSqlite::CancelAlarmHandler, ActorSqlite::RunAlarmHandler> ActorSqlite::
    armAlarmHandler(kj::Date scheduledTime,
        SpanParent parentSpan,
        kj::Date currentTime,
        bool noCache,
        kj::StringPtr actorId) {
  KJ_ASSERT(!inAlarmHandler);

  if (haveDeferredDelete) {
    // Unlikely to happen, unless caller is starting new alarm handler before previous alarm
    // handler cleanup has completed.
    LOG_WARNING_ONCE("expected previous alarm handler to be cleaned up");
  }

  auto localAlarmState = metadata.getAlarm();
  if (localAlarmState != scheduledTime) {
    if (localAlarmState == lastConfirmedAlarmDbState) {
      // If the local alarm time is already in the past, just run the handler now. This avoids
      // blocking alarm execution on the AlarmManager sync when storage is overloaded. The alarm
      // will either delete itself on success or reschedule on failure.
      if ((willFireEarlier(localAlarmState, currentTime))) {
        auto localAlarmTime = KJ_ASSERT_NONNULL(localAlarmState);
        LOG_WARNING_PERIODICALLY(
            "NOSENTRY SQLite alarm overdue, running despite AlarmManager mismatch", scheduledTime,
            localAlarmTime, currentTime, actorId);
        haveDeferredDelete = true;
        inAlarmHandler = true;
        deferredAlarmSpan = kj::mv(parentSpan);
        static const DeferredAlarmDeleter disposer;
        return RunAlarmHandler{.deferredDelete = kj::Own<void>(this, disposer)};
      }

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
        LOG_WARNING_PERIODICALLY(
            "NOSENTRY SQLite alarm handler canceled with requestScheduledAlarm.", scheduledTime,
            localAlarmState.orDefault(kj::UNIX_EPOCH), actorId);

        // Since we're requesting to move the alarm time to later, we need to add to our
        // `alarmLaterChain`. Note that for the chain, we want to make sure any scheduling failure
        // does not break us, but for the `CancelAlarmHandler`, we want the caller to receive the
        // exception normally, so we do not consume the exception.
        auto schedulingPromise =
            requestScheduledAlarm(localAlarmState, alarmLaterChain.addBranch()).fork();
        alarmLaterChain = schedulingPromise.addBranch()
                              .catch_([](kj::Exception&& e) {
          // If an exception occurs when scheduling the alarm later, it's OK -- the alarm will
          // eventually fire at the earlier time, and the rescheduling will be retried.
          // We catch here to prevent the chain from breaking on errors.
          LOG_WARNING_PERIODICALLY("NOSENTRY SQLite reschedule later alarm failed", e);
        }).fork();
        return CancelAlarmHandler{.waitBeforeCancel = schedulingPromise.addBranch()};
      } else {
        // We have a clean local alarm time that is earlier than the handler's scheduled time,
        // which suggests that either the alarm manager is working with stale data or that local
        // alarm time has somehow gotten out of sync with the scheduled alarm time.

        // We know localAlarmState has a value here because we're in the branch where it's earlier
        // than scheduledTime (not equal, and not later).
        auto localTime = KJ_ASSERT_NONNULL(localAlarmState);

        // Only log if the alarm manager is significantly late (>10 seconds behind SQLite)
        if (scheduledTime - localTime > 10 * kj::SECONDS) {
          LOG_WARNING_PERIODICALLY(
              "NOSENTRY SQLite alarm handler canceled.", scheduledTime, actorId, localTime);
        }

        // Tell the caller to wait for successful rescheduling before cancelling the current
        // handler invocation.
        //
        // We pass kj::READY_NOW because being in this branch (SQLite is ahead of the alarm manager)
        // means there's no recent move-later operation to wait for, so no need for alarmLaterChain.
        return CancelAlarmHandler{
          .waitBeforeCancel = requestScheduledAlarm(localAlarmState, kj::READY_NOW)};
      }
    } else {
      // There's a alarm write that hasn't been set yet pending for a time different than ours --
      // We won't cancel the alarm because it hasn't been confirmed, but we shouldn't delete
      // the pending write.
      haveDeferredDelete = false;
    }
  } else {
    haveDeferredDelete = true;
    deferredAlarmSpan = kj::mv(parentSpan);
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

kj::Maybe<kj::Promise<void>> ActorSqlite::onNoPendingFlush(SpanParent parentSpan) {
  // This implements sync().
  //
  // sync() should wait for ALL writes (both confirmed and unconfirmed) that are outstanding at the
  // time sync() is called. We use lastCommit which keeps track of the most recent commit to be
  // formed. We join with the outputGate because there are a lot of edge cases where we break the
  // output gate and it's easiest to catch all of those instances here rather than updating
  // everything to also break lastCommit.
  return kj::joinPromisesFailFast(
      kj::arr(lastCommit.addBranch(), outputGate.wait(kj::mv(parentSpan))));
}

kj::Promise<kj::String> ActorSqlite::getCurrentBookmark(SpanParent parentSpan) {
  // This is an ersatz implementation that's good enough for local dev with D1's Session API.
  //
  // The returned bookmark satisfies the properties that D1 cares about:
  //
  // * Later bookmarks sort after earlier bookmarks.  We implement this by incrementing the bookmark
  // * whenever getCurrentBookmark() is called.
  //
  // * Bookmarks from the current workerd session sort after bookmarks from previous sessions.  We
  //   implement this by saving an ersatz bookmark in the SqliteMetadata table.

  requireNotBroken();
  uint64_t bookmark = 0;
  KJ_IF_SOME(b, metadata.getLocalDevelopmentBookmark()) {
    bookmark = b + 1;
  }
  metadata.setLocalDevelopmentBookmark(bookmark);

  // TODO(cleanup): Left-padded number stringification should maybe be in KJ?
  auto paddedHex = [](uint32_t n) {
    kj::FixedArray<char, 8> result;
    for (auto i = 0; i < result.size(); i++) {
      char digit = n % 16;
      n /= 16;
      digit += digit < 10 ? '0' : ('a' - 10);
      result[result.size() - 1 - i] = digit;
    }
    return result;
  };

  // Turn the bookmark into a format matching what Cloudflare's production returns.
  constexpr uint32_t uint32_max = kj::maxValue;
  kj::FixedArray<char, 32> pad;
  pad.fill('0');
  return kj::str(paddedHex(bookmark / uint32_max), '-', paddedHex(bookmark % uint32_max), '-',
      paddedHex(0), '-', pad);
}

kj::Promise<void> ActorSqlite::waitForBookmark(kj::StringPtr bookmark, SpanParent parentSpan) {
  // This is an ersatz implementation that's good enough for local dev with D1's Session API.
  requireNotBroken();
  return kj::READY_NOW;
}

void ActorSqlite::TxnCommitRegulator::onError(
    kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const {
  KJ_IF_SOME(c, sqliteErrorCode) {
    // We cannot `#include <sqlite3.h>` in the same compilation unit as `#include
    // <workerd/io/trace.h>` because the latter includes v8 and v8 seems to conflict with sqlite.
    // So we copy the value of SQLITE_CONSTRAINT from sqlite3.h
    constexpr int SQLITE_CONSTRAINT = 19;
    if (c == SQLITE_CONSTRAINT) {
      JSG_ASSERT(false, Error,
          "Durable Object was reset and rolled back to its last known good state because the "
          "application left the database in a state where constraints were violated: ",
          message);
    }
  }

  // For any other type of error, fall back to the default behavior (throwing a non-JSG exception)
  // as we don't know for sure that the problem is the application's fault.
}

const ActorSqlite::Hooks ActorSqlite::Hooks::DEFAULT = ActorSqlite::Hooks{};

kj::Promise<void> ActorSqlite::Hooks::scheduleRun(
    kj::Maybe<kj::Date> newAlarmTime, kj::Promise<void> priorTask) {
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
    Key key, Value value, WriteOptions options, SpanParent traceSpan) {
  return actorSqlite.put(kj::mv(key), kj::mv(value), options, kj::mv(traceSpan));
}
kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options, SpanParent traceSpan) {
  return actorSqlite.put(kj::mv(pairs), options, kj::mv(traceSpan));
}
kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::ExplicitTxn::delete_(
    Key key, WriteOptions options, SpanParent traceSpan) {
  return actorSqlite.delete_(kj::mv(key), options, kj::mv(traceSpan));
}
kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::ExplicitTxn::delete_(
    kj::Array<Key> keys, WriteOptions options, SpanParent traceSpan) {
  return actorSqlite.delete_(kj::mv(keys), options, kj::mv(traceSpan));
}
kj::Maybe<kj::Promise<void>> ActorSqlite::ExplicitTxn::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options, SpanParent traceSpan) {
  return actorSqlite.setAlarm(newAlarmTime, options, kj::mv(traceSpan));
}

}  // namespace workerd
