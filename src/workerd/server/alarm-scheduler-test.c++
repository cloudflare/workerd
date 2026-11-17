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

// Minimal WorkerInterface that supports runAlarm() and abandonAlarm(); every other entry point is
// unused by the alarm scheduler tests. By default, runAlarm() reports success (no retry) and invokes
// `onAlarm` so the test can observe that the alarm fired.
class AlarmStubWorkerInterface final: public WorkerInterface {
 public:
  explicit AlarmStubWorkerInterface(kj::Function<void()> onAlarm)
      : onAlarm(kj::mv(onAlarm)),
        onAbandon([]() { return kj::Maybe<kj::Date>(kj::none); }) {}

  AlarmStubWorkerInterface(kj::Function<void()> onAlarm,
      AlarmOutcome outcome,
      kj::Function<kj::Promise<kj::Maybe<kj::Date>>()> onAbandon)
      : onAlarm(kj::mv(onAlarm)),
        outcome(outcome),
        onAbandon(kj::mv(onAbandon)) {}

  kj::Promise<AlarmResult> runAlarm(kj::Date, uint32_t) override {
    onAlarm();
    return AlarmResult{.retry = outcome.retry,
      .retryCountsAgainstLimit = outcome.retryCountsAgainstLimit,
      .outcome = outcome.outcome};
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date) override {
    return onAbandon();
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
  AlarmOutcome outcome{.retry = false, .outcome = EventOutcome::OK};
  kj::Function<kj::Promise<kj::Maybe<kj::Date>>()> onAbandon;
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

    // The old schema has no actor name to restore.
    auto alarm = scheduler.getAlarm(ActorKey("old-actor"_kj, kj::none));
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
    scheduler.setAlarm(ActorKey("named-actor"_kj, "my-name"_kj), scheduledTime);

    // A subsequent update without a name must not clear the previously-persisted name. This mirrors
    // how the alarm scheduler is driven: the name is only supplied when the actor is created via
    // getByName(), but later setAlarm() calls (e.g. from an already-running alarm handler) may not
    // carry it.
    scheduler.setAlarm(ActorKey("named-actor"_kj, kj::none), updatedTime);
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
    // Alarm lookup uses only actor identity, so no name is needed.
    auto alarm = scheduler.getAlarm(ActorKey("named-actor"_kj, kj::none));
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
    scheduler.setAlarm(ActorKey("named-actor"_kj, "my-name"_kj), scheduledTime);
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

KJ_TEST("AlarmScheduler abandons an alarm when ctx.abort() sets retryAlarm to false") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  AdjustableClock clock;
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  kj::Path path({"alarms"});
  auto scheduledTime = kj::UNIX_EPOCH + 1 * kj::HOURS;
  // This test actor was not created from a name.
  auto actor = ActorKey("terminal-actor"_kj, kj::none);

  bool fired = false;
  bool abandoned = false;
  auto getActor = [&](const ActorKey&) -> kj::Own<WorkerInterface> {
    return kj::heap<AlarmStubWorkerInterface>([&fired]() { fired = true; },
        WorkerInterface::AlarmOutcome{
          .retry = false,
          .retryCountsAgainstLimit = true,
          .outcome = EventOutcome::ABORTED,
        },
        [&abandoned]() {
      abandoned = true;
      return kj::Maybe<kj::Date>(kj::none);
    });
  };

  AlarmScheduler scheduler(clock, timer, vfs, path.clone(), kj::mv(getActor));
  scheduler.setAlarm(actor, scheduledTime);

  clock.setTime(scheduledTime);
  timer.advanceTo(kj::origin<kj::TimePoint>() + (scheduledTime - kj::UNIX_EPOCH));
  for (uint i = 0; i < 100 && !abandoned; i++) {
    waitScope.poll();
  }

  KJ_EXPECT(fired);
  KJ_EXPECT(abandoned);
  KJ_EXPECT(scheduler.getAlarm(actor) == kj::none);
}

KJ_TEST("AlarmScheduler keeps the actor's WorkerInterface until it is told the alarm is abandoned") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  AdjustableClock clock;
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  kj::Path path({"alarms"});
  auto scheduledTime = kj::UNIX_EPOCH + 1 * kj::HOURS;
  // This test actor was not created from a name.
  auto actor = ActorKey("terminal-actor"_kj, kj::none);
  auto pendingAbandon = kj::newPromiseAndFulfiller<kj::Maybe<kj::Date>>();
  auto abandonPromise = pendingAbandon.promise.fork();
  bool abandonStarted = false;
  uint liveWorkers = 0;

  auto getActor = [&](const ActorKey&) -> kj::Own<WorkerInterface> {
    liveWorkers++;
    return kj::heap<AlarmStubWorkerInterface>([]() {},
        WorkerInterface::AlarmOutcome{
          .retry = false,
          .retryCountsAgainstLimit = true,
          .outcome = EventOutcome::ABORTED,
        },
        [&abandonStarted, &abandonPromise]() {
      abandonStarted = true;
      return abandonPromise.addBranch();
    }).attach(kj::defer([&liveWorkers]() { liveWorkers--; }));
  };

  AlarmScheduler scheduler(clock, timer, vfs, path.clone(), kj::mv(getActor));
  scheduler.setAlarm(actor, scheduledTime);

  clock.setTime(scheduledTime);
  timer.advanceTo(kj::origin<kj::TimePoint>() + (scheduledTime - kj::UNIX_EPOCH));
  for (uint i = 0; i < 100 && !abandonStarted; i++) {
    waitScope.poll();
  }
  KJ_ASSERT(abandonStarted);

  // The worker that ran the alarm is gone, but the one being told it is abandoned stays until the
  // call returns.
  waitScope.poll();
  KJ_EXPECT(liveWorkers == 1);

  pendingAbandon.fulfiller->fulfill(kj::none);
  for (uint i = 0; i < 100 && scheduler.getAlarm(actor) != kj::none; i++) {
    waitScope.poll();
  }
  KJ_EXPECT(scheduler.getAlarm(actor) == kj::none);
  KJ_EXPECT(liveWorkers == 0);
}

KJ_TEST("AlarmScheduler preserves an alarm queued while abandonment is pending") {
  for (bool abandonFails: {false, true}) {
    KJ_CONTEXT(abandonFails);
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    AdjustableClock clock;
    kj::TimerImpl timer(kj::origin<kj::TimePoint>());

    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    kj::Path path({"alarms"});
    auto scheduledTime = kj::UNIX_EPOCH + 1 * kj::HOURS;
    auto replacementTime = kj::UNIX_EPOCH + 2 * kj::HOURS;
    // This test actor was not created from a name.
    auto actor = ActorKey("terminal-actor"_kj, kj::none);
    auto pendingAbandon = kj::newPromiseAndFulfiller<kj::Maybe<kj::Date>>();
    auto abandonPromise = pendingAbandon.promise.fork();
    bool abandonStarted = false;

    {
      auto getActor = [&](const ActorKey&) -> kj::Own<WorkerInterface> {
        return kj::heap<AlarmStubWorkerInterface>([]() {},
            WorkerInterface::AlarmOutcome{
              .retry = false,
              .retryCountsAgainstLimit = true,
              .outcome = EventOutcome::ABORTED,
            },
            [&abandonStarted, &abandonPromise]() {
          abandonStarted = true;
          return abandonPromise.addBranch();
        });
      };

      AlarmScheduler scheduler(clock, timer, vfs, path.clone(), kj::mv(getActor));
      scheduler.setAlarm(actor, scheduledTime);

      clock.setTime(scheduledTime);
      timer.advanceTo(kj::origin<kj::TimePoint>() + (scheduledTime - kj::UNIX_EPOCH));
      for (uint i = 0; i < 100 && !abandonStarted; i++) {
        waitScope.poll();
      }

      KJ_EXPECT(abandonStarted);
      scheduler.setAlarm(actor, replacementTime);
      if (abandonFails) {
        pendingAbandon.fulfiller->reject(KJ_EXCEPTION(DISCONNECTED, "actor went away"));
      } else {
        pendingAbandon.fulfiller->fulfill(kj::none);
      }

      for (uint i = 0; i < 100 && scheduler.getAlarm(actor) != replacementTime; i++) {
        waitScope.poll();
      }
      KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
    }

    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());
    KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
  }
}

KJ_TEST("AlarmScheduler tells the actor again that an alarm is abandoned if telling it fails") {
  for (bool actorSetsAlarm: {false, true}) {
    KJ_CONTEXT(actorSetsAlarm);
    kj::EventLoop loop;
    kj::WaitScope waitScope(loop);
    AdjustableClock clock;
    kj::TimerImpl timer(kj::origin<kj::TimePoint>());
    auto advanceTo = [&](kj::Date time) {
      clock.setTime(time);
      timer.advanceTo(kj::origin<kj::TimePoint>() + (time - kj::UNIX_EPOCH));
      waitScope.poll();
    };

    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    kj::Path path({"alarms"});
    auto scheduledTime = kj::UNIX_EPOCH + 1 * kj::HOURS;
    auto replacementTime = kj::UNIX_EPOCH + 2 * kj::HOURS;
    // Allows for the jitter of the first retry.
    auto afterRetryDelay = scheduledTime + 2 * AlarmScheduler::RETRY_START_SECONDS * kj::SECONDS;
    // This test actor was not created from a name.
    auto actor = ActorKey("terminal-actor"_kj, kj::none);

    uint runs = 0;
    uint abandonTries = 0;
    auto getActor = [&](const ActorKey&) -> kj::Own<WorkerInterface> {
      return kj::heap<AlarmStubWorkerInterface>([&runs]() { runs++; },
          WorkerInterface::AlarmOutcome{
            .retry = false,
            .retryCountsAgainstLimit = true,
            .outcome = EventOutcome::ABORTED,
          },
          [&abandonTries]() -> kj::Promise<kj::Maybe<kj::Date>> {
        if (abandonTries++ == 0) {
          return KJ_EXCEPTION(DISCONNECTED, "actor went away");
        }
        return kj::Maybe<kj::Date>(kj::none);
      });
    };

    AlarmScheduler scheduler(clock, timer, vfs, path.clone(), kj::mv(getActor));
    scheduler.setAlarm(actor, scheduledTime);

    // The failure keeps the alarm, without running it again.
    advanceTo(scheduledTime);
    KJ_EXPECT(runs == 1);
    KJ_EXPECT(abandonTries == 1);
    KJ_EXPECT(scheduler.getAlarm(actor) == scheduledTime);

    if (actorSetsAlarm) {
      // The alarm the actor sets replaces the abandoned one without waiting for the retry, which it
      // makes unnecessary.
      scheduler.setAlarm(actor, replacementTime);
      advanceTo(afterRetryDelay);
      KJ_EXPECT(abandonTries == 1);
      KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);

      advanceTo(replacementTime);
      KJ_EXPECT(runs == 2);
      KJ_EXPECT(abandonTries == 2);
    } else {
      advanceTo(afterRetryDelay);
      KJ_EXPECT(runs == 1);
      KJ_EXPECT(abandonTries == 2);
    }
    KJ_EXPECT(scheduler.getAlarm(actor) == kj::none);
  }
}

}  // namespace
}  // namespace workerd::server
