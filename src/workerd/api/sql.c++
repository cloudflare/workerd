// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"
#include "actor-state.h"

namespace workerd::api {

SqlResult::SqlResult::State::State(
    kj::RefcountedWrapper<SqliteDatabase::Statement>& statement, kj::Array<SqlBindingValue> bindingsParam)
    : dependency(statement.addWrappedRef()),
      bindings(kj::mv(bindingsParam)),
      query(statement.getWrapped().run(mapBindings(bindings).asPtr())) {}

SqlResult::SqlResult::State::State(
    SqliteDatabase& db, SqliteDatabase::Regulator& regulator,
    kj::StringPtr sqlCode, kj::Array<SqlBindingValue> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(db.run(regulator, sqlCode, mapBindings(bindings).asPtr())) {}

jsg::Ref<SqlResult::RowIterator> SqlResult::rows(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<RowIterator>(JSG_THIS);
}

kj::Maybe<jsg::Dict<SqlResult::Value>> SqlResult::rowIteratorNext(
    jsg::Lock& js, jsg::Ref<SqlResult>& obj) {
  auto& state = *obj->state;

  if (state.isFirst) {
    // Little hack: We don't want to call query.nextRow() at the end of this method because it
    // may invalidate the backing buffers of StringPtrs that we haven't returned to JS yet.
    state.isFirst = false;
  } else {
    state.query.nextRow();
  }

  auto& query = state.query;
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

SqlPreparedStatement::SqlPreparedStatement(SqliteDatabase::Statement&& statement)
    : statement(IoContext::current().addObject(
        kj::refcountedWrapper<SqliteDatabase::Statement>(kj::mv(statement)))) {}

kj::Array<const SqliteDatabase::Query::ValuePtr> SqlResult::mapBindings(
    kj::ArrayPtr<SqlBindingValue> values) {
  return KJ_MAP(value, values) -> SqliteDatabase::Query::ValuePtr {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(data, kj::Array<const byte>) {
        return data.asPtr();
      }
      KJ_CASE_ONEOF(text, kj::String) {
        return text.asPtr();
      }
      KJ_CASE_ONEOF(d, double) {
        return d;
      }
    }
    KJ_UNREACHABLE;
  };
}

jsg::Ref<SqlResult> SqlPreparedStatement::run(jsg::Arguments<SqlBindingValue> bindings) {
  return jsg::alloc<SqlResult>(*statement, kj::mv(bindings));
}

SqlDatabase::SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage)
    : sqlite(IoContext::current().addObject(sqlite)), storage(kj::mv(storage)) {}

SqlDatabase::~SqlDatabase() {}

jsg::Ref<SqlResult> SqlDatabase::exec(jsg::Lock& js, kj::String querySql,
                                      jsg::Arguments<SqlBindingValue> bindings) {
  SqliteDatabase::Regulator& regulator = *this;
  return jsg::alloc<SqlResult>(*sqlite, regulator, querySql, kj::mv(bindings));
}

jsg::Ref<SqlPreparedStatement> SqlDatabase::prepare(jsg::Lock& js, kj::String query) {
  return jsg::alloc<SqlPreparedStatement>(sqlite->prepare(*this, query));
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
