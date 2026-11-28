// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/rust/sqlite-rs/ffi.rs.h>

#include <kj/debug.h>

namespace workerd::rust::sqlite_rs {

namespace {

// What a zero-length blob binds to. SQLite binds a null pointer as NULL, not as an empty blob,
// and an empty `kj::Array` has none.
constexpr kj::byte EMPTY_BLOB[1] = {0};

kj::Own<SqliteVfs> newVfs(kj::Own<const kj::Directory> directory, VfsSettings settings) {
  SqliteVfs::Options options{
    .deviceCharacteristics = settings.device_characteristics,
    .sync = settings.sync,
  };
  return kj::heap<SqliteVfs>(*directory, options).attach(kj::mv(directory));
}

kj::Maybe<kj::WriteMode> toWriteMode(OpenMode mode) {
  switch (mode) {
    case OpenMode::ReadOnly:
      return kj::none;
    case OpenMode::ReadWrite:
      return kj::WriteMode::MODIFY;
    case OpenMode::Create:
      return kj::WriteMode::CREATE | kj::WriteMode::MODIFY;
    case OpenMode::CreateWithParents:
      return kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT;
  }
  KJ_FAIL_REQUIRE("unknown open mode", static_cast<uint8_t>(mode));
}

::rust::Vec<uint8_t> toRust(kj::ArrayPtr<const kj::byte> bytes) {
  return bytes_to_vec({bytes.begin(), bytes.size()});
}

// The path of a database, relative to its VFS's directory. On Windows, a component with a `\` or a
// `:` would reach outside of it once the path is made native, so neither is accepted on any
// platform.
kj::Path databasePath(::rust::Str path) {
  auto parsed = kj::Path::parse(kj::str(path));
  for (auto& part: parsed) {
    KJ_REQUIRE(part.findFirst('\\') == kj::none && part.findFirst(':') == kj::none,
        "a database path component cannot contain '\\' or ':'", part);
  }
  return parsed;
}

// The row's value of `column`, for a getter to read.
SqliteDatabase::Query& row(const Query& query, uint32_t column) {
  KJ_REQUIRE(!query.hasFailed(), "the query failed");
  auto& q = query.get();
  KJ_REQUIRE(!q.isDone(), "the query has no more rows");
  KJ_REQUIRE(column < q.columnCount(), "column out of range", column, q.columnCount());
  return q;
}

}  // namespace

Bindings::Bindings(size_t capacity) {
  values.reserve(capacity);
}

void Bindings::addNull() {
  values.add(nullptr);
}

void Bindings::addInt64(int64_t value) {
  values.add(value);
}

void Bindings::addDouble(double value) {
  values.add(value);
}

void Bindings::addText(::rust::Str value) {
  values.add(kj::StringPtr(texts.add(kj::str(value))));
}

void Bindings::addBlob(::rust::Slice<const uint8_t> value) {
  if (value.empty()) {
    values.add(kj::ArrayPtr<const kj::byte>(EMPTY_BLOB).first(0));
  } else {
    values.add(blobs.add(kj::heapArray<kj::byte>(value.data(), value.size())).asPtr().asConst());
  }
}

Database::Database(
    const SqliteVfs& vfs, kj::Path path, kj::Maybe<kj::WriteMode> mode, size_t maxMemoryBytes)
    // SQLite reads a per-process limit of `kj::maxValue` as -1, which leaves its hard heap limit
    // as it is, so the first database to open sets only the soft limit.
    // `set_max_memory_per_process_bytes()` sets the hard one.
    : db(vfs, kj::mv(path), mode, maxMemoryBytes, kj::maxValue) {}

Query::Query(SqliteDatabase& db, kj::StringPtr sql, kj::Own<Bindings> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(db.run(SqliteDatabase::QueryOptions{.regulator = SqliteDatabase::TRUSTED},
          sql,
          bindings->asPtr())) {}

Query::Query(Statement& statement, kj::Own<Bindings> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(statement.run(bindings->asPtr())) {}

VfsSettings vfs_default_settings() {
  SqliteVfs::Options options;
  return {.device_characteristics = options.deviceCharacteristics, .sync = options.sync};
}

kj::Own<SqliteVfs> vfs_new_in_memory(VfsSettings settings) {
  return newVfs(kj::newInMemoryDirectory(kj::systemPreciseCalendarClock()), settings);
}

kj::Own<SqliteVfs> vfs_new_on_disk(::rust::Str path, VfsSettings settings) {
  auto filesystem = kj::newDiskFilesystem();
  auto directoryPath = filesystem->getCurrentPath().evalNative(kj::str(path));
  return newVfs(filesystem->getRoot().openSubdir(directoryPath, kj::WriteMode::MODIFY), settings);
}

int64_t set_max_memory_per_process_bytes(int64_t bytes) {
  return SqliteDatabase::setMaxMemoryPerProcessBytes(bytes);
}

kj::Own<Database> database_open(
    const SqliteVfs& vfs, ::rust::Str path, OpenMode mode, size_t maxMemoryBytes) {
  return kj::heap<Database>(vfs, databasePath(path), toWriteMode(mode), maxMemoryBytes);
}

void database_set_error_context(const Database& db, ::rust::Str context) {
  db.get().setErrorContext(kj::str(context));
}

kj::Own<Query> database_run(const Database& db, ::rust::Str sql, kj::Own<Bindings> bindings) {
  return kj::heap<Query>(db.get(), kj::str(sql), kj::mv(bindings));
}

kj::Own<Statement> database_prepare(const Database& db, ::rust::Str sql) {
  return kj::heap<Statement>(db.get().prepare(SqliteDatabase::TRUSTED, kj::str(sql)));
}

kj::Own<Query> statement_run(Statement& statement, kj::Own<Bindings> bindings) {
  return kj::heap<Query>(statement, kj::mv(bindings));
}

kj::Own<Bindings> bindings_new(size_t capacity) {
  return kj::heap<Bindings>(capacity);
}

void bindings_add_null(Bindings& bindings) {
  bindings.addNull();
}

void bindings_add_i64(Bindings& bindings, int64_t value) {
  bindings.addInt64(value);
}

void bindings_add_f64(Bindings& bindings, double value) {
  bindings.addDouble(value);
}

void bindings_add_text(Bindings& bindings, ::rust::Str value) {
  bindings.addText(value);
}

void bindings_add_blob(Bindings& bindings, ::rust::Slice<const uint8_t> value) {
  bindings.addBlob(value);
}

bool query_is_done(const Query& query) {
  return query.get().isDone();
}

void query_next(Query& query) {
  // Stepping a finished or failed statement would start it over.
  KJ_REQUIRE(!query.hasFailed(), "the query failed");
  auto& q = query.get();
  KJ_REQUIRE(!q.isDone(), "the query has no more rows");
  KJ_ON_SCOPE_FAILURE(query.markFailed());
  q.nextRow();
}

uint64_t query_change_count(const Query& query) {
  KJ_REQUIRE(!query.hasFailed(), "the query failed");
  auto& q = query.get();
  KJ_REQUIRE(q.isDone(), "the query has rows left");
  KJ_REQUIRE(q.columnCount() == 0, "a query that returns rows has no change count");
  return q.ownChangeCount();
}

uint32_t query_column_count(const Query& query) {
  return query.get().columnCount();
}

bool query_is_null(const Query& query, uint32_t column) {
  return row(query, column).isNull(column);
}

int64_t query_i64(const Query& query, uint32_t column) {
  return row(query, column).getInt64(column);
}

double query_f64(const Query& query, uint32_t column) {
  return row(query, column).getDouble(column);
}

::rust::Vec<uint8_t> query_text(const Query& query, uint32_t column) {
  return toRust(row(query, column).getCheckedText(column).asBytes());
}

::rust::Vec<uint8_t> query_blob(const Query& query, uint32_t column) {
  return toRust(row(query, column).getCheckedBlob(column));
}

}  // namespace workerd::rust::sqlite_rs
