// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sqlite-metering.h"

#include <sqlite3.h>
#include <stdlib.h>

#include <kj/debug.h>

#if defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#define SQLITE_METERING_MALLOC_USABLE_SIZE(p) malloc_usable_size(p)
#elifdef __APPLE__
#include <malloc/malloc.h>
#define SQLITE_METERING_MALLOC_USABLE_SIZE(p) malloc_size(p)
#elifdef _WIN32
#include <malloc.h>
#define SQLITE_METERING_MALLOC_USABLE_SIZE(p) _msize(p)
#else
#error "Platform not supported"
#endif

namespace {

inline size_t usableSize(void* ptr) {
  if (ptr == nullptr) return 0;
  return SQLITE_METERING_MALLOC_USABLE_SIZE(ptr);
}

}  // namespace

namespace workerd {

thread_local SqliteMemoryScope* SqliteMemoryScope::threadLocalScope = nullptr;

void* sqliteMemMalloc(int size) {
  if (size <= 0) return nullptr;

  SqliteMemoryScope* scope = SqliteMemoryScope::threadLocalScope;
  if (scope != nullptr) {
    if (scope->memoryBytes + static_cast<size_t>(size) > scope->maxMemoryBytes) {
      return nullptr;
    }
  } else {
    // This branch is a defensive measure, if we have custom sqlite3_mem_methods installed, and we
    // must to hit this branch, then scope should not be a nullptr. If we see this, then something
    // is wrong with our memory metering.
    KJ_LOG(ERROR, "sqliteMemMalloc did not find a valid scope.");
  }

  void* ptr = malloc(static_cast<size_t>(size));
  if (ptr == nullptr) return nullptr;
  if (scope != nullptr) {
    size_t actual = usableSize(ptr);
    if (actual > 0) {
      // We have already checked that allocating the requested size will not exceed maxMemoryBytes,
      // but we still have to check that the actual size allocated does not exceed maxMemoryBytes.
      if (scope->memoryBytes + actual > scope->maxMemoryBytes) {
        free(ptr);
        return nullptr;
      }
      scope->memoryBytes += actual;
    }
  }
  return ptr;
}

void sqliteMemFree(void* ptr) {
  if (ptr == nullptr) return;
  SqliteMemoryScope* scope = SqliteMemoryScope::threadLocalScope;
  if (scope != nullptr) {
    size_t actual = usableSize(ptr);
    if (actual > 0) {
      if (scope->memoryBytes >= actual) {
        scope->memoryBytes -= actual;
      } else {
        // This branch is a defensive measure, but it is important since scope->memoryBytes is a size_t
        // and an underflow would result in all subsequent allocations failing with SQLITE_NOMEM.
        // If we see this, then something is wrong with our memory metering.
        KJ_LOG(ERROR, "sqliteMemFree would have triggered a memoryBytes underflow.");
        scope->memoryBytes = 0;
      }
    }
  } else {
    // This branch is a defensive measure, if we have custom sqlite3_mem_methods installed, and we
    // must to hit this branch, then scope should not be a nullptr. If we see this, then something
    // is wrong with our memory metering.
    KJ_LOG(ERROR, "sqliteMemFree did not find a valid scope.");
  }
  free(ptr);
}

void* sqliteMemRealloc(void* ptr, int newSize) {
  // SQLite assumes that sqliteMemRealloc(ptr, newSize) where newSize <= 0 is equivalent to
  // sqliteMemFree(ptr).
  if (newSize <= 0) {
    sqliteMemFree(ptr);
    return nullptr;
  }

  // SQLite assumes that sqliteMemRealloc(nullptr, newSize) is equivalent to
  // sqliteMemMalloc(newSize).
  if (ptr == nullptr) {
    return sqliteMemMalloc(newSize);
  }

  SqliteMemoryScope* scope = SqliteMemoryScope::threadLocalScope;
  if (scope == nullptr) {
    // This branch is a defensive measure, if we have custom sqlite3_mem_methods installed, and we
    // must to hit this branch, then scope should not be a nullptr. If we see this, then something
    // is wrong with our memory metering.
    KJ_LOG(ERROR, "sqliteMemRealloc did not find a valid scope.");
  }

  size_t oldActual = usableSize(ptr);

  // sqliteMemRealloc must leave the original buffer intact per SQLite's contract when returning
  // a nullptr. Because realloc consumes the original pointer, we must check whether the requested
  // size would exceed the limit before calling realloc.
  if (scope != nullptr && oldActual > 0 && static_cast<size_t>(newSize) > oldActual) {
    size_t growth = static_cast<size_t>(newSize) - oldActual;
    if (scope->memoryBytes + growth >= scope->maxMemoryBytes) {
      return nullptr;
    }
  }

  void* newPtr = realloc(ptr, static_cast<size_t>(newSize));
  if (newPtr == nullptr) return nullptr;

  if (scope != nullptr && oldActual > 0) {
    size_t newActual = usableSize(newPtr);
    // Note that `scope->memoryBytes + (newActual - oldActual) >= scope->maxMemoryBytes` could
    // still be true, but realloc has already freed the original memory block, so we cannot return
    // a nullptr.
    if (scope->memoryBytes + newActual >= oldActual) {
      scope->memoryBytes = scope->memoryBytes + newActual - oldActual;
    } else {
      // This branch is a defensive measure, but it is important since scope->memoryBytes is a size_t
      // and an underflow would result in all subsequent allocations failing with SQLITE_NOMEM. If
      // we see this, then something is wrong with our memory metering.
      KJ_LOG(ERROR, "sqliteMemRealloc would have triggered a memoryBytes underflow.");
      scope->memoryBytes = 0;
    }
  }
  return newPtr;
}

namespace {

int sqliteMemSize(void* ptr) {
  return static_cast<int>(usableSize(ptr));
}

// sqliteMemRoundup is used by xRoundup as a performance optimization to tell SQLite how the memory
// allocator rounds up allocation requests. This implementation rounds up to the next multiple of 8
// bytes because we assume that most allocators round up to the next 8-bytes. TCMalloc rounds up to
// the next 8 bytes for small size classes.
int sqliteMemRoundup(int n) {
  // Round up to the next multiple of 8 as a conservative estimate.
  return (n + 7) & ~7;
}

int sqliteMemInit(void* /*pAppData*/) {
  return SQLITE_OK;
}

void sqliteMemShutdown(void* /*pAppData*/) {}

static const sqlite3_mem_methods kSqliteMemMethods = {
  sqliteMemMalloc,
  sqliteMemFree,
  sqliteMemRealloc,
  sqliteMemSize,
  sqliteMemRoundup,
  sqliteMemInit,
  sqliteMemShutdown,
  /*pAppData=*/nullptr,
};

}  // namespace

SqliteMemoryScope::SqliteMemoryScope(size_t& memoryBytes, size_t maxMemoryBytes)
    : memoryBytes(memoryBytes),
      maxMemoryBytes(maxMemoryBytes) {
  if (threadLocalScope == nullptr) {
    threadLocalScope = this;
  }
}

SqliteMemoryScope::~SqliteMemoryScope() noexcept(false) {
  if (threadLocalScope == this) {
    threadLocalScope = nullptr;
  }
}

void installSqliteCustomAllocator() {
  // sqlite3_config() must be called before sqlite3_initialize(), which is itself invoked
  // implicitly by the first sqlite3_vfs_register() or sqlite3_open_v2() call. We rely on callers
  // to invoke this before any of those.
  static bool installed KJ_UNUSED = []() {
    int rc = sqlite3_config(SQLITE_CONFIG_MALLOC, &kSqliteMemMethods);
    KJ_ASSERT(rc == SQLITE_OK, "sqlite3_config(SQLITE_CONFIG_MALLOC) failed", rc);

    // Hack: Explicitly initialize SQLite now with a SqliteMemoryScope to avoid cases that
    // implicitly call sqlite3_initialize() without an active memory scope. sqlite3_initialize() is
    // idempotent per-process (https://sqlite.org/c3ref/initialize.html).
    static size_t initMemoryBytes = 0;
    SqliteMemoryScope scope(initMemoryBytes, kj::maxValue);
    sqlite3_initialize();
    return true;
  }();
}

}  // namespace workerd
