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
  return [](const ActorKey&) -> kj::Own<WorkerInterface> {
    KJ_FAIL_ASSERT("getActor should not be called in this test");
  };
}

int64_t toNs(kj::Date date) {
  return (date - kj::UNIX_EPOCH) / kj::NANOSECONDS;
}

// A clock whose current time can be moved forward manually, so a test can drive an alarm to fire.
class AdjustableClock final: public kj::Clock {
 public:
  kj::Date now() const override {
    return time;
  }
  void setTime(kj::Date newTime) {
    time = newTime;
  }

 private:
  kj::Date time = kj::UNIX_EPOCH;
};

// Minimal WorkerInterface that only supports runAlarm(); every other entry point is unused by the
// alarm scheduler tests. runAlarm() reports success (no retry) and invokes `onAlarm` so the test
// can observe that the alarm fired.
class AlarmStubWorkerInterface final: public WorkerInterface {
 public:
  explicit AlarmStubWorkerInterface(kj::Function<void()> onAlarm): onAlarm(kj::mv(onAlarm)) {}

  kj::Promise<AlarmResult> runAlarm(kj::Date, uint32_t) override {
    onAlarm();
    return AlarmResult{.retry = false, .outcome = EventOutcome::OK};
  }

  kj::Promise<void> request(kj::HttpMethod,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncInputStream&,
      kj::HttpService::Response&) override {
    KJ_UNIMPLEMENTED("AlarmStubWorkerInterface::request not used");
  }
  kj::Promise<void> connect(kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncIoStream&,
      ConnectResponse&,
      kj::HttpConnectSettings) override {
    KJ_UNIMPLEMENTED("AlarmStubWorkerInterface::connect not used");
  }
  kj::Promise<void> prewarm(kj::StringPtr) override {
    KJ_UNIMPLEMENTED("AlarmStubWorkerInterface::prewarm not used");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("AlarmStubWorkerInterface::runScheduled not used");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent>) override {
    KJ_UNIMPLEMENTED("AlarmStubWorkerInterface::customEvent not used");
  }

 private:
  kj::Function<void()> onAlarm;
};

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

KJ_TEST("AlarmScheduler restores the persisted actor_name onto the ActorKey when an alarm fires") {
  // The in-memory name loaded by loadAlarmsFromDb is only observable when an alarm actually fires
  // (it is handed to getActor so the reconstructed ID exposes ctx.id.name). This test seeds a named
  // alarm, drops the scheduler, then constructs a fresh scheduler that must load the alarm from disk
  // and fire it, and verifies the name reaches getActor.
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  AdjustableClock clock;
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  kj::Path path({"alarms"});

  kj::Date scheduledTime = kj::UNIX_EPOCH + 1 * kj::HOURS;

  // Persist a named alarm, then drop the scheduler so nothing about the name survives in memory.
  {
    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());
    scheduler.setAlarm(ActorKey{.actorId = "named-actor"_kj, .name = "my-name"_kj}, scheduledTime);
  }

  // A fresh scheduler must reload the alarm (and its name) from disk.
  kj::Maybe<kj::String> observedName;
  bool fired = false;
  auto getActor = [&](const ActorKey& actor) -> kj::Own<WorkerInterface> {
    observedName = actor.name.map([](kj::StringPtr n) { return kj::str(n); });
    return kj::heap<AlarmStubWorkerInterface>([&fired]() { fired = true; });
  };

  AlarmScheduler scheduler(clock, timer, vfs, path.clone(), kj::mv(getActor));

  // Advance both the wall clock and the timer past the scheduled time so the alarm fires.
  clock.setTime(scheduledTime);
  timer.advanceTo(kj::origin<kj::TimePoint>() + (scheduledTime - kj::UNIX_EPOCH));

  // Pump the event loop until the alarm task has run (bounded so a regression can't hang forever).
  for (uint i = 0; i < 100 && !fired; i++) {
    waitScope.poll();
  }

  KJ_EXPECT(fired);
  KJ_EXPECT(KJ_ASSERT_NONNULL(observedName) == "my-name"_kj);
}

}  // namespace
}  // namespace workerd::server
