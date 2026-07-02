// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alarm-scheduler.h"

#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/timer.h>

namespace workerd::server {
namespace {

// A getActor callback that must never be invoked. These tests only exercise startup migration and
// persistence; no alarm is ever allowed to fire (the timer is never advanced), so getActor should
// not be called.
AlarmScheduler::GetActorFn failingGetActor() {
  return [](kj::String) -> kj::Own<WorkerInterface> {
    KJ_FAIL_ASSERT("getActor should not be called in this test");
  };
}

int64_t toNs(kj::Date date) {
  return (date - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

KJ_TEST("AlarmScheduler migrates a database created before the actor_name column existed") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto& clock = kj::nullClock();
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  kj::Path path({"alarms"});

  // Alarms are scheduled far in the future so they never fire during the test.
  kj::Date scheduledTime = kj::UNIX_EPOCH + 24 * kj::HOURS;

  // Seed a database using the *old* schema, which lacks the `actor_name` column, and insert one
  // pending alarm. This is what a database persisted by an older workerd would look like.
  {
    SqliteDatabase db(vfs, path.clone(), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    db.run("PRAGMA journal_mode=WAL;");
    db.run(R"(
      CREATE TABLE _cf_ALARM (
        actor_id TEXT PRIMARY KEY,
        scheduled_time INTEGER
      ) WITHOUT ROWID;
    )");
    db.run("INSERT INTO _cf_ALARM VALUES (?, ?)", "old-actor"_kj, toNs(scheduledTime));
  }

  // Constructing the scheduler must migrate the schema (adding actor_name) and load the pre-existing
  // alarm without error.
  {
    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());

    auto alarm = scheduler.getAlarm(ActorKey{.actorId = "old-actor"_kj});
    KJ_EXPECT(KJ_ASSERT_NONNULL(alarm) == scheduledTime);
  }

  // The migrated database should now have the actor_name column, populated with NULL for the row
  // that predated it.
  {
    SqliteDatabase db(vfs, path.clone(), kj::WriteMode::MODIFY);
    auto query = db.run("SELECT actor_name FROM _cf_ALARM WHERE actor_id = ?", "old-actor"_kj);
    KJ_ASSERT(!query.isDone());
    KJ_EXPECT(query.getMaybeText(0) == kj::none);
  }
}

KJ_TEST("AlarmScheduler persists actor_name and preserves it across a nameless update") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  auto& clock = kj::nullClock();
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  kj::Path path({"alarms"});

  kj::Date scheduledTime = kj::UNIX_EPOCH + 24 * kj::HOURS;
  kj::Date updatedTime = kj::UNIX_EPOCH + 48 * kj::HOURS;

  {
    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());

    // A named actor persists its name alongside the alarm.
    scheduler.setAlarm(ActorKey{.actorId = "named-actor"_kj, .name = "my-name"_kj}, scheduledTime);

    // A subsequent update without a name must not clear the previously-persisted name. This mirrors
    // how the alarm scheduler is driven: the name is only supplied when the actor is created via
    // getByName(), but later setAlarm() calls (e.g. from an already-running alarm handler) may not
    // carry it.
    scheduler.setAlarm(ActorKey{.actorId = "named-actor"_kj}, updatedTime);
  }

  // Reopen the database directly to confirm both the updated time and the retained name.
  {
    SqliteDatabase db(vfs, path.clone(), kj::WriteMode::MODIFY);
    auto query = db.run(
        "SELECT scheduled_time, actor_name FROM _cf_ALARM WHERE actor_id = ?", "named-actor"_kj);
    KJ_ASSERT(!query.isDone());
    KJ_EXPECT(query.getInt64(0) == toNs(updatedTime));
    KJ_EXPECT(KJ_ASSERT_NONNULL(query.getMaybeText(1)) == "my-name"_kj);
  }

  // A fresh scheduler should load the named alarm from disk without error.
  {
    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());
    auto alarm = scheduler.getAlarm(ActorKey{.actorId = "named-actor"_kj});
    KJ_EXPECT(KJ_ASSERT_NONNULL(alarm) == updatedTime);
  }
}

}  // namespace
}  // namespace workerd::server
