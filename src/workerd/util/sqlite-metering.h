// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <stddef.h>

#include <kj/common.h>

namespace workerd {

// This module implements per-database SQLite memory metering.
//
// SQLite uses a single process-wide memory allocator, but we want to account allocations against
// the specific SqliteDatabase on whose behalf they are made, and enforce a hard limit for memory
// allocations per-database. We do this by:
//
// 1. Installing a custom sqlite3_mem_methods that wraps the system allocator.
//
// 2. SqliteDatabase wraps every sqlite3_* entry point that may allocate or deallocate memory with
//    a stack-allocated SqliteMemoryScope. The scope points at a size_t byte counter owned by
//    SqliteDatabase to meter memory allocations.
//
// 3. When a SqliteMemoryScope is active on the current thread, we count each memory allocation
//    against the byte counter and enforce the per-database hard limit by returning nullptr, signalling
//    SQLite to throw a SQLITE_NOMEM exception.
//
// SqliteMemoryScope is idempotent: if a scope is already active on the current thread, additional
// scopes are no-ops. This allows safe nesting (e.g., when a column accessor calls into SQLite while
// a Query::nextRow() scope is already active).
class SqliteMemoryScope {
 public:
  // memoryBytes: the per-database byte counter owned by SqliteDatabase for its lifetime.
  // maxMemoryBytes: the per-database cap from WorkerLimits::sqliteMaxMemoryMb.
  //
  // If a scope is already active on this thread, this constructor is a no-op (idempotent).
  explicit SqliteMemoryScope(size_t& memoryBytes, size_t maxMemoryBytes);

  ~SqliteMemoryScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SqliteMemoryScope);

 private:
  // These are accessed by the allocator functions via threadLocalScope.
  size_t& memoryBytes;
  const size_t maxMemoryBytes;

  // Thread-local pointer to the active scope. Set to this on construction (if not already set),
  // cleared on destruction (if we set it).
  static thread_local SqliteMemoryScope* threadLocalScope;

  friend void* sqliteMemMalloc(int);
  friend void sqliteMemFree(void*);
  friend void* sqliteMemRealloc(void*, int);
};

// The custom sqlite3_mem_methods functions. Declared here so tests can call them directly to
// verify accounting behaviour without going through the SQLite query engine.
void* sqliteMemMalloc(int size);
void sqliteMemFree(void* ptr);
void* sqliteMemRealloc(void* ptr, int newSize);

// Install the custom sqlite3_mem_methods.
//
// This must be called before the first sqlite3_initialize(), sqlite3_open_v2(), or
// sqlite3_vfs_register() call in the process. Idempotent (uses a static once-flag).
void installSqliteCustomAllocator();

}  // namespace workerd
