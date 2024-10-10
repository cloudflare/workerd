// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-metadata.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("SQLite-METADATA") {
  auto dir = kj::newInMemoryDirectory(kj::nullClock());
  SqliteDatabase::Vfs vfs(*dir);
  SqliteDatabase db(vfs, kj::Path({"foo"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  SqliteMetadata metadata(db);

  // Initial state has empty alarm
  KJ_EXPECT(metadata.getAlarm() == kj::none);

  // Can set alarm to an explicit time
  constexpr kj::Date anAlarmTime1 =
      kj::UNIX_EPOCH + 1734099316 * kj::SECONDS + 987654321 * kj::NANOSECONDS;
  metadata.setAlarm(anAlarmTime1);

  // Can get the set alarm time
  KJ_EXPECT(metadata.getAlarm() == anAlarmTime1);

  // Can overwrite the alarm time
  constexpr kj::Date anAlarmTime2 = anAlarmTime1 + 1 * kj::NANOSECONDS;
  metadata.setAlarm(anAlarmTime2);
  KJ_EXPECT(metadata.getAlarm() != anAlarmTime1);
  KJ_EXPECT(metadata.getAlarm() == anAlarmTime2);

  // Can clear alarm
  metadata.setAlarm(kj::none);
  KJ_EXPECT(metadata.getAlarm() == kj::none);

  // Zero alarm is distinct from unset (probably not important, but just checking)
  metadata.setAlarm(kj::UNIX_EPOCH);
  KJ_EXPECT(metadata.getAlarm() == kj::UNIX_EPOCH);

  // Can recreate table after resetting database
  metadata.setAlarm(anAlarmTime1);
  KJ_EXPECT(metadata.getAlarm() == anAlarmTime1);
  db.reset();
  KJ_EXPECT(metadata.getAlarm() == kj::none);
  metadata.setAlarm(anAlarmTime2);
  KJ_EXPECT(KJ_ASSERT_NONNULL(metadata.getAlarm()) == anAlarmTime2);

  // Can invalidate cache after rolling back.
  metadata.setAlarm(anAlarmTime2);
  db.run("BEGIN TRANSACTION");
  metadata.setAlarm(anAlarmTime1);
  KJ_EXPECT(metadata.getAlarm() == anAlarmTime1);
  db.run("ROLLBACK TRANSACTION");
  KJ_EXPECT(metadata.getAlarm() == anAlarmTime2);
}

}  // namespace
}  // namespace workerd
