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
  using RunAlarmFn = kj::Function<AlarmResult(kj::Date scheduledTime, uint32_t retryCount)>;
  using AbandonAlarmFn = kj::Function<kj::Promise<kj::Maybe<kj::Date>>(kj::Date scheduledTime)>;

  explicit AlarmStubWorkerInterface(kj::Function<void()> onAlarm)
      : AlarmStubWorkerInterface(kj::mv(onAlarm),
            {.retry = false, .outcome = EventOutcome::OK},
            [](kj::Date) { return kj::Maybe<kj::Date>(kj::none); }) {}

  AlarmStubWorkerInterface(
      kj::Function<void()> onAlarm, AlarmOutcome outcome, AbandonAlarmFn onAbandon)
      : AlarmStubWorkerInterface(
            [onAlarm = kj::mv(onAlarm), outcome](kj::Date, uint32_t) mutable {
    onAlarm();
    return AlarmResult{.retry = outcome.retry,
      .retryCountsAgainstLimit = outcome.retryCountsAgainstLimit,
      .outcome = outcome.outcome};
  }, kj::mv(onAbandon)) {}

  // Answers each runAlarm() with what `run` returns for the alarm.
  AlarmStubWorkerInterface(RunAlarmFn run, AbandonAlarmFn onAbandon)
      : run(kj::mv(run)),
        onAbandon(kj::mv(onAbandon)) {}

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    return run(scheduledTime, retryCount);
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    return onAbandon(scheduledTime);
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
  RunAlarmFn run;
  AbandonAlarmFn onAbandon;
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
        [&abandoned](kj::Date) {
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

KJ_TEST("AlarmScheduler preserves an alarm queued while abandonment is pending") {
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
          [&abandonStarted, &abandonPromise](kj::Date) {
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
    pendingAbandon.fulfiller->fulfill(kj::none);

    for (uint i = 0; i < 100 && scheduler.getAlarm(actor) != replacementTime; i++) {
      waitScope.poll();
    }
    KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
  }

  AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());
  KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
}

// An actor that, by the time its alarm at `requestedTime` runs, has moved the alarm later to
// `storedTime`, which has also passed, and so runs that alarm instead, as ActorSqlite does. Its
// write of `storedTime` reaches the scheduler while the replaced alarm runs. Every run ends with
// `outcome`.
struct OverdueStoredAlarmTest {
  struct Run {
    kj::Date scheduledTime;
    uint32_t retryCount;
  };

  explicit OverdueStoredAlarmTest(WorkerInterface::AlarmOutcome outcome): outcome(outcome) {
    scheduler.setAlarm(actor, requestedTime);
    clock.setTime(storedTime);
  }

  // Runs the actor's alarms, and each retry once its backoff has passed, until none is left.
  void runAlarms() {
    for (uint i = 0; i < 100 && scheduler.getAlarm(actor) != kj::none; i++) {
      timer.advanceTo(timer.now() + 1 * kj::HOURS);
      waitScope.poll();
    }
  }

  WorkerInterface::AlarmResult runAlarm(kj::Date scheduledTime, uint32_t retryCount) {
    runs.add(Run{scheduledTime, retryCount});
    WorkerInterface::AlarmResult result{.retry = outcome.retry,
      .retryCountsAgainstLimit = outcome.retryCountsAgainstLimit,
      .outcome = outcome.outcome};
    if (scheduledTime == requestedTime) {
      scheduler.setAlarm(actor, storedTime);
      result.ranScheduledTime = storedTime;
    }
    return result;
  }

  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  AdjustableClock clock;
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
  kj::Own<const kj::Directory> dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs{*dir};
  kj::Date requestedTime = kj::UNIX_EPOCH + 1 * kj::HOURS;
  kj::Date storedTime = requestedTime + 1 * kj::MILLISECONDS;
  ActorKey actor{"overdue-actor"_kj, kj::none};
  WorkerInterface::AlarmOutcome outcome;
  kj::Vector<Run> runs;
  kj::Vector<kj::Date> abandoned;
  AlarmScheduler scheduler{clock, timer, vfs, kj::Path({"alarms"}),
    [this](const ActorKey&) -> kj::Own<WorkerInterface> {
    return kj::heap<AlarmStubWorkerInterface>(
        [this](kj::Date scheduledTime, uint32_t retryCount) {
      return runAlarm(scheduledTime, retryCount);
    }, [this](kj::Date scheduledTime) {
      abandoned.add(scheduledTime);
      return kj::Maybe<kj::Date>(kj::none);
    });
  }};
};

KJ_TEST("AlarmScheduler counts a failed run toward the overdue alarm the actor ran instead") {
  OverdueStoredAlarmTest t({.retry = true, .outcome = EventOutcome::EXCEPTION});
  t.runAlarms();

  // The stored alarm's retries continue from its first run rather than start over.
  KJ_ASSERT(t.runs.size() == AlarmScheduler::RETRY_MAX_TRIES + 1, t.runs.size());
  KJ_EXPECT(t.runs[0].scheduledTime == t.requestedTime);
  for (uint32_t i = 1; i < t.runs.size(); i++) {
    KJ_EXPECT(t.runs[i].scheduledTime == t.storedTime, i);
    KJ_EXPECT(t.runs[i].retryCount == i, i, t.runs[i].retryCount);
  }
  KJ_ASSERT(t.abandoned.size() == 1, t.abandoned.size());
  KJ_EXPECT(t.abandoned[0] == t.storedTime);
  KJ_EXPECT(t.scheduler.getAlarm(t.actor) == kj::none);
}

KJ_TEST("AlarmScheduler doesn't rerun the overdue alarm the actor ran instead after it succeeds") {
  OverdueStoredAlarmTest t({.retry = false, .outcome = EventOutcome::OK});
  t.runAlarms();

  KJ_EXPECT(t.runs.size() == 1, t.runs.size());
  KJ_EXPECT(t.scheduler.getAlarm(t.actor) == kj::none);
}

}  // namespace
}  // namespace workerd::server
