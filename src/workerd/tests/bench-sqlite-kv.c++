// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/tests/bench-tools.h>
#include <workerd/util/sqlite-kv.h>

#include <kj/filesystem.h>

// Compares listing a KV range with and without reading the `value` column, across value sizes.
// `_cf_KV` is WITHOUT ROWID, so it is an index b-tree whose maximum local payload is far smaller
// than a table b-tree's; values spill to overflow pages at a correspondingly smaller size, and
// listing keys is where reading them back is pure waste.
//
// The database lives in an in-memory directory, so this measures the CPU cost of materializing
// values — b-tree overflow traversal and copying — and not cold-cache disk I/O. A deployment
// reading overflow pages from disk should see a larger gap than this benchmark reports.

namespace workerd {
namespace {

struct GlobalInit {
  GlobalInit() {
    installSqliteCustomAllocator();
  }
};

static GlobalInit init;

constexpr size_t NUM_KEYS = 1000;

struct Fixture {
  kj::Own<const kj::Directory> dir;
  SqliteDatabase::Vfs vfs;
  SqliteDatabase db;
  SqliteKv kv;

  explicit Fixture(size_t valueSize)
      : dir(kj::newInMemoryDirectory(kj::nullClock())),
        vfs(*dir),
        db(vfs, kj::Path({"bench"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY),
        kv(db) {
    auto value = kj::heapArray<kj::byte>(valueSize);
    for (auto& b: value) {
      b = 'x';
    }

    for (size_t i = 0; i < NUM_KEYS; ++i) {
      kv.put(kj::str("key-", i), value);
    }
  }
};

static void SqliteKv_ListWithValues(benchmark::State& state) {
  Fixture fixture(state.range(0));

  for (auto _: state) {
    auto cursor = fixture.kv.list(nullptr, kj::none, kj::none, SqliteKv::FORWARD);
    size_t count = 0;
    for (;;) {
      KJ_IF_SOME(pair, cursor->next()) {
        benchmark::DoNotOptimize(pair.key.size());
        benchmark::DoNotOptimize(pair.value.size());
        ++count;
      } else {
        break;
      }
    }
    KJ_ASSERT(count == NUM_KEYS);
    benchmark::DoNotOptimize(count);
  }
}

static void SqliteKv_ListKeysOnly(benchmark::State& state) {
  Fixture fixture(state.range(0));

  for (auto _: state) {
    auto cursor = fixture.kv.listKeys(nullptr, kj::none, kj::none, SqliteKv::FORWARD);
    size_t count = 0;
    for (;;) {
      KJ_IF_SOME(key, cursor->nextKey()) {
        benchmark::DoNotOptimize(key.size());
        ++count;
      } else {
        break;
      }
    }
    KJ_ASSERT(count == NUM_KEYS);
    benchmark::DoNotOptimize(count);
  }
}

WD_BENCHMARK(SqliteKv_ListWithValues)
    ->Name("SqliteKv::ListWithValues")
    ->Arg(256)
    ->Arg(1024)
    ->Arg(8192)
    ->Arg(65536);
WD_BENCHMARK(SqliteKv_ListKeysOnly)
    ->Name("SqliteKv::ListKeysOnly")
    ->Arg(256)
    ->Arg(1024)
    ->Arg(8192)
    ->Arg(65536);

}  // namespace
}  // namespace workerd
