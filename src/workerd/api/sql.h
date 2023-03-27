// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/sqlite.h>
#include "basics.h"

namespace workerd::api {

class DurableObjectStorage;

typedef kj::OneOf<kj::Array<const byte>, kj::String, double> ValueBind;
class SqlDatabase;

class SqlResult final: public jsg::Object {
public:
  SqlResult(SqliteDatabase::Query&& query);

  JSG_RESOURCE_TYPE(SqlResult, CompatibilityFlags::Reader flags) {
    JSG_ITERABLE(rows);
  }

  JSG_ITERATOR(RowIterator, rows,
                jsg::Dict<SqliteDatabase::Query::ValueOwned>,
                jsg::Ref<SqlResult>,
                rowIteratorNext);

  static kj::Maybe<jsg::Dict<SqliteDatabase::Query::ValueOwned>> rowIteratorNext(jsg::Lock& js, jsg::Ref<SqlResult>& state);
private:
  SqliteDatabase::Query query;
};

class SqlPreparedStatement final: public jsg::Object {
public:
  SqlPreparedStatement(jsg::Ref<SqlDatabase>&& sqlDb, SqliteDatabase::Statement&& statement);

  struct SqlRunOptions {
    kj::Maybe<kj::Array<ValueBind>> bindValues;
    JSG_STRUCT(bindValues);
  };

  jsg::Ref<SqlResult> run(jsg::Optional<SqlRunOptions> options);

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

class SqlDatabase final: public jsg::Object {
public:
  friend class SqlPreparedStatement;
  SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage);
  ~SqlDatabase();

  struct SqlExecOptions {
    bool admin = false;
    kj::Maybe<kj::Array<ValueBind>> bindValues;
    JSG_STRUCT(admin, bindValues);
  };

  jsg::Ref<SqlResult> exec(jsg::Lock& js, kj::String query, jsg::Optional<SqlExecOptions> options);

  struct SqlPrepareOptions {
    bool admin = false;
    JSG_STRUCT(admin);
  };

  jsg::Ref<SqlPreparedStatement> prepare(jsg::Lock& js, kj::String query, jsg::Optional<SqlPrepareOptions> options);

  JSG_RESOURCE_TYPE(SqlDatabase, CompatibilityFlags::Reader flags) {
    JSG_METHOD(exec);
    JSG_METHOD(prepare);
  }

private:
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(storage);
  }

  static int authorize(
    void* userdata,
    int actionCode,
    const char* param1,
    const char* param2,
    const char* dbName,
    const char* triggerName);

  void onError(const char* errStr);

  SqliteDatabase& sqlite;
  jsg::Ref<DurableObjectStorage> storage;

  bool isAdmin = false;
};

#define EW_SQL_ISOLATE_TYPES                \
  api::SqlDatabase,                         \
  api::SqlPreparedStatement,                \
  api::SqlResult,                           \
  api::SqlResult::RowIterator,              \
  api::SqlResult::RowIterator::Next,        \
  api::SqlPreparedStatement::SqlRunOptions, \
  api::SqlDatabase::SqlExecOptions,         \
  api::SqlDatabase::SqlPrepareOptions
// The list of sql.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
