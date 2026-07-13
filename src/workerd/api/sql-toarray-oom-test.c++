// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-state.h"
#include "sql.h"

#include <workerd/io/actor-cache.h>
#include <workerd/io/actor-sqlite.h>
#include <workerd/io/io-gate.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/sqlite.h>

#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

using workerd::TestFixture;

jsg::Arguments<SqlStorage::BindingValue> noBindings() {
  return jsg::Arguments<SqlStorage::BindingValue>(kj::Array<SqlStorage::BindingValue>());
}

// A SELECT over a literal VALUES list returns rows without performing any writes, so we don't have
// to drive the output gate / commit machinery just to get a cursor with multiple rows.
constexpr kj::StringPtr kFiveRowQuery =
    "SELECT column1 AS v FROM (VALUES (1), (2), (3), (4), (5))"_kj;

// Runs `callback` with a freshly-built, SQL-enabled DurableObjectStorage inside an IoContext.
void runWithSql(TestFixture& fixture, kj::Function<void(jsg::Lock&, SqlStorage&)> callback) {
  fixture.runInIoContext([&callback](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto dir = kj::newInMemoryDirectory(kj::nullClock());
    SqliteDatabase::Vfs vfs(*dir);
    OutputGate gate;
    auto db = kj::heap<SqliteDatabase>(
        vfs, kj::Path({"db"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    ActorSqlite actorSqlite(
        kj::mv(db), gate, [](SpanParent) -> kj::Promise<void> { return kj::READY_NOW; });

    auto storage = js.alloc<DurableObjectStorage>(js,
        IoContext::current().addObject(static_cast<ActorCacheInterface&>(actorSqlite)),
        /*enableSql=*/true);
    auto sql = storage->getSql(js);

    callback(js, *sql);
    return kj::READY_NOW;
  });
}

KJ_TEST("SqlStorage::Cursor::toArray() returns all rows when the isolate is healthy") {
  TestFixture fixture;
  runWithSql(fixture, [](jsg::Lock& js, SqlStorage& sql) {
    auto rows = sql.exec(js, js.str(kFiveRowQuery), noBindings())->toArray(js);
    KJ_EXPECT(rows.size() == 5);
  });
}

KJ_TEST("SqlStorage::Cursor::toArray() aborts once the isolate has excessively exceeded its heap "
        "limit") {
  TestFixture fixture;
  runWithSql(fixture, [&fixture](jsg::Lock& js, SqlStorage& sql) {
    // Simulate the isolate having been condemned for excessively exceeding its heap limit, as the
    // NearHeapLimitCallback would do synchronously mid-iteration in production.
    fixture.setHeapLimitExcessivelyExceeded(true);

    auto cursor = sql.exec(js, js.str(kFiveRowQuery), noBindings());
    KJ_EXPECT_THROW_MESSAGE("result set is too large to fit in memory", cursor->toArray(js));
  });
}

KJ_TEST("SqlStorage::Cursor::toArray() does not throw on an empty result set even when the isolate "
        "is already condemned") {
  TestFixture fixture;
  runWithSql(fixture, [&fixture](jsg::Lock& js, SqlStorage& sql) {
    fixture.setHeapLimitExcessivelyExceeded(true);

    // The guard is checked only after a row is appended, so a query that yields no rows must not
    // throw the misleading "result set is too large" error.
    auto rows = sql.exec(js, js.str("SELECT column1 FROM (VALUES (1)) WHERE 0"_kj), noBindings())
                    ->toArray(js);
    KJ_EXPECT(rows.size() == 0);
  });
}

}  // namespace
}  // namespace workerd::api
