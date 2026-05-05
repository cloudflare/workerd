// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-metering.h"

#include <kj/test.h>
#include <kj/thread.h>

#include <atomic>

namespace workerd {
namespace {

void test_sqlite3_mem_methods(bool expectEnforcedLimits) {
  // sqliteMemMalloc with a negative size returns a nullptr.
  KJ_EXPECT(sqliteMemMalloc(-1) == nullptr);

  // sqliteMemMalloc with a zero size returns a nullptr.
  KJ_EXPECT(sqliteMemMalloc(0) == nullptr);

  // sqliteMemMalloc with a positive size returns a non-nullptr.
  void* ptr = sqliteMemMalloc(1024);
  KJ_EXPECT(ptr != nullptr);

  // sqliteMemFree on a non-nullptr does not throw.
  sqliteMemFree(ptr);

  // sqliteMemMalloc with a larger size. Note that the size allocated here needs to be less than 1
  // MiB below, or the pointer address will be reused when requesting 1 MiB.
  ptr = sqliteMemMalloc(1024 * 512 + 1);
  if (expectEnforcedLimits) {
    KJ_EXPECT(ptr == nullptr);
  } else {
    KJ_EXPECT(ptr != nullptr);
  }
  sqliteMemFree(ptr);

  // sqliteMemFree on a nullptr does not throw.
  sqliteMemFree(nullptr);

  // sqliteMemRealloc(nullptr, <n>) should behave as sqliteMemMalloc(<n>).
  ptr = sqliteMemRealloc(nullptr, -1);
  KJ_EXPECT(ptr == nullptr);
  ptr = sqliteMemRealloc(nullptr, 0);
  KJ_EXPECT(ptr == nullptr);
  ptr = sqliteMemRealloc(nullptr, 1024);
  KJ_EXPECT(ptr != nullptr);
  sqliteMemFree(ptr);

  // sqliteMemRealloc should return a different pointer if a larger size is requested.
  ptr = sqliteMemMalloc(1024);
  void* new_ptr = sqliteMemRealloc(ptr, 1024 * 1024);
  KJ_EXPECT(ptr != new_ptr);
  KJ_EXPECT(ptr != nullptr);
  if (expectEnforcedLimits) {
    // Realloc was rejected: new_ptr is nullptr and ptr still owns the original allocation.
    KJ_EXPECT(new_ptr == nullptr);
    sqliteMemFree(ptr);
  } else {
    KJ_EXPECT(new_ptr != nullptr);
    sqliteMemFree(new_ptr);
  }

  // sqliteMemRealloc with a valid pointer and negative size returns a nullptr. Note that ptr is
  // implicitly freed by the preceding call to sqliteMemRealloc.
  ptr = sqliteMemMalloc(1024);
  KJ_EXPECT(ptr != nullptr);
  new_ptr = sqliteMemRealloc(ptr, -1);
  KJ_EXPECT(new_ptr == nullptr);
  // sqliteMemRealloc with a valid pointer and zero size returns a nullptr. Note again that ptr is
  // implicitly freed by the preceding call to sqliteMemRealloc.
  ptr = sqliteMemMalloc(1024);
  KJ_EXPECT(ptr != nullptr);
  new_ptr = sqliteMemRealloc(ptr, 0);
  KJ_EXPECT(new_ptr == nullptr);
}

KJ_TEST("sqlite3_mem_methods work without SqliteMemoryScope") {
  // We expect a bunch of error logs, but we do not crash.
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemRealloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  KJ_EXPECT_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  test_sqlite3_mem_methods(/*expectEnforcedLimits*/ false);
}

KJ_TEST("sqlite3_mem_methods work with SqliteMemoryScope and high limits") {
  size_t memoryBytes = 0;
  SqliteMemoryScope scope(memoryBytes, 2 * 1024 * 1024);
  test_sqlite3_mem_methods(/*expectEnforcedLimits*/ false);
}

KJ_TEST("sqlite3_mem_methods work with SqliteMemoryScope and low limits") {
  size_t memoryBytes = 0;
  SqliteMemoryScope scope(memoryBytes, 512 * 1024);
  test_sqlite3_mem_methods(/*expectEnforcedLimits*/ true);
}

KJ_TEST("sqlite3_mem_methods correctly modify the SqliteMemoryScope memoryBytes") {
  auto runThread = [&]() {
    size_t memoryBytes = 0;
    SqliteMemoryScope scope(memoryBytes, 1024 * 1024);

    // sqliteMemMalloc followed by sqliteMemFree should result in a zero memoryBytes.
    void* p1 = sqliteMemMalloc(1024);
    KJ_EXPECT(p1 != nullptr);
    KJ_EXPECT(memoryBytes >= 1024);
    sqliteMemFree(p1);
    KJ_EXPECT(memoryBytes == 0);

    // Multiple sqliteMemMalloc's should increase memoryBytes respectively.
    void* p2 = sqliteMemMalloc(4096);
    KJ_EXPECT(p2 != nullptr);
    KJ_EXPECT(memoryBytes >= 4096);
    void* p3 = sqliteMemMalloc(4096);
    KJ_EXPECT(p3 != nullptr);
    KJ_EXPECT(memoryBytes >= 8192);
    size_t memoryBytesSnapshot = memoryBytes;

    // sqliteMemMalloc leaves memoryBytes unchanged when hardLimitBytes is exceeded.
    KJ_EXPECT(sqliteMemMalloc(1024 * 1024) == nullptr);
    KJ_EXPECT(memoryBytes == memoryBytesSnapshot);

    // sqliteMemRealloc with an increase in size should increase memoryBytes respectively, but less than
    // a sqliteMemMalloc of equivalent size.
    void* p4 = sqliteMemRealloc(p2, 8192);
    KJ_EXPECT(p4 != nullptr);
    KJ_EXPECT(memoryBytes >= 12288);
    KJ_EXPECT(memoryBytes <= 16384);

    // sqliteMemRealloc with an decrease in size should decrease memoryBytes respectively.
    void* p5 = sqliteMemRealloc(p3, 1024);
    KJ_EXPECT(p5 != nullptr);
    KJ_EXPECT(memoryBytes >= 9216);
    KJ_EXPECT(memoryBytes <= 12288);
    memoryBytesSnapshot = memoryBytes;

    // sqliteMemRealloc leaves memoryBytes unchanged when hardLimitBytes is exceeded.
    KJ_EXPECT(sqliteMemRealloc(p5, 1024 * 1024) == nullptr);
    KJ_EXPECT(memoryBytes == memoryBytesSnapshot);

    // sqliteMemFree on all active pointers should result in a zero memoryBytes.
    sqliteMemFree(p4);
    sqliteMemFree(p5);
    KJ_EXPECT(memoryBytes == 0);
  };

  kj::Thread thread1(runThread);
  kj::Thread thread2(runThread);
}

KJ_TEST("SqliteMemoryScope is idempotent and handles nested scopes") {
  size_t memoryBytes = 0;
  SqliteMemoryScope outerScope(memoryBytes, 1024 * 1024);

  void* p1 = sqliteMemMalloc(1024);
  KJ_EXPECT(p1 != nullptr);
  size_t memoryBytesSnapshot = memoryBytes;
  KJ_EXPECT(memoryBytesSnapshot >= 1024);

  {
    size_t innerMemoryBytes = 0;
    SqliteMemoryScope innerScope(innerMemoryBytes, 512);

    void* p2 = sqliteMemMalloc(2048);
    KJ_EXPECT(p2 != nullptr);
    KJ_EXPECT(memoryBytes > memoryBytesSnapshot);
    KJ_EXPECT(innerMemoryBytes == 0);

    sqliteMemFree(p2);
  }

  void* p3 = sqliteMemMalloc(512);
  KJ_EXPECT(p3 != nullptr);
  KJ_EXPECT(memoryBytes >= memoryBytesSnapshot);

  sqliteMemFree(p1);
  sqliteMemFree(p3);
  KJ_EXPECT(memoryBytes == 0);
}

}  // namespace
}  // namespace workerd
