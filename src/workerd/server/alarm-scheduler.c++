// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alarm-scheduler.h"

namespace workerd::server {

int AlarmScheduler::maxJitterMsForDelay(kj::Duration delay) {
  double delayMs = delay / kj::MILLISECONDS;
  return std::floor(RETRY_JITTER_FACTOR * delayMs);
}

namespace {

std::default_random_engine makeSeededRandomEngine() {
  // Using the time as a seed here is fine, we just want to have some randomness for retry jitter
  auto time = kj::systemPreciseMonotonicClock().now();
  auto seed = (time - kj::origin<kj::TimePoint>()) / kj::NANOSECONDS;

  std::default_random_engine engine(seed);
  return engine;
}

} // namespace

AlarmScheduler::AlarmScheduler(
    const kj::Clock& clock,
    kj::Timer& timer,
    const SqliteDatabase::Vfs& vfs,
    kj::PathPtr path)
    : clock(clock), timer(timer), random(makeSeededRandomEngine()),
      db([&]{
        auto db = kj::heap<SqliteDatabase>(vfs, path,
            kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
        ensureInitialized(*db);
        return kj::mv(db);
      }()),
      tasks(*this) {
    loadAlarmsFromDb();
  }

void AlarmScheduler::ensureInitialized(SqliteDatabase& db) {
  // TODO(sqlite): Do this automatically at a lower layer?
  db.run("PRAGMA journal_mode=WAL;");

  db.run(R"(
    CREATE TABLE IF NOT EXISTS _cf_ALARM (
      actor_unique_key TEXT,
      actor_id TEXT,
      scheduled_time INTEGER,
      PRIMARY KEY (actor_unique_key, actor_id)
    ) WITHOUT ROWID;
  )");
}

void AlarmScheduler::loadAlarmsFromDb() {
  auto now = clock.now();

  // TODO(someday): don't maintain the entire alarm set in memory -- right now for the usecase of
  // local development, doing so is sufficent.
  auto query = db->run(R"(
    SELECT actor_unique_key, actor_id, scheduled_time FROM _cf_ALARM;
  )");

  while (!query.isDone()) {
    auto date = kj::UNIX_EPOCH + (kj::NANOSECONDS * query.getInt64(2));

    auto ownUniqueKey = kj::str(query.getText(0));
    auto ownActorId = kj::str(query.getText(1));
    auto actor = kj::attachVal(ActorKey { .uniqueKey = ownUniqueKey, .actorId = ownActorId },
        kj::mv(ownUniqueKey), kj::mv(ownActorId));

    alarms.insert(*actor, scheduleAlarm(now, kj::mv(actor), date));

    query.nextRow();
  }
}

void AlarmScheduler::registerNamespace(kj::StringPtr uniqueKey, GetActorFn getActor) {
  namespaces.insert(uniqueKey, Namespace{
    .getActor = kj::mv(getActor)
  });
}

kj::Maybe<kj::Date> AlarmScheduler::getAlarm(ActorKey actor) {
  KJ_IF_MAYBE(alarm, alarms.find(actor)) {
    if (alarm->status == AlarmStatus::STARTED) {
      // getAlarm() when the alarm handler is running should return null,
      // unless an alarm is queued;
      return alarm->queuedAlarm;
    } else {
      return alarm->scheduledTime;
    }
  } else {
    // We currently retain the entire set of queued alarms in memory, no need to hit sqlite
    return nullptr;
  }
}

bool AlarmScheduler::setAlarm(ActorKey actor, kj::Date scheduledTime) {
  int64_t scheduledTimeNs = (scheduledTime - kj::UNIX_EPOCH) / kj::NANOSECONDS;
  auto query = stmtSetAlarm.run(actor.uniqueKey, actor.actorId, scheduledTimeNs);

  bool existing = true;
  auto& entry = alarms.findOrCreate(actor, [&]() {
    existing = false;

    auto ownUniqueKey = kj::str(actor.uniqueKey);
    auto ownActorId = kj::str(actor.actorId);
    auto ownActor = kj::attachVal(ActorKey { .uniqueKey = ownUniqueKey, .actorId = ownActorId },
    kj::mv(ownUniqueKey), kj::mv(ownActorId));

    return decltype(alarms)::Entry {
        *ownActor, scheduleAlarm(clock.now(), kj::mv(ownActor), scheduledTime) };
  });

  if (existing) {
    if (entry.status != AlarmStatus::WAITING) {
      // We queue any new alarm after the existing alarm even if the new alarm has the same scheduled
      // time, as receiving a notification directly maps to a write for that time in the actor.
      entry.queuedAlarm = scheduledTime;
    } else {
      entry = scheduleAlarm(clock.now(), kj::mv(entry.actor), scheduledTime);
    }
  }

  return query.changeCount() > 0;
}

bool AlarmScheduler::deleteAlarm(ActorKey actor) {
  auto query = stmtDeleteAlarm.run(actor.uniqueKey, actor.actorId);

  KJ_IF_MAYBE(entry, alarms.findEntry(actor)) {
    KJ_IF_MAYBE(queued, entry->value.queuedAlarm) {
      if ((*entry).value.status == AlarmStatus::STARTED) {
        // If we are currently running an alarm, we want to delete the queued instead of current.
        entry->value.queuedAlarm = nullptr;
      } else {
        entry->value = scheduleAlarm(clock.now(), kj::mv(entry->value.actor), *queued);
      }
    } else {
      if ((*entry).value.status != AlarmStatus::STARTED) {
        // We can't remove running alarms.
        alarms.erase(*entry);
      }
    }
  }

  return query.changeCount() > 0;
}

kj::Promise<AlarmScheduler::RetryInfo> AlarmScheduler::runAlarm(
    const ActorKey& actor, kj::Date scheduledTime) {
  KJ_IF_MAYBE(ns, namespaces.find(actor.uniqueKey)) {
    auto result = co_await ns->getActor(kj::str(actor.actorId))->runAlarm(scheduledTime);

    co_return RetryInfo {
      .retry = result.outcome != EventOutcome::OK && result.retry,
      .retryCountsAgainstLimit = result.retryCountsAgainstLimit
    };
  } else {
    throw KJ_EXCEPTION(FAILED, "uniqueKey for stored alarm was not registered?");
  }
}

AlarmScheduler::ScheduledAlarm AlarmScheduler::scheduleAlarm(
    kj::Date now, kj::Own<ActorKey> actor, kj::Date scheduledTime) {
  auto task = makeAlarmTask(scheduledTime - now, *actor, scheduledTime);

  return ScheduledAlarm { kj::mv(actor), scheduledTime, kj::mv(task) };
}

kj::Promise<void> AlarmScheduler::checkTimestamp(kj::Duration delay, kj::Date scheduledTime) {
  co_await timer.afterDelay(delay);

  // Since we are waiting on timer.afterDelay, it's possible that timer.now() was behind
  // the real time by a few ms, leading to premature alarm() execution. This checks it the current
  // time is >= than scheduledTime to ensure we run alarms only on or after their scheduled time.
  auto now = clock.now();
  if (now < scheduledTime) {
    // If it's not yet time to trigger the alarm, we shall wait a while longer until we can
    // trigger it. This repeats until it's time for the alarm to run.
    co_await checkTimestamp(scheduledTime - now, scheduledTime);
  }
}

kj::Promise<void> AlarmScheduler::makeAlarmTask(kj::Duration delay,
                                                const ActorKey& actorRef,
                                                kj::Date scheduledTime) {
  co_await checkTimestamp(delay, scheduledTime);

  {
    auto& entry = KJ_ASSERT_NONNULL(alarms.findEntry(actorRef));
    entry.value.status = AlarmStatus::STARTED;
  }

  auto retryInfo = co_await ([&]() -> kj::Promise<RetryInfo> {
    try {
      co_return co_await runAlarm(actorRef, scheduledTime);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_LOG(WARNING, exception);
      co_return RetryInfo {
        .retry = true,

        // An exception here is "weird", they should normally
        // be turned into AlarmResult statuses in the sandbox
        // for any user-caused error. Let's not count this
        // retry attempt against the limit.
        .retryCountsAgainstLimit = false
      };
    }
  })();

  try {
    auto& entry = KJ_ASSERT_NONNULL(alarms.findEntry(actorRef));

    // We can't overwrite our entry before moving ourselves out of it, as a promise cannot
    // delete itself.
    tasks.add(kj::mv(entry.value.task));

    // If an alarm is queued, there's no point in retrying the current one -- proceed
    // to running the queued alarm instead.
    KJ_IF_MAYBE(a, entry.value.queuedAlarm) {
      // creating a new alarm and overwriting the old one will reset
      // `status` to WAITING and `queuedAlarm` to null
      entry.value = scheduleAlarm(clock.now(), kj::mv(entry.value.actor), *a);
      co_return;
    }

    // When we reach this block of code and alarm has either successed or failed and may (or may
    // not) retry. Setting the status of an alarm as FINISHED here, will allow deletion of alarms
    // between retries. If there's a retry, `makeAlarmTask` is called, setting status as RUNNING
    // again.
    entry.value.status = AlarmStatus::FINISHED;

    if (retryInfo.retry) {
      // recreate the task, running after a delay determined using the retry factor
      if (entry.value.countedRetry >= AlarmScheduler::RETRY_MAX_TRIES) {
        deleteAlarm(*entry.value.actor);
        co_return;
      }
      if (retryInfo.retryCountsAgainstLimit) {
        entry.value.countedRetry++;

        if (!entry.value.previousRetryCountedAgainstLimit) {
          // The last retry didn't count against the limit, indicating it was due to some internal
          // error. However, this retry does, meaning it's due to an error in user code,
          // most likely a different error. We should reset the retry counter used for
          // calculating backoff, so user-caused retries don't have an unnecessarily high backoff
          // time if they come after internal-caused retries.

          entry.value.backoff = 0;
        }
      }
      entry.value.previousRetryCountedAgainstLimit = retryInfo.retryCountsAgainstLimit;

      entry.value.backoff = kj::min(AlarmScheduler::RETRY_BACKOFF_MAX, entry.value.backoff);
      auto delay = (AlarmScheduler::RETRY_START_SECONDS << entry.value.backoff) * kj::SECONDS;

      std::uniform_int_distribution<> distribution(0, maxJitterMsForDelay(delay));
      delay += distribution(random) * kj::MILLISECONDS;

      entry.value.backoff++;
      entry.value.retry++;

      entry.value.task = makeAlarmTask(delay, actorRef, scheduledTime);
    } else {
      KJ_ASSERT(entry.value.queuedAlarm == nullptr);
      deleteAlarm(actorRef);
    }
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_LOG(ERROR, "Failed to run alarm and was unable to schedule a retry", exception);
  }
}

void AlarmScheduler::taskFailed(kj::Exception&& e) {
  KJ_LOG(WARNING, e);
}

}  // namespace workerd::server
