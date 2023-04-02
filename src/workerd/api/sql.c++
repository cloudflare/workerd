// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"
#include <sqlite3.h>

namespace workerd::api {

SqlResult::SqlResult(SqliteDatabase::Query&& queryMoved): query(kj::mv(queryMoved)) {}

jsg::Ref<SqlResult::RowIterator> SqlResult::rows(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<RowIterator>(JSG_THIS);
}

kj::Maybe<jsg::Dict<SqlResult::Value>> SqlResult::rowIteratorNext(
    jsg::Lock& js, jsg::Ref<SqlResult>& state) {
  if (state->isFirst) {
    // Little hack: We don't want to call query.nextRow() at the end of this method because it
    // may invalidate the backing buffers of StringPtrs that we haven't returned to JS yet.
    state->isFirst = false;
  } else {
    state->query.nextRow();
  }

  auto& query = state->query;
  if (query.isDone()) {
    return nullptr;
  }

  kj::Vector<jsg::Dict<Value>::Field> fields;
  for (auto i: kj::zeroTo(query.columnCount())) {
    Value value;
    KJ_SWITCH_ONEOF(query.getValue(i)) {
      KJ_CASE_ONEOF(data, kj::ArrayPtr<const byte>) {
        value.emplace(kj::heapArray(data));
      }
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        value.emplace(text);
      }
      KJ_CASE_ONEOF(i, int64_t) {
        // int64 will become BigInt, but most applications won't want all their integers to be
        // BigInt. We will coerce to a double here.
        // TODO(someday): Allow applications to request that certain columns use BigInt.
        value.emplace(static_cast<double>(i));
      }
      KJ_CASE_ONEOF(d, double) {
        value.emplace(d);
      }
      KJ_CASE_ONEOF(_, decltype(nullptr)) {
        // leave value null
      }
    }
    fields.add(jsg::Dict<Value>::Field{
      .name = kj::heapString(query.getColumnName(i)),
      .value = kj::mv(value)
    });
  }
  return jsg::Dict<Value>{.fields = fields.releaseAsArray()};
}

SqlPreparedStatement::SqlPreparedStatement(jsg::Ref<SqlDatabase>&& sqlDb, SqliteDatabase::Statement&& statement):
  sqlDatabase(kj::mv(sqlDb)),
  statement(kj::mv(statement)) {
}

void fillBindValues(
  kj::Vector<SqliteDatabase::Query::ValuePtr>& bindValues, kj::Maybe<kj::Array<ValueBind>>& bindValuesOptional) {
  KJ_IF_MAYBE(values, bindValuesOptional) {
    for (auto& value : *values) {
      KJ_SWITCH_ONEOF(value) {
        KJ_CASE_ONEOF(val, kj::Array<const byte>) {
          bindValues.add(val.asPtr());
        }
        KJ_CASE_ONEOF(val, kj::String) {
          bindValues.add(val.asPtr());
        }
        KJ_CASE_ONEOF(val, double) {
          bindValues.add(val);
        }
      }
    }
  }
}

jsg::Ref<SqlResult> SqlPreparedStatement::run(jsg::Optional<SqlRunOptions> options) {
  kj::Vector<SqliteDatabase::Query::ValuePtr> bindValues;

  KJ_IF_MAYBE(o, options) {
    fillBindValues(bindValues, o->bindValues);
  }

  kj::ArrayPtr<const SqliteDatabase::Query::ValuePtr> boundValues = bindValues.asPtr();
  SqliteDatabase::Query query(sqlDatabase->sqlite, *sqlDatabase, statement, boundValues);
  return jsg::alloc<SqlResult>(kj::mv(query));
}

SqlDatabase::SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage)
    : sqlite(sqlite), storage(kj::mv(storage)) {
  sqlite3* db = sqlite;
  sqlite3_set_authorizer(db, authorize, this);
}

SqlDatabase::~SqlDatabase() {
  sqlite3* db = sqlite;
  sqlite3_set_authorizer(db, nullptr, nullptr);
}

const char* getAuthorizorTableName(int actionCode, const char* param1, const char* param2) {
  switch (actionCode) {
    case SQLITE_CREATE_INDEX:        return param2;
    case SQLITE_CREATE_TABLE:        return param1;
    case SQLITE_CREATE_TEMP_INDEX:   return param2;
    case SQLITE_CREATE_TEMP_TABLE:   return param1;
    case SQLITE_CREATE_TEMP_TRIGGER: return param2;
    case SQLITE_CREATE_TRIGGER:      return param2;
    case SQLITE_DELETE:              return param1;
    case SQLITE_DROP_INDEX:          return param2;
    case SQLITE_DROP_TABLE:          return param1;
    case SQLITE_DROP_TEMP_INDEX:     return param2;
    case SQLITE_DROP_TEMP_TABLE:     return param1;
    case SQLITE_DROP_TEMP_TRIGGER:   return param2;
    case SQLITE_DROP_TRIGGER:        return param2;
    case SQLITE_INSERT:              return param1;
    case SQLITE_READ:                return param1;
    case SQLITE_UPDATE:              return param1;
    case SQLITE_ALTER_TABLE:         return param2;
    case SQLITE_ANALYZE:             return param1;
    case SQLITE_CREATE_VTABLE:       return param1;
    case SQLITE_DROP_VTABLE:         return param1;
  }
  return "";
}

int SqlDatabase::authorize(
  void* userdata,
  int actionCode,
  const char* param1,
  const char* param2,
  const char* dbName,
  const char* triggerName) {
  SqlDatabase* self = reinterpret_cast<SqlDatabase*>(userdata);

  if (self->isAdmin) {
    return SQLITE_OK;
  }

  kj::StringPtr tableName = getAuthorizorTableName(actionCode, param1, param2);
  if (tableName.startsWith("_cf_")) {
    return SQLITE_DENY;
  }

  if (actionCode == SQLITE_PRAGMA) {
    return SQLITE_DENY;
  }

  if (actionCode == SQLITE_TRANSACTION) {
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

jsg::Ref<SqlResult> SqlDatabase::exec(jsg::Lock& js, kj::String querySql, jsg::Optional<SqlExecOptions> options) {
  kj::Vector<SqliteDatabase::Query::ValuePtr> bindValues;

  KJ_IF_MAYBE(o, options) {
    fillBindValues(bindValues, o->bindValues);
    isAdmin = o->admin;
  }

  kj::String error;
  kj::ArrayPtr<const SqliteDatabase::Query::ValuePtr> boundValues = bindValues.asPtr();

  SqliteDatabase::Query query = sqlite.run(*this, querySql, boundValues);
  isAdmin = false;
  return jsg::alloc<SqlResult>(kj::mv(query));
}

jsg::Ref<SqlPreparedStatement> SqlDatabase::prepare(jsg::Lock& js, kj::String query, jsg::Optional<SqlPrepareOptions> options) {
  KJ_IF_MAYBE(o, options) {
    isAdmin = o->admin;
  }

  SqliteDatabase::Statement statement = sqlite.prepare(*this, query);
  isAdmin = false;
  return jsg::alloc<SqlPreparedStatement>(JSG_THIS, kj::mv(statement));
}

void SqlDatabase::onError(kj::StringPtr message) {
  JSG_ASSERT(false, Error, message);
}

}  // namespace workerd::api
