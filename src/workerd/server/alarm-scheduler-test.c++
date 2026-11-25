// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "alarm-scheduler.h"

#include <kj/async-io.h>
#include <kj/filesystem.h>
#include <kj/test.h>
#include <kj/timer.h>

#include <algorithm>

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

// An alarm a rehome test seeds and then expects to find in its actor's home database.
struct RehomedAlarm {
  kj::StringPtr actorId;
  kj::Date time;
  kj::Maybe<kj::StringPtr> name;
};

// The home thread of the rehome tests' actors: their ids start with a letter, which picks it.
uint homeOf(kj::StringPtr actorId, uint threads) {
  return uint(actorId[0] - 'a') % threads;
}

kj::Function<uint(kj::StringPtr)> homeFn(uint threads) {
  return [threads](kj::StringPtr actorId) { return homeOf(actorId, threads); };
}

void seedAlarms(const SqliteDatabase::Vfs& vfs,
    kj::Timer& timer,
    uint thread,
    kj::ArrayPtr<const RehomedAlarm> alarms) {
  AlarmScheduler scheduler(
      kj::nullClock(), timer, vfs, AlarmScheduler::databasePath(thread), failingGetActor());
  for (auto& alarm: alarms) {
    scheduler.setAlarm(ActorKey(alarm.actorId, alarm.name), alarm.time);
  }
}

// Checks that each alarm is in the database of its home among `threads` threads, with its time
// and name, and in no other.
void expectRehomed(const SqliteDatabase::Vfs& vfs,
    kj::Timer& timer,
    uint threads,
    kj::ArrayPtr<const RehomedAlarm> alarms) {
  for (uint thread = 0; thread < threads; thread++) {
    {
      AlarmScheduler scheduler(
          kj::nullClock(), timer, vfs, AlarmScheduler::databasePath(thread), failingGetActor());
      for (auto& alarm: alarms) {
        auto found = scheduler.getAlarm(ActorKey(alarm.actorId, kj::none));
        if (homeOf(alarm.actorId, threads) == thread) {
          KJ_EXPECT(found == alarm.time, alarm.actorId, thread);
        } else {
          KJ_EXPECT(found == kj::none, alarm.actorId, thread);
        }
      }
    }

    SqliteDatabase db(vfs, AlarmScheduler::databasePath(thread), kj::WriteMode::MODIFY);
    int64_t homed = 0;
    for (auto& alarm: alarms) {
      if (homeOf(alarm.actorId, threads) != thread) {
        continue;
      }
      homed++;
      auto query = db.run(
          "SELECT scheduled_time, actor_name FROM _cf_ALARM WHERE actor_id = ?", alarm.actorId);
      KJ_ASSERT(!query.isDone(), alarm.actorId, thread);
      KJ_EXPECT(query.getInt64(0) == toNs(alarm.time), alarm.actorId, thread);
      KJ_EXPECT(query.getMaybeText(1) == alarm.name, alarm.actorId, thread);
    }
    auto count = db.run("SELECT COUNT(*) FROM _cf_ALARM");
    KJ_EXPECT(count.getInt64(0) == homed, thread);
  }
}

// The directory's file names, sorted, without the `-wal` files SQLite may leave behind.
kj::String listFiles(const kj::Directory& dir) {
  auto names = dir.listNames();
  std::sort(names.begin(), names.end());
  kj::Vector<kj::StringPtr> kept;
  for (auto& name: names) {
    if (!name.endsWith("-wal")) {
      kept.add(name);
    }
  }
  return kj::strArray(kept, " ");
}

void writeFile(const kj::Directory& dir, kj::StringPtr name, kj::StringPtr content) {
  dir.openFile(kj::Path(name), kj::WriteMode::CREATE)->writeAll(content);
}

kj::String readFile(const kj::Directory& dir, kj::StringPtr name) {
  return dir.openFile(kj::Path(name))->readAllText();
}

KJ_TEST("AlarmScheduler::databasePath names thread 0's database metadata.sqlite") {
  KJ_EXPECT(AlarmScheduler::databasePath(0).toString() == "metadata.sqlite");
  KJ_EXPECT(AlarmScheduler::databasePath(1).toString() == "metadata-1.sqlite");
  KJ_EXPECT(AlarmScheduler::databasePath(13).toString() == "metadata-13.sqlite");
}

KJ_TEST("AlarmScheduler::rehome spreads the alarms of metadata.sqlite over four threads") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  const RehomedAlarm alarms[] = {
    {"a-one"_kj, kj::UNIX_EPOCH + 24 * kj::HOURS, "first"_kj},
    {"b-one"_kj, kj::UNIX_EPOCH + 25 * kj::HOURS, "second"_kj},
    {"c-one"_kj, kj::UNIX_EPOCH + 26 * kj::HOURS, kj::none},
    {"d-one"_kj, kj::UNIX_EPOCH + 27 * kj::HOURS, "fourth"_kj},
    {"e-one"_kj, kj::UNIX_EPOCH + 28 * kj::HOURS, "fifth"_kj},
    {"b-two"_kj, kj::UNIX_EPOCH + 29 * kj::HOURS, kj::none},
    {"h-one"_kj, kj::UNIX_EPOCH + 30 * kj::HOURS, "eighth"_kj},
  };
  seedAlarms(vfs, timer, 0, alarms);

  AlarmScheduler::rehome(vfs, *dir, 4, homeFn(4));

  KJ_EXPECT(
      listFiles(*dir) == "metadata-1.sqlite metadata-2.sqlite metadata-3.sqlite metadata.sqlite");
  expectRehomed(vfs, timer, 4, alarms);
}

KJ_TEST("AlarmScheduler::rehome shrinks four threads to two and then to one") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  const RehomedAlarm alarms[] = {
    {"a-one"_kj, kj::UNIX_EPOCH + 24 * kj::HOURS, "first"_kj},
    {"b-one"_kj, kj::UNIX_EPOCH + 25 * kj::HOURS, kj::none},
    {"c-one"_kj, kj::UNIX_EPOCH + 26 * kj::HOURS, "third"_kj},
    {"d-one"_kj, kj::UNIX_EPOCH + 27 * kj::HOURS, "fourth"_kj},
    {"c-two"_kj, kj::UNIX_EPOCH + 28 * kj::HOURS, kj::none},
    {"d-two"_kj, kj::UNIX_EPOCH + 29 * kj::HOURS, "eighth"_kj},
  };
  seedAlarms(vfs, timer, 0, alarms);
  AlarmScheduler::rehome(vfs, *dir, 4, homeFn(4));
  expectRehomed(vfs, timer, 4, alarms);

  // Side files of the databases that go must go with them.
  writeFile(*dir, "metadata-2.sqlite-shm", "");
  writeFile(*dir, "metadata-3.sqlite-shm", "");
  writeFile(*dir, "metadata-3.sqlite-wal", "");

  AlarmScheduler::rehome(vfs, *dir, 2, homeFn(2));
  KJ_EXPECT(listFiles(*dir) == "metadata-1.sqlite metadata.sqlite");
  for (auto& name: dir->listNames()) {
    KJ_EXPECT(!name.startsWith("metadata-2") && !name.startsWith("metadata-3"), name);
  }
  expectRehomed(vfs, timer, 2, alarms);

  AlarmScheduler::rehome(vfs, *dir, 1, homeFn(1));
  KJ_EXPECT(listFiles(*dir) == "metadata.sqlite");
  for (auto& name: dir->listNames()) {
    KJ_EXPECT(!name.startsWith("metadata-"), name);
  }
  expectRehomed(vfs, timer, 1, alarms);
}

KJ_TEST("AlarmScheduler::rehome keeps the earlier time of an alarm found in two databases") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  auto early = kj::UNIX_EPOCH + 24 * kj::HOURS;
  auto late = kj::UNIX_EPOCH + 48 * kj::HOURS;

  // Both actors are at home on thread 1 of two. `b-one` is earlier where it does not belong,
  // `d-one` is earlier at home; each has its name in only one of the two databases.
  const RehomedAlarm away[] = {
    {"b-one"_kj, early, "bee"_kj},
    {"d-one"_kj, late, "dee"_kj},
  };
  const RehomedAlarm atHome[] = {
    {"b-one"_kj, late, kj::none},
    {"d-one"_kj, early, kj::none},
  };
  seedAlarms(vfs, timer, 0, away);
  seedAlarms(vfs, timer, 1, atHome);

  AlarmScheduler::rehome(vfs, *dir, 2, homeFn(2));

  const RehomedAlarm expected[] = {
    {"b-one"_kj, early, "bee"_kj},
    {"d-one"_kj, early, "dee"_kj},
  };
  expectRehomed(vfs, timer, 2, expected);
}

KJ_TEST("AlarmScheduler::rehome leaves files that are not scheduler databases alone") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);

  // None of these is a database, so opening any as one would fail.
  const kj::StringPtr others[] = {
    "0123456789abcdef.sqlite"_kj,
    "metadata-+1.sqlite"_kj,
    "metadata-.sqlite"_kj,
    "metadata-0.sqlite"_kj,
    "metadata-01.sqlite"_kj,
    "metadata-1.sqlite.bak"_kj,
    "metadata-1x.sqlite"_kj,
    "metadata-x.sqlite"_kj,
    "metadata.3.sqlite"_kj,
    "metadata.sqlite.bak"_kj,
  };
  for (auto name: others) {
    writeFile(*dir, name, kj::str("not a database: ", name));
  }

  const RehomedAlarm alarms[] = {
    {"a-one"_kj, kj::UNIX_EPOCH + 24 * kj::HOURS, "first"_kj},
    {"b-one"_kj, kj::UNIX_EPOCH + 25 * kj::HOURS, "second"_kj},
  };
  seedAlarms(vfs, timer, 3, alarms);

  AlarmScheduler::rehome(vfs, *dir, 1, homeFn(1));

  for (auto name: others) {
    KJ_EXPECT(dir->exists(kj::Path(name)), name);
    KJ_EXPECT(readFile(*dir, name) == kj::str("not a database: ", name), name);
  }
  KJ_EXPECT(!dir->exists(kj::Path("metadata-3.sqlite")));
  expectRehomed(vfs, timer, 1, alarms);
}

KJ_TEST("AlarmScheduler::rehome creates nothing when there is nothing to move") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  // An empty directory stays empty.
  {
    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    AlarmScheduler::rehome(vfs, *dir, 4, homeFn(4));
    KJ_EXPECT(dir->listNames().size() == 0);
  }

  // So does one without a scheduler database.
  {
    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    writeFile(*dir, "0123456789abcdef.sqlite", "not a database");
    AlarmScheduler::rehome(vfs, *dir, 4, homeFn(4));
    KJ_EXPECT(listFiles(*dir) == "0123456789abcdef.sqlite");
    KJ_EXPECT(readFile(*dir, "0123456789abcdef.sqlite") == "not a database");
  }

  // A metadata.sqlite whose alarms are all at home on thread 0 is not written.
  {
    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    const RehomedAlarm alarms[] = {
      {"a-one"_kj, kj::UNIX_EPOCH + 24 * kj::HOURS, "first"_kj},
      {"e-one"_kj, kj::UNIX_EPOCH + 25 * kj::HOURS, kj::none},
    };
    seedAlarms(vfs, timer, 0, alarms);
    auto before = dir->openFile(kj::Path("metadata.sqlite"))->readAllBytes();

    AlarmScheduler::rehome(vfs, *dir, 4, homeFn(4));

    KJ_EXPECT(listFiles(*dir) == "metadata.sqlite");
    KJ_EXPECT(dir->openFile(kj::Path("metadata.sqlite"))->readAllBytes().asPtr() == before.asPtr());
    expectRehomed(vfs, timer, 4, alarms);
  }
}

}  // namespace
}  // namespace workerd::server
