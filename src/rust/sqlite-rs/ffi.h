// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of the `sqlite-rs` crate's bridge (`ffi.rs`): thin wrappers over `SqliteDatabase`
// (`workerd/util/sqlite.h`). Every SQLite call is made by `SqliteDatabase` and its `Statement` and
// `Query`, which keep the invariants around it (the memory scope, the VFS error listener); the
// shim only adapts types.
//
// The bridge is visible to all, but only so that other bridges can name `SqliteVfs`. `Database`,
// `Query`, `Bindings` and the functions below are the crate's internals, not an API to use
// directly.
//
// Rust holds `Database` and `Query` through shared references, and SQLite's state changes behind
// them, so their wrapped objects are `mutable`. Rust never reads them except through these
// functions, and cxx's opaque types are neither `Sync` nor read-only, so a shared reference is
// only ever used on one thread at a time.

#include <workerd/util/sqlite.h>

#include <rust/cxx.h>

#include <kj/common.h>
#include <kj/filesystem.h>
#include <kj/memory.h>
#include <kj/string.h>
#include <kj/vector.h>

namespace workerd::rust::sqlite_rs {

// Defined by the generated bridge header, which includes this one.
enum class OpenMode : uint8_t;

using SqliteVfs = SqliteDatabase::Vfs;
using Statement = SqliteDatabase::Statement;

// The parameters of one query. Text and blobs are copied: SQLite reads them until the query is
// destroyed, and `kj::StringPtr` needs the NUL terminator that a Rust string lacks.
class Bindings {
 public:
  explicit Bindings(size_t capacity);
  KJ_DISALLOW_COPY_AND_MOVE(Bindings);

  void addNull();
  void addInt64(int64_t value);
  void addDouble(double value);
  void addText(::rust::Str value);
  void addBlob(::rust::Slice<const uint8_t> value);

  kj::ArrayPtr<const SqliteDatabase::Query::ValuePtr> asPtr() const {
    return values.asPtr();
  }

 private:
  // Declared before `values`, which points into them.
  kj::Vector<kj::String> texts;
  kj::Vector<kj::Array<kj::byte>> blobs;
  kj::Vector<SqliteDatabase::Query::ValuePtr> values;
};

class Database {
 public:
  Database(const SqliteVfs& vfs, kj::Path path, kj::Maybe<kj::WriteMode> mode);
  KJ_DISALLOW_COPY_AND_MOVE(Database);

  SqliteDatabase& get() const {
    return db;
  }

 private:
  mutable SqliteDatabase db;
};

// A running query and the parameters it reads.
class Query {
 public:
  Query(SqliteDatabase& db, kj::StringPtr sql, kj::Own<Bindings> bindings);
  Query(Statement& statement, kj::Own<Bindings> bindings);
  KJ_DISALLOW_COPY_AND_MOVE(Query);

  SqliteDatabase::Query& get() const {
    return query;
  }

  // Whether a step threw. SQLite leaves the statement where it failed, and stepping it again
  // would start it over, so the query is finished without being done.
  bool hasFailed() const {
    return failed;
  }
  void markFailed() {
    failed = true;
  }

 private:
  // Declared before `query`, which reads it.
  kj::Own<Bindings> bindings;
  mutable SqliteDatabase::Query query;
  bool failed = false;
};

// VFS
kj::Own<SqliteVfs> vfs_new_in_memory();
kj::Own<SqliteVfs> vfs_new_on_disk(::rust::Str path);

// Database
kj::Own<Database> database_open(const SqliteVfs& vfs, ::rust::Str path, OpenMode mode);
kj::Own<Query> database_run(const Database& db, ::rust::Str sql, kj::Own<Bindings> bindings);
kj::Own<Statement> database_prepare(const Database& db, ::rust::Str sql);

// Statement
kj::Own<Query> statement_run(Statement& statement, kj::Own<Bindings> bindings);

// Bindings
kj::Own<Bindings> bindings_new(size_t capacity);
void bindings_add_null(Bindings& bindings);
void bindings_add_i64(Bindings& bindings, int64_t value);
void bindings_add_f64(Bindings& bindings, double value);
void bindings_add_text(Bindings& bindings, ::rust::Str value);
void bindings_add_blob(Bindings& bindings, ::rust::Slice<const uint8_t> value);

// Query
bool query_is_done(const Query& query);
void query_next(Query& query);
uint64_t query_change_count(const Query& query);
uint32_t query_column_count(const Query& query);
bool query_is_null(const Query& query, uint32_t column);
int64_t query_i64(const Query& query, uint32_t column);
double query_f64(const Query& query, uint32_t column);
// Copies of the value, which SQLite keeps only until the next getter of the column or the next step.
::rust::Vec<uint8_t> query_text(const Query& query, uint32_t column);
::rust::Vec<uint8_t> query_blob(const Query& query, uint32_t column);

}  // namespace workerd::rust::sqlite_rs
