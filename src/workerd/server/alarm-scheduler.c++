// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alarm-scheduler.h"

#include <kj/debug.h>

#include <cmath>

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

enum class AlarmTable { ABSENT, WITHOUT_NAME, WITH_NAME };

// Whether `db` has the `_cf_ALARM` table, and whether that has the `actor_name` column.
AlarmTable inspectAlarmTable(SqliteDatabase& db) {
  auto info = db.run("PRAGMA table_info(_cf_ALARM);");
  if (info.isDone()) {
    return AlarmTable::ABSENT;
  }
  while (!info.isDone()) {
    // Column 1 of table_info output is the column name.
    if (info.getText(1) == "actor_name"_kj) {
      return AlarmTable::WITH_NAME;
    }
    info.nextRow();
  }
  return AlarmTable::WITHOUT_NAME;
}

// The thread whose scheduler database `name` is, the inverse of `AlarmScheduler::databasePath()`,
// or none if `name` is not such a database.
kj::Maybe<uint> parseDatabaseName(kj::StringPtr name) {
  if (name == "metadata.sqlite"_kj) {
    return 0u;
  }
  constexpr auto prefix = "metadata-"_kj;
  constexpr auto suffix = ".sqlite"_kj;
  if (!name.startsWith(prefix) || !name.endsWith(suffix)) {
    return kj::none;
  }
  auto digits = name.slice(prefix.size(), name.size() - suffix.size());
  if (digits.size() == 0 || digits[0] < '1' || digits[0] > '9') {
    return kj::none;
  }
  uint64_t thread = 0;
  for (char c: digits) {
    if (c < '0' || c > '9') {
      return kj::none;
    }
    thread = thread * 10 + (c - '0');
    if (thread > static_cast<uint>(kj::maxValue)) {
      return kj::none;
    }
  }
  return static_cast<uint>(thread);
}

}  // namespace

AlarmScheduler::AlarmScheduler(const kj::Clock& clock,
    kj::Timer& timer,
    const SqliteDatabase::Vfs& vfs,
    kj::Path path,
    GetActorFn getActor)
    : clock(clock),
      timer(timer),
      random(makeSeededRandomEngine()),
      getActor(kj::mv(getActor)),
      db([&] {
        auto db = kj::heap<SqliteDatabase>(vfs, kj::mv(path),
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
      actor_id TEXT PRIMARY KEY,
      scheduled_time INTEGER,
      actor_name TEXT
    ) WITHOUT ROWID;
  )");

  // Migrate databases created before the `actor_name` column existed. SQLite has no
  // "ADD COLUMN IF NOT EXISTS", so detect the column via PRAGMA table_info first.
  if (inspectAlarmTable(db) != AlarmTable::WITH_NAME) {
    db.run("ALTER TABLE _cf_ALARM ADD COLUMN actor_name TEXT;");
  }
}

kj::Path AlarmScheduler::databasePath(uint thread) {
  if (thread == 0) {
    return kj::Path("metadata.sqlite"_kj);
  }
  return kj::Path(kj::str("metadata-", thread, ".sqlite"));
}

void AlarmScheduler::rehome(const SqliteDatabase::Vfs& vfs,
    const kj::Directory& directory,
    uint threads,
    kj::Function<uint(kj::StringPtr actorId)> home) {
  KJ_REQUIRE(threads > 0);

  struct Row {
    kj::String actorId;
    int64_t scheduledTime;
    kj::Maybe<kj::String> name;
  };
  struct Source {
    uint thread;
    // The actors of the rows that move out of this database.
    kj::Vector<kj::String> movedOut;
  };

  kj::Vector<Source> sources;
  for (auto& name: directory.listNames()) {
    KJ_IF_SOME(thread, parseDatabaseName(name)) {
      sources.add(Source{.thread = thread, .movedOut = {}});
    }
  }

  auto incomingBuilder = kj::heapArrayBuilder<kj::Vector<Row>>(threads);
  for (uint i = 0; i < threads; i++) {
    incomingBuilder.add();
  }
  auto incoming = incomingBuilder.finish();

  // A database of a thread past `threads` is removed even when it has no rows.
  bool changes = false;

  for (auto& source: sources) {
    if (source.thread >= threads) {
      changes = true;
    }

    SqliteDatabase db(vfs, databasePath(source.thread), kj::WriteMode::MODIFY);
    auto collect = [&](SqliteDatabase::Query& query) {
      while (!query.isDone()) {
        auto actorId = query.getText(0);
        uint to = home(actorId);
        KJ_REQUIRE(to < threads, "an actor's home thread must be one of the run's", to, threads);
        if (to != source.thread) {
          incoming[to].add(Row{
            .actorId = kj::str(actorId),
            .scheduledTime = query.getInt64(1),
            .name = query.getMaybeText(2).map([](kj::StringPtr n) { return kj::str(n); }),
          });
          source.movedOut.add(kj::str(actorId));
          changes = true;
        }
        query.nextRow();
      }
    };
    switch (inspectAlarmTable(db)) {
      case AlarmTable::ABSENT:
        break;
      case AlarmTable::WITHOUT_NAME: {
        auto query = db.run("SELECT actor_id, scheduled_time, NULL FROM _cf_ALARM;");
        collect(query);
        break;
      }
      case AlarmTable::WITH_NAME: {
        auto query = db.run("SELECT actor_id, scheduled_time, actor_name FROM _cf_ALARM;");
        collect(query);
        break;
      }
    }
  }

  if (!changes) {
    return;
  }

  // Every row is written to its destination before any is deleted from its source.
  for (uint to = 0; to < threads; to++) {
    auto& rows = incoming[to];
    if (rows.empty()) {
      continue;
    }
    SqliteDatabase db(vfs, databasePath(to), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    ensureInitialized(db);
    db.run("BEGIN TRANSACTION;");
    {
      auto stmt = db.prepare(R"(
        INSERT INTO _cf_ALARM VALUES(?, ?, ?)
          ON CONFLICT DO UPDATE SET
            scheduled_time = MIN(_cf_ALARM.scheduled_time, excluded.scheduled_time),
            actor_name = COALESCE(_cf_ALARM.actor_name, excluded.actor_name);
      )");
      for (auto& row: rows) {
        SqliteDatabase::Query::ValuePtr nameParam = nullptr;
        KJ_IF_SOME(n, row.name) {
          nameParam = n.asPtr();
        }
        stmt.run(row.actorId.asPtr(), row.scheduledTime, nameParam);
      }
    }
    db.run("COMMIT TRANSACTION;");
  }

  for (auto& source: sources) {
    if (source.thread >= threads || source.movedOut.empty()) {
      continue;
    }
    SqliteDatabase db(vfs, databasePath(source.thread), kj::WriteMode::MODIFY);
    db.run("BEGIN TRANSACTION;");
    {
      auto stmt = db.prepare("DELETE FROM _cf_ALARM WHERE actor_id = ?;");
      for (auto& actorId: source.movedOut) {
        stmt.run(actorId.asPtr());
      }
    }
    db.run("COMMIT TRANSACTION;");
  }

  // Every database is closed by now, so the files of the threads past `threads` can go.
  for (auto& source: sources) {
    if (source.thread < threads) {
      continue;
    }
    auto path = databasePath(source.thread);
    auto name = path.toString();
    directory.tryRemove(path);
    directory.tryRemove(kj::Path(kj::str(name, "-wal")));
    directory.tryRemove(kj::Path(kj::str(name, "-shm")));
  }
}

void AlarmScheduler::loadAlarmsFromDb() {
  auto now = clock.now();

  // TODO(someday): don't maintain the entire alarm set in memory -- right now for the usecase of
  // local development, doing so is sufficient.
  auto query = db->run(R"(
    SELECT actor_id, scheduled_time, actor_name FROM _cf_ALARM;
  )");

  while (!query.isDone()) {
    auto date = kj::UNIX_EPOCH + (kj::NANOSECONDS * query.getInt64(1));

    auto actor = ActorKey(query.getText(0), query.getMaybeText(2));
    auto scheduledAlarm = scheduleAlarm(now, actor, date);
    alarms.insert(kj::mv(actor), kj::mv(scheduledAlarm));

    query.nextRow();
  }
}

kj::Maybe<kj::Date> AlarmScheduler::getAlarm(const ActorKey& actor) {
  // TODO(someday): Might be able to simplify AlarmScheduler somewhat, now that ActorSqlite no
  // longer relies on it for getAlarm()?
  KJ_IF_SOME(alarm, alarms.find(actor)) {
    if (alarm.status == AlarmStatus::STARTED) {
      // getAlarm() when the alarm handler is running should return null,
      // unless an alarm is queued;
      return alarm.queuedAlarm;
    } else {
      return alarm.scheduledTime;
    }
  } else {
    // We currently retain the entire set of queued alarms in memory, no need to hit sqlite
    return kj::none;
  }
}

bool AlarmScheduler::setAlarm(const ActorKey& actor, kj::Date scheduledTime) {
  int64_t scheduledTimeNs = (scheduledTime - kj::UNIX_EPOCH) / kj::NANOSECONDS;
  SqliteDatabase::Query::ValuePtr nameParam = nullptr;
  KJ_IF_SOME(n, actor.name) {
    nameParam = n.asPtr();
  }
  auto query = stmtSetAlarm.run(actor.actorId, scheduledTimeNs, nameParam);

  bool existing = true;
  auto& entry = alarms.findOrCreate(actor, [&]() {
    existing = false;
    return decltype(alarms)::Entry{actor.clone(), scheduleAlarm(clock.now(), actor, scheduledTime)};
  });

  if (existing) {
    if (entry.status != AlarmStatus::WAITING) {
      // We queue any new alarm after the existing alarm even if the new alarm has the same scheduled
      // time, as receiving a notification directly maps to a write for that time in the actor.
      entry.queuedAlarm = scheduledTime;
    } else {
      entry = scheduleAlarm(clock.now(), entry.actor, scheduledTime);
    }
  }

  return query.changeCount() > 0;
}

void AlarmScheduler::deleteAll() {
  // Cancel all in-memory alarm tasks.
  alarms.clear();
  // Wipe the persistent store.
  db->run("DELETE FROM _cf_ALARM;");
}

bool AlarmScheduler::deleteAlarm(const ActorKey& actor) {
  auto query = stmtDeleteAlarm.run(actor.actorId);

  KJ_IF_SOME(entry, alarms.findEntry(actor)) {
    KJ_IF_SOME(queued, entry.value.queuedAlarm) {
      if (entry.value.status == AlarmStatus::STARTED) {
        // If we are currently running an alarm, we want to delete the queued instead of current.
        entry.value.queuedAlarm = kj::none;
      } else {
        entry.value = scheduleAlarm(clock.now(), entry.value.actor, queued);
      }
    } else {
      if (entry.value.status != AlarmStatus::STARTED) {
        // We can't remove running alarms.
        alarms.erase(entry);
      }
    }
  }

  return query.changeCount() > 0;
}

AlarmScheduler::ScheduledAlarm AlarmScheduler::scheduleAlarm(
    kj::Date now, const ActorKey& actor, kj::Date scheduledTime) {
  auto task = makeAlarmTask(scheduledTime - now, actor, scheduledTime);
  return ScheduledAlarm{actor.clone(), scheduledTime, kj::mv(task)};
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

kj::Promise<void> AlarmScheduler::makeAlarmTask(
    kj::Duration delay, const ActorKey& actor, kj::Date scheduledTime) {
  auto ownActor = actor.clone();
  co_await checkTimestamp(delay, scheduledTime);
  uint32_t retryCount = 0;
  {
    auto& entry = KJ_ASSERT_NONNULL(alarms.findEntry(ownActor));
    entry.value.status = AlarmStatus::STARTED;
    retryCount = entry.value.countedRetry;
  }

  auto alarmOutcome = co_await ([&]() -> kj::Promise<WorkerInterface::AlarmOutcome> {
    try {
      auto result = co_await getActor(ownActor)->runAlarm(scheduledTime, retryCount);
      auto outcome = result.asOutcome();
      outcome.retry = outcome.outcome != EventOutcome::OK && outcome.retry;
      co_return outcome;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_LOG(WARNING, exception);
      co_return WorkerInterface::AlarmOutcome{.retry = true,

        // An exception here is "weird", they should normally
        // be turned into AlarmResult statuses in the sandbox
        // for any user-caused error. Let's not count this
        // retry attempt against the limit.
        .retryCountsAgainstLimit = false,
        .outcome = EventOutcome::EXCEPTION};
    }
  })();

  try {
    auto& entry = KJ_ASSERT_NONNULL(alarms.findEntry(ownActor));

    // We can't overwrite our entry before moving ourselves out of it, as a promise cannot
    // delete itself.
    tasks.add(kj::mv(entry.value.task));

    // If an alarm is queued, there's no point in retrying the current one -- proceed
    // to running the queued alarm instead.
    KJ_IF_SOME(a, entry.value.queuedAlarm) {
      // creating a new alarm and overwriting the old one will reset
      // `status` to WAITING and `queuedAlarm` to null
      entry.value = scheduleAlarm(clock.now(), entry.value.actor, a);
      co_return;
    }

    // When we reach this block of code and alarm has either succeeded or failed and may (or may
    // not) retry. Setting the status of an alarm as FINISHED here, will allow deletion of alarms
    // between retries. If there's a retry, `makeAlarmTask` is called, setting status as RUNNING
    // again.
    entry.value.status = AlarmStatus::FINISHED;

    if (alarmOutcome.retry) {
      // recreate the task, running after a delay determined using the retry factor
      if (entry.value.countedRetry >= AlarmScheduler::RETRY_MAX_TRIES) {
        // Notify the actor to clear its in-memory alarm state so getAlarm() reflects the
        // deletion. We ignore the returned remaining time — the workerd-local alarm scheduler
        // already has visibility into the actor's alarm state via its SQLite hooks.
        // If the notification fails, we keep the alarm in the scheduler so it is not silently
        // lost.
        entry.value.task = abandonAlarm(ownActor, scheduledTime);
        co_return;
      }
      if (alarmOutcome.retryCountsAgainstLimit) {
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
      entry.value.previousRetryCountedAgainstLimit = alarmOutcome.retryCountsAgainstLimit;

      entry.value.backoff = kj::min(AlarmScheduler::RETRY_BACKOFF_MAX, entry.value.backoff);
      auto delay = (AlarmScheduler::RETRY_START_SECONDS << entry.value.backoff) * kj::SECONDS;

      std::uniform_int_distribution<> distribution(0, maxJitterMsForDelay(delay));
      delay += distribution(random) * kj::MILLISECONDS;

      entry.value.backoff++;
      entry.value.retry++;

      entry.value.task = makeAlarmTask(delay, ownActor, scheduledTime);
    } else {
      KJ_ASSERT(entry.value.queuedAlarm == kj::none);
      if (alarmOutcome.outcome == EventOutcome::ABORTED) {
        entry.value.task = abandonAlarm(ownActor, scheduledTime);
        co_return;
      }
      deleteAlarm(ownActor);
    }
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_LOG(ERROR, "Failed to run alarm and was unable to schedule a retry", exception);
  }
}

kj::Promise<void> AlarmScheduler::abandonAlarm(const ActorKey& actor, kj::Date scheduledTime) {
  auto ownActor = actor.clone();
  try {
    co_await getActor(ownActor)->abandonAlarm(scheduledTime);
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_LOG(WARNING, "abandonAlarm notification failed, keeping alarm in scheduler", exception);
    co_return;
  }

  KJ_IF_SOME(entry, alarms.findEntry(ownActor)) {
    if (entry.value.status != AlarmStatus::FINISHED || entry.value.scheduledTime != scheduledTime) {
      co_return;
    }

    // This task cannot remove or replace the promise that owns it until it has been moved out of
    // the alarm entry.
    tasks.add(kj::mv(entry.value.task));
    KJ_IF_SOME(replacement, entry.value.queuedAlarm) {
      entry.value = scheduleAlarm(clock.now(), entry.value.actor, replacement);
    } else {
      deleteAlarm(ownActor);
    }
  }
}

void AlarmScheduler::taskFailed(kj::Exception&& e) {
  KJ_LOG(WARNING, e);
}

}  // namespace workerd::server
