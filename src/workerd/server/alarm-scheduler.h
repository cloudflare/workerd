// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/async.h>
#include <kj/time.h>
#include <kj/timer.h>
#include <kj/map.h>

#include <random>

#include <workerd/util/sqlite.h>
#include <workerd/io/worker-interface.h>

namespace workerd::server {

using byte = kj::byte;

struct ActorKey {
  kj::StringPtr uniqueKey;
  kj::StringPtr actorId;

  bool operator==(const ActorKey& other) const {
    return uniqueKey == other.uniqueKey && actorId == other.actorId;
  }

  kj::Own<ActorKey> clone() const {
    auto ownUniqueKey = kj::str(uniqueKey);
    auto ownActorId = kj::str(actorId);

    return kj::attachVal(ActorKey{
      .uniqueKey = ownUniqueKey, .actorId = ownActorId
    }, kj::mv(ownUniqueKey), kj::mv(ownActorId));
  }
};

inline uint KJ_HASHCODE(const ActorKey& k) { return kj::hashCode(k.uniqueKey, k.actorId); }

class AlarmScheduler final : kj::TaskSet::ErrorHandler {
  // Allows scheduling alarm executions at specific times, returning a promise representing
  // the completion of the alarm event.
public:
  static constexpr auto RETRY_START_SECONDS = WorkerInterface::ALARM_RETRY_START_SECONDS;

  static constexpr auto RETRY_MAX_TRIES = WorkerInterface::ALARM_RETRY_MAX_TRIES;
  // Max number of "valid" retry attempts, i.e the worker returned an error

  static constexpr auto RETRY_BACKOFF_MAX = 9;
  // Bound for exponential backoff when RETRY_MAX_TRIES is exceeded due to internal errors.
  // 2 << 9 is 1024 seconds, about 17 minutes. Total time spent in retries once the backoff limit
  // is reached is over 30 minutes.

  static constexpr auto RETRY_JITTER_FACTOR = 0.25;
  // How much jitter should be applied to retry times to avoid bundled retries overloading
  // some common dependency between a set of failed alarms

  using GetActorFn = kj::Function<kj::Own<WorkerInterface>(kj::String)>;

  AlarmScheduler(
    const kj::Clock& clock,
    kj::Timer& timer,
    const SqliteDatabase::Vfs& vfs,
    kj::PathPtr path);

  kj::Maybe<kj::Date> getAlarm(ActorKey actor);
  bool setAlarm(ActorKey actor, kj::Date scheduledTime);
  bool deleteAlarm(ActorKey actor);

  void registerNamespace(kj::StringPtr uniqueKey, GetActorFn getActor);

private:
  const kj::Clock& clock;
  kj::Timer& timer;
  std::default_random_engine random;

  struct Namespace {
    GetActorFn getActor;
  };
  kj::HashMap<kj::StringPtr, Namespace> namespaces;
  kj::Own<SqliteDatabase> db;
  kj::TaskSet tasks;

  struct ScheduledAlarm {
    kj::Own<ActorKey> actor;
    kj::Date scheduledTime;
    kj::Promise<void> task;
    kj::Maybe<kj::Date> queuedAlarm = nullptr;
    // Once started, an alarm can have a single alarm queued behind it.
    bool started = false;

    bool previousRetryCountedAgainstLimit = false;

    // Counter for calculating backoff -- separate from retry, so we can reset backoff without losing
    // the total count of retry attempts
    uint32_t backoff = 0;

    // Counter for retry attempts, whether or not they apply to the limit
    uint32_t retry = 0;

    // Counter for retry attempts that apply to the retry limit.
    uint32_t countedRetry = 0;
  };

  kj::HashMap<ActorKey, ScheduledAlarm> alarms;

  struct RetryInfo {
    bool retry;
    bool retryCountsAgainstLimit;
  };
  kj::Promise<RetryInfo> runAlarm(const ActorKey& actor, kj::Date scheduledTime);

  void setAlarmInMemory(kj::Own<ActorKey> actor, kj::Date scheduledTime);

  ScheduledAlarm scheduleAlarm(kj::Date now, kj::Own<ActorKey> actor, kj::Date scheduledTime);

  kj::Promise<void> makeAlarmTask(kj::Duration delay, const ActorKey& actor, kj::Date scheduledTime);

  SqliteDatabase::Statement stmtSetAlarm = db->prepare(R"(
    INSERT INTO _cf_ALARM VALUES(?, ?, ?)
      ON CONFLICT DO UPDATE SET scheduled_time = excluded.scheduled_time;
  )");
  SqliteDatabase::Statement stmtDeleteAlarm = db->prepare(R"(
    DELETE FROM _cf_ALARM WHERE actor_unique_key = ? AND actor_id = ?
  )");

  void taskFailed(kj::Exception&& exception) override;

  int maxJitterMsForDelay(kj::Duration delay);

  static void ensureInitialized(SqliteDatabase& db);
  void loadAlarmsFromDb();
};

} // namespace workerd::server
