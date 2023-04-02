// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"

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
    : sqlite(sqlite), storage(kj::mv(storage)) {}

SqlDatabase::~SqlDatabase() {}

jsg::Ref<SqlResult> SqlDatabase::exec(jsg::Lock& js, kj::String querySql, jsg::Optional<SqlExecOptions> options) {
  kj::Vector<SqliteDatabase::Query::ValuePtr> bindValues;

  KJ_IF_MAYBE(o, options) {
    fillBindValues(bindValues, o->bindValues);
  }

  kj::String error;
  kj::ArrayPtr<const SqliteDatabase::Query::ValuePtr> boundValues = bindValues.asPtr();

  SqliteDatabase::Query query = sqlite.run(*this, querySql, boundValues);
  return jsg::alloc<SqlResult>(kj::mv(query));
}

jsg::Ref<SqlPreparedStatement> SqlDatabase::prepare(jsg::Lock& js, kj::String query) {
  return jsg::alloc<SqlPreparedStatement>(JSG_THIS, sqlite.prepare(*this, query));
}

bool SqlDatabase::isAllowedName(kj::StringPtr name) {
  return !name.startsWith("_cf_");
}

bool SqlDatabase::isAllowedTrigger(kj::StringPtr name) {
  return true;
}

void SqlDatabase::onError(kj::StringPtr message) {
  JSG_ASSERT(false, Error, message);
}

}  // namespace workerd::api
