// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alarm-scheduler.h"

#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/timer.h>

#include <cmath>

namespace workerd::server {
namespace {

// A getActor callback that must never be invoked, for a scheduler whose alarms the test never lets
// fire: it does not advance the timer to their time while the scheduler exists.
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
    pendingAbandon.fulfiller->fulfill(kj::none);

    for (uint i = 0; i < 100 && scheduler.getAlarm(actor) != replacementTime; i++) {
      waitScope.poll();
    }
    KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
  }

  AlarmScheduler scheduler(clock, timer, vfs, path.clone(), failingGetActor());
  KJ_EXPECT(scheduler.getAlarm(actor) == replacementTime);
}

// The tests below pin the scheduler's state machine: retries and their backoff, alarms set or
// deleted while a handler runs or a retry waits, and storage.

using AlarmResult = WorkerInterface::AlarmResult;

AlarmResult succeeded() {
  return {.retry = false, .outcome = EventOutcome::OK};
}

AlarmResult failedCounted() {
  return {.retry = true, .retryCountsAgainstLimit = true, .outcome = EventOutcome::EXCEPTION};
}

AlarmResult failedUncounted() {
  return {.retry = true, .retryCountsAgainstLimit = false, .outcome = EventOutcome::EXCEPTION};
}

struct AlarmRun {
  kj::String actorId;
  kj::Date scheduledTime;
  uint32_t retryCount;
};

// What the actors of a test do when the scheduler runs or abandons their alarms. runAlarm() calls
// take the next step in order; once the steps run out, every run succeeds.
struct ScriptedActors {
  kj::Vector<kj::Function<kj::Promise<AlarmResult>()>> steps;
  size_t nextStep = 0;
  kj::Vector<AlarmRun> runs;
  kj::Vector<kj::Date> abandons;
  bool failAbandon = false;

  void then(AlarmResult result) {
    steps.add(
        [result = kj::mv(result)]() mutable -> kj::Promise<AlarmResult> { return kj::mv(result); });
  }

  void thenThrow() {
    steps.add(
        []() -> kj::Promise<AlarmResult> { return KJ_EXCEPTION(FAILED, "test actor failed"); });
  }

  // A run that finishes when the test fulfills the returned fulfiller.
  kj::Own<kj::PromiseFulfiller<AlarmResult>> thenHang() {
    auto paf = kj::newPromiseAndFulfiller<AlarmResult>();
    steps.add([promise = kj::mv(paf.promise)]() mutable { return kj::mv(promise); });
    return kj::mv(paf.fulfiller);
  }
};

class ScriptedWorkerInterface final: public WorkerInterface {
 public:
  ScriptedWorkerInterface(ScriptedActors& actors, kj::String actorId)
      : actors(actors),
        actorId(kj::mv(actorId)) {}

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    actors.runs.add(AlarmRun{kj::str(actorId), scheduledTime, retryCount});
    if (actors.nextStep < actors.steps.size()) {
      return actors.steps[actors.nextStep++]();
    }
    return succeeded();
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    actors.abandons.add(scheduledTime);
    if (actors.failAbandon) {
      return KJ_EXCEPTION(FAILED, "test actor failed to abandon its alarm");
    }
    return kj::Maybe<kj::Date>(kj::none);
  }

  kj::Promise<void> request(kj::HttpMethod,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncInputStream&,
      kj::HttpService::Response&) override {
    KJ_UNIMPLEMENTED("ScriptedWorkerInterface::request not used");
  }
  kj::Promise<void> connect(kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncIoStream&,
      ConnectResponse&,
      kj::HttpConnectSettings) override {
    KJ_UNIMPLEMENTED("ScriptedWorkerInterface::connect not used");
  }
  kj::Promise<void> prewarm(kj::StringPtr) override {
    KJ_UNIMPLEMENTED("ScriptedWorkerInterface::prewarm not used");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("ScriptedWorkerInterface::runScheduled not used");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent>) override {
    KJ_UNIMPLEMENTED("ScriptedWorkerInterface::customEvent not used");
  }

 private:
  ScriptedActors& actors;
  kj::String actorId;
};

kj::TimePoint toTimePoint(kj::Date date) {
  return kj::origin<kj::TimePoint>() + (date - kj::UNIX_EPOCH);
}

// A scheduler over in-memory storage, with a clock and a timer that only the test moves. The timer
// reads the same as the clock (the Unix epoch is its origin) unless a test moves them apart.
struct SchedulerFixture {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  AdjustableClock clock;
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
  kj::Own<const kj::Directory> dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs{*dir};
  kj::Path path = kj::Path({"alarms"});
  ScriptedActors actors;
  kj::Own<AlarmScheduler> scheduler;

  ActorKey actor = ActorKey("actor"_kj, kj::none);
  kj::Date t0 = kj::UNIX_EPOCH + 1 * kj::HOURS;

  SchedulerFixture() {
    open();
  }

  // Replaces the scheduler with a new one that loads its alarms from storage.
  AlarmScheduler& open() {
    scheduler = nullptr;
    scheduler = kj::heap<AlarmScheduler>(
        clock, timer, vfs, path.clone(), [this](const ActorKey& key) -> kj::Own<WorkerInterface> {
      return kj::heap<ScriptedWorkerInterface>(actors, kj::str(key.actorId));
    });
    return *scheduler;
  }

  // Moves the clock and the timer to `time` and runs whatever becomes ready. TimerImpl fires a
  // timer only in advanceTo(), so this repeats it for timers that tasks arm for a time already
  // reached.
  void advanceTo(kj::Date time) {
    clock.setTime(time);
    for (uint i = 0; i < 10; i++) {
      timer.advanceTo(toTimePoint(time));
      waitScope.poll();
    }
  }

  // Expects the next run `delay` after `from`, within the retry jitter (up to
  // `RETRY_JITTER_FACTOR` of the delay, added), and returns the time the test moved to, which the
  // next backoff starts from.
  kj::Date expectRetry(kj::Date from, kj::Duration delay) {
    auto before = actors.runs.size();
    advanceTo(from + delay - 1 * kj::MILLISECONDS);
    KJ_EXPECT(actors.runs.size() == before, "retried before its backoff", delay);
    // The scheduler's own bound: whole milliseconds, rounded down.
    double delayMs = delay / kj::MILLISECONDS;
    auto maxJitter =
        static_cast<int64_t>(std::floor(AlarmScheduler::RETRY_JITTER_FACTOR * delayMs)) *
        kj::MILLISECONDS;
    auto latest = from + delay + maxJitter;
    advanceTo(latest);
    KJ_EXPECT(actors.runs.size() == before + 1, "not retried within its backoff", delay);
    return latest;
  }

  AlarmRun& lastRun() {
    KJ_ASSERT(actors.runs.size() > 0);
    return actors.runs.back();
  }
};

KJ_TEST("AlarmScheduler reports whether setAlarm() and deleteAlarm() changed storage") {
  SchedulerFixture f;
  KJ_EXPECT(!f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->setAlarm(f.actor, f.t0));
  KJ_EXPECT(f.scheduler->setAlarm(f.actor, f.t0 + 1 * kj::HOURS));
  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(!f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler runs a successful alarm once and deletes it") {
  SchedulerFixture f;
  f.scheduler->setAlarm(f.actor, f.t0);

  f.advanceTo(f.t0 - 1 * kj::MILLISECONDS);
  KJ_EXPECT(f.actors.runs.size() == 0);

  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.lastRun().actorId == "actor"_kj);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);

  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler runs an alarm that came due while it was not running at startup") {
  SchedulerFixture f;
  f.scheduler->setAlarm(f.actor, f.t0);
  f.scheduler = nullptr;

  // An alarm waits on the timer for its time less the clock's, so one already due runs without
  // the timer moving: the timer stays at its origin, an hour before the alarm.
  f.clock.setTime(f.t0 + 1 * kj::HOURS);
  f.open();
  for (uint i = 0; i < 10; i++) {
    f.timer.advanceTo(f.timer.now());
    f.waitScope.poll();
  }
  KJ_ASSERT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler waits for the clock when the timer fires before the scheduled time") {
  SchedulerFixture f;
  f.scheduler->setAlarm(f.actor, f.t0);

  // The timer reaches the scheduled time while the clock is 5 seconds behind it: the scheduler
  // waits another 5 seconds on the timer.
  f.clock.setTime(f.t0 - 5 * kj::SECONDS);
  for (uint i = 0; i < 10; i++) {
    f.timer.advanceTo(toTimePoint(f.t0));
    f.waitScope.poll();
  }
  KJ_EXPECT(f.actors.runs.size() == 0);

  f.clock.setTime(f.t0);
  f.timer.advanceTo(toTimePoint(f.t0 + 5 * kj::SECONDS - 1 * kj::MILLISECONDS));
  f.waitScope.poll();
  KJ_EXPECT(f.actors.runs.size() == 0);

  f.timer.advanceTo(toTimePoint(f.t0 + 5 * kj::SECONDS));
  f.waitScope.poll();
  KJ_EXPECT(f.actors.runs.size() == 1);
}

KJ_TEST("AlarmScheduler reschedules a waiting alarm when it is set again") {
  SchedulerFixture f;

  // Later.
  f.scheduler->setAlarm(f.actor, f.t0);
  f.scheduler->setAlarm(f.actor, f.t0 + 1 * kj::HOURS);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0 + 1 * kj::HOURS);
  f.advanceTo(f.t0);
  KJ_EXPECT(f.actors.runs.size() == 0);
  f.advanceTo(f.t0 + 1 * kj::HOURS);
  KJ_ASSERT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0 + 1 * kj::HOURS);

  // Earlier.
  auto t1 = f.t0 + 2 * kj::HOURS;
  f.scheduler->setAlarm(f.actor, t1 + 1 * kj::HOURS);
  f.scheduler->setAlarm(f.actor, t1);
  f.advanceTo(t1);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == t1);
  f.advanceTo(t1 + 2 * kj::HOURS);
  KJ_EXPECT(f.actors.runs.size() == 2);
}

KJ_TEST("AlarmScheduler never runs a waiting alarm that was deleted") {
  SchedulerFixture f;
  f.scheduler->setAlarm(f.actor, f.t0);
  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 0);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler retries counted failures with exponential backoff, then abandons") {
  SchedulerFixture f;
  constexpr uint MAX_TRIES = AlarmScheduler::RETRY_MAX_TRIES;
  for (uint i = 0; i <= MAX_TRIES; i++) {
    f.actors.then(failedCounted());
  }
  f.scheduler->setAlarm(f.actor, f.t0);

  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.lastRun().retryCount == 0);

  // Retries 2, 4, 8, 16, 32 and 64 seconds apart, each told how many counted retries came before.
  auto now = f.t0;
  for (uint i = 0; i < MAX_TRIES; i++) {
    now = f.expectRetry(now, (AlarmScheduler::RETRY_START_SECONDS << i) * kj::SECONDS);
    KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
    KJ_EXPECT(f.lastRun().retryCount == i + 1);
  }

  // The seventh failure exhausts the retries: the actor is told, and the alarm is gone.
  KJ_ASSERT(f.actors.abandons.size() == 1);
  KJ_EXPECT(f.actors.abandons[0] == f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.advanceTo(now + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == MAX_TRIES + 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

// Once the counted retries are used up, any failure that asks for a retry abandons the alarm, even
// one that would not count against the limit.
void expectAbandonedAtLimitBy(kj::FunctionParam<void(ScriptedActors&)> addLastFailure) {
  SchedulerFixture f;
  constexpr uint MAX_TRIES = AlarmScheduler::RETRY_MAX_TRIES;
  for (uint i = 0; i < MAX_TRIES; i++) {
    f.actors.then(failedCounted());
  }
  addLastFailure(f.actors);
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  auto now = f.t0;
  for (uint i = 0; i < MAX_TRIES; i++) {
    now = f.expectRetry(now, (AlarmScheduler::RETRY_START_SECONDS << i) * kj::SECONDS);
    KJ_EXPECT(f.lastRun().retryCount == i + 1);
  }

  KJ_EXPECT(f.actors.abandons.size() == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.advanceTo(now + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == MAX_TRIES + 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler abandons an uncounted failure once the counted retries are used up") {
  expectAbandonedAtLimitBy([](ScriptedActors& actors) { actors.then(failedUncounted()); });
}

KJ_TEST("AlarmScheduler abandons a run that threw once the counted retries are used up") {
  expectAbandonedAtLimitBy([](ScriptedActors& actors) { actors.thenThrow(); });
}

KJ_TEST("AlarmScheduler retries uncounted failures without limit, backing off up to 1024 seconds") {
  SchedulerFixture f;
  constexpr uint RETRIES = 12;
  for (uint i = 0; i < RETRIES; i++) {
    f.actors.then(failedUncounted());
  }
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  auto now = f.t0;
  for (uint i = 0; i < RETRIES; i++) {
    auto shift = kj::min(i, static_cast<uint>(AlarmScheduler::RETRY_BACKOFF_MAX));
    now = f.expectRetry(now, (AlarmScheduler::RETRY_START_SECONDS << shift) * kj::SECONDS);
    KJ_EXPECT(f.lastRun().retryCount == 0);
  }

  // The last run succeeded.
  KJ_EXPECT(f.actors.abandons.size() == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler retries an alarm whose run threw, without counting it") {
  SchedulerFixture f;
  f.actors.thenThrow();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 1);

  f.expectRetry(f.t0, AlarmScheduler::RETRY_START_SECONDS * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler resets the backoff when a counted failure follows uncounted ones") {
  SchedulerFixture f;
  f.actors.then(failedUncounted());
  f.actors.then(failedUncounted());
  f.actors.then(failedCounted());
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  auto now = f.expectRetry(f.t0, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  now = f.expectRetry(now, 4 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  // The first counted failure starts the backoff over.
  now = f.expectRetry(now, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 1);
  now = f.expectRetry(now, 4 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 2);

  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler resets the backoff on every switch from uncounted to counted failures") {
  SchedulerFixture f;
  f.actors.then(failedCounted());
  f.actors.then(failedUncounted());
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  auto now = f.expectRetry(f.t0, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 1);
  // An uncounted failure after a counted one continues the backoff.
  now = f.expectRetry(now, 4 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 1);
  // The next counted failure starts it over.
  now = f.expectRetry(now, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().retryCount == 2);

  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler deletes, without abandoning, a failed run that asks for no retry") {
  SchedulerFixture f;
  f.actors.then(AlarmResult{.retry = false, .outcome = EventOutcome::EXCEPTION});
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.actors.abandons.size() == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);

  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler runs an alarm queued for the same time again after the handler") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  f.scheduler->setAlarm(f.actor, f.t0);

  run->fulfill(succeeded());
  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.actors.runs[0].scheduledTime == f.t0);
  KJ_EXPECT(f.actors.runs[1].scheduledTime == f.t0);
  KJ_EXPECT(f.actors.runs[1].retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler does not retry a run that asks for a retry but succeeded") {
  SchedulerFixture f;
  f.actors.then(
      AlarmResult{.retry = true, .retryCountsAgainstLimit = true, .outcome = EventOutcome::OK});
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler queues an alarm set while the handler runs, even for the same time") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 1);

  // A running alarm is not reported; only one queued behind it is, and a later set replaces it.
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.scheduler->setAlarm(f.actor, f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0);
  f.scheduler->setAlarm(f.actor, f.t0 + 1 * kj::HOURS);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0 + 1 * kj::HOURS);

  run->fulfill(succeeded());
  f.advanceTo(f.t0);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0 + 1 * kj::HOURS);

  f.advanceTo(f.t0 + 1 * kj::HOURS);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0 + 1 * kj::HOURS);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler runs a queued alarm instead of retrying the failed one") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  f.scheduler->setAlarm(f.actor, f.t0 + 1 * kj::MINUTES);

  run->fulfill(failedCounted());
  f.advanceTo(f.t0 + 3 * kj::SECONDS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0 + 1 * kj::MINUTES);

  f.advanceTo(f.t0 + 1 * kj::MINUTES);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0 + 1 * kj::MINUTES);
  KJ_EXPECT(f.lastRun().retryCount == 0);
}

KJ_TEST("AlarmScheduler runs a queued alarm for the same time instead of retrying the failed one") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  f.scheduler->setAlarm(f.actor, f.t0);

  run->fulfill(failedCounted());
  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler deletes only the queued alarm while the handler runs") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  f.scheduler->setAlarm(f.actor, f.t0 + 1 * kj::HOURS);

  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);

  run->fulfill(succeeded());
  f.advanceTo(f.t0 + 2 * kj::HOURS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler still retries an alarm deleted while its handler ran") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  // The running alarm stays in memory, though its row is gone.
  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);

  run->fulfill(failedCounted());
  f.advanceTo(f.t0);
  // Waiting to retry, the alarm is reported again.
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0);

  f.expectRetry(f.t0, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.lastRun().retryCount == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler cancels a retry when the alarm is deleted during the backoff") {
  SchedulerFixture f;
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0);

  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  f.advanceTo(f.t0 + 1 * kj::HOURS);
  KJ_EXPECT(f.actors.runs.size() == 1);
}

KJ_TEST("AlarmScheduler queues an alarm set during the backoff behind the retry") {
  SchedulerFixture f;
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);

  // During the backoff the retrying alarm is reported, not the one queued behind it, although the
  // queued time is what storage holds.
  auto queued = f.t0 + 10 * kj::MINUTES;
  f.scheduler->setAlarm(f.actor, queued);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0);

  f.expectRetry(f.t0, 2 * kj::SECONDS);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.lastRun().retryCount == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == queued);

  f.advanceTo(queued - 1 * kj::MILLISECONDS);
  KJ_EXPECT(f.actors.runs.size() == 2);
  f.advanceTo(queued);
  KJ_ASSERT(f.actors.runs.size() == 3);
  KJ_EXPECT(f.lastRun().scheduledTime == queued);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

// Deleting an alarm during its backoff, with another queued behind it, deletes the stored row and
// cancels the retry, but schedules the queued alarm in memory only.
KJ_TEST("AlarmScheduler runs the queued alarm when one is deleted during its backoff") {
  SchedulerFixture f;
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  auto queued = f.t0 + 10 * kj::MINUTES;
  f.scheduler->setAlarm(f.actor, queued);

  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == queued);

  // No retry.
  f.advanceTo(f.t0 + 3 * kj::SECONDS);
  KJ_EXPECT(f.actors.runs.size() == 1);

  f.advanceTo(queued - 1 * kj::MILLISECONDS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  f.advanceTo(queued);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == queued);
  KJ_EXPECT(f.lastRun().retryCount == 0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler does not store the queued alarm when one is deleted during its backoff") {
  SchedulerFixture f;
  f.actors.then(failedCounted());
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  auto queued = f.t0 + 10 * kj::MINUTES;
  f.scheduler->setAlarm(f.actor, queued);
  KJ_EXPECT(f.scheduler->deleteAlarm(f.actor));
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == queued);

  // A restart before the queued alarm's time loses it.
  KJ_EXPECT(f.open().getAlarm(f.actor) == kj::none);
  f.advanceTo(queued + 1 * kj::HOURS);
  KJ_EXPECT(f.actors.runs.size() == 1);
}

KJ_TEST("AlarmScheduler keeps an alarm whose abandonment failed, but runs it only after restart") {
  SchedulerFixture f;
  f.actors.then(AlarmResult{.retry = false, .outcome = EventOutcome::ABORTED});
  f.actors.failAbandon = true;
  f.scheduler->setAlarm(f.actor, f.t0);
  f.advanceTo(f.t0);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.actors.abandons.size() == 1);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == f.t0);

  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  KJ_EXPECT(f.actors.abandons.size() == 1);

  f.open();
  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_ASSERT(f.actors.runs.size() == 2);
  KJ_EXPECT(f.lastRun().scheduledTime == f.t0);
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
}

KJ_TEST("AlarmScheduler deleteAll() cancels running and waiting alarms and empties storage") {
  SchedulerFixture f;
  auto run = f.actors.thenHang();
  auto other = ActorKey("other"_kj, kj::none);
  f.scheduler->setAlarm(f.actor, f.t0);
  f.scheduler->setAlarm(other, f.t0 + 1 * kj::HOURS);
  f.advanceTo(f.t0);
  KJ_ASSERT(f.actors.runs.size() == 1);
  KJ_EXPECT(run->isWaiting());

  f.scheduler->deleteAll();
  KJ_EXPECT(!run->isWaiting());
  KJ_EXPECT(f.scheduler->getAlarm(f.actor) == kj::none);
  KJ_EXPECT(f.scheduler->getAlarm(other) == kj::none);

  f.advanceTo(f.t0 + 1 * kj::DAYS);
  KJ_EXPECT(f.actors.runs.size() == 1);
  auto& reopened = f.open();
  KJ_EXPECT(reopened.getAlarm(f.actor) == kj::none);
  KJ_EXPECT(reopened.getAlarm(other) == kj::none);
}

}  // namespace
}  // namespace workerd::server
