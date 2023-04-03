// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/sqlite.h>
#include "basics.h"

namespace workerd::api {

class DurableObjectStorage;

typedef kj::OneOf<kj::Array<const byte>, kj::String, double> SqlBindingValue;
class SqlDatabase;

class SqlResult final: public jsg::Object {
public:
  SqlResult(SqliteDatabase::Statement& statement, kj::Array<SqlBindingValue> bindings);
  SqlResult(SqliteDatabase& db, SqliteDatabase::Regulator& regulator,
            kj::StringPtr sqlCode, kj::Array<SqlBindingValue> bindings);

  JSG_RESOURCE_TYPE(SqlResult, CompatibilityFlags::Reader flags) {
    JSG_ITERABLE(rows);
  }

  using Value = kj::Maybe<kj::OneOf<kj::Array<byte>, kj::StringPtr, double>>;
  // One value returned from SQL. Note that we intentionally return StringPtr instead of String
  // because we know that the underlying buffer returned by SQLite will be valid long enough to be
  // converted by JSG into a V8 string. For byte arrays, on the other hand, we pass ownership to
  // JSG, which does not need to make a copy.

  JSG_ITERATOR(RowIterator, rows, jsg::Dict<Value>, jsg::Ref<SqlResult>, rowIteratorNext);
  static kj::Maybe<jsg::Dict<Value>> rowIteratorNext(jsg::Lock& js, jsg::Ref<SqlResult>& state);
private:
  SqliteDatabase::Query query;

  kj::Array<SqlBindingValue> bindings;
  // The bindings that were used to construct `query`. We have to keep these alive until the query
  // is done since it might contain pointers into strings and blobs.

  bool isFirst = true;

  static kj::Array<const SqliteDatabase::Query::ValuePtr> mapBindings(
      kj::ArrayPtr<SqlBindingValue> values);
};

class SqlPreparedStatement final: public jsg::Object {
public:
  SqlPreparedStatement(jsg::Ref<SqlDatabase>&& sqlDb, SqliteDatabase::Statement&& statement);

  jsg::Ref<SqlResult> run(jsg::Arguments<SqlBindingValue> bindings);

  JSG_RESOURCE_TYPE(SqlPreparedStatement, CompatibilityFlags::Reader flags) {
    JSG_CALLABLE(run);
  }

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(sqlDatabase);
  }

  jsg::Ref<SqlDatabase> sqlDatabase;
  SqliteDatabase::Statement statement;
};

class SqlDatabase final: public jsg::Object, private SqliteDatabase::Regulator {
public:
  SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage);
  ~SqlDatabase();

  jsg::Ref<SqlResult> exec(jsg::Lock& js, kj::String query, jsg::Arguments<SqlBindingValue> bindings);

  jsg::Ref<SqlPreparedStatement> prepare(jsg::Lock& js, kj::String query);

  JSG_RESOURCE_TYPE(SqlDatabase, CompatibilityFlags::Reader flags) {
    JSG_METHOD(exec);
    JSG_METHOD(prepare);
  }

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(storage);
  }

  bool isAllowedName(kj::StringPtr name) override;
  bool isAllowedTrigger(kj::StringPtr name) override;
  void onError(kj::StringPtr message) override;

  SqliteDatabase& sqlite;
  jsg::Ref<DurableObjectStorage> storage;

  friend class SqlPreparedStatement;
};

#define EW_SQL_ISOLATE_TYPES                \
  api::SqlDatabase,                         \
  api::SqlPreparedStatement,                \
  api::SqlResult,                           \
  api::SqlResult::RowIterator,              \
  api::SqlResult::RowIterator::Next
// The list of sql.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
