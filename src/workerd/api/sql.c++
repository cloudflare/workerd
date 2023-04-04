// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sql.h"
#include "actor-state.h"

namespace workerd::api {

SqlDatabase::SqlDatabase(SqliteDatabase& sqlite, jsg::Ref<DurableObjectStorage> storage)
    : sqlite(IoContext::current().addObject(sqlite)), storage(kj::mv(storage)) {}

SqlDatabase::~SqlDatabase() {}

jsg::Ref<SqlDatabase::Cursor> SqlDatabase::exec(jsg::Lock& js, kj::String querySql,
                                                jsg::Arguments<BindingValue> bindings) {
  SqliteDatabase::Regulator& regulator = *this;
  return jsg::alloc<Cursor>(*sqlite, regulator, querySql, kj::mv(bindings));
}

jsg::Ref<SqlDatabase::Statement> SqlDatabase::prepare(jsg::Lock& js, kj::String query) {
  return jsg::alloc<Statement>(sqlite->prepare(*this, query));
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

SqlDatabase::Cursor::State::State(
    kj::RefcountedWrapper<SqliteDatabase::Statement>& statement, kj::Array<BindingValue> bindingsParam)
    : dependency(statement.addWrappedRef()),
      bindings(kj::mv(bindingsParam)),
      query(statement.getWrapped().run(mapBindings(bindings).asPtr())) {}

SqlDatabase::Cursor::State::State(
    SqliteDatabase& db, SqliteDatabase::Regulator& regulator,
    kj::StringPtr sqlCode, kj::Array<BindingValue> bindingsParam)
    : bindings(kj::mv(bindingsParam)),
      query(db.run(regulator, sqlCode, mapBindings(bindings).asPtr())) {}

SqlDatabase::Cursor::~Cursor() noexcept(false) {
  // If this Cursor was created from a Statement, clear the Statement's currentCursor weak ref.
  KJ_IF_MAYBE(s, selfRef) {
    KJ_IF_MAYBE(p, *s) {
      if (p == this) {
        *s = nullptr;
      }
    }
  }
}

void SqlDatabase::Cursor::CachedColumnNames::ensureInitialized(
    jsg::Lock& js, SqliteDatabase::Query& source) {
  if (names == nullptr) {
    v8::HandleScope scope(js.v8Isolate);
    auto builder = kj::heapArrayBuilder<jsg::V8Ref<v8::String>>(source.columnCount());
    for (auto i: kj::zeroTo(builder.capacity())) {
      builder.add(js.v8Isolate, jsg::v8StrIntern(js.v8Isolate, source.getColumnName(i)));
    }
    names = builder.finish();
  }
}

jsg::Ref<SqlDatabase::Cursor::RowIterator> SqlDatabase::Cursor::rows(
    jsg::Lock& js,
    CompatibilityFlags::Reader featureFlags) {
  KJ_IF_MAYBE(s, state) {
    cachedColumnNames.ensureInitialized(js, (*s)->query);
  }
  return jsg::alloc<RowIterator>(JSG_THIS);
}

kj::Maybe<SqlDatabase::Cursor::RowDict> SqlDatabase::Cursor::rowIteratorNext(
    jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  auto names = obj->cachedColumnNames.get();
  return iteratorImpl(js, obj,
      [&](State& state, uint i, Value&& value) {
    return RowDict::Field{
      // A little trick here: We know there are no HandleScopes on the stack between JSG and here,
      // so we can return a dict keyed by local handles, which avoids constructing new V8Refs here
      // which would be relatively slower.
      .name = names[i].getHandle(js),
      .value = kj::mv(value)
    };
  }).map([&](kj::Array<RowDict::Field>&& fields) {
    return RowDict { .fields = kj::mv(fields) };
  });
}

jsg::Ref<SqlDatabase::Cursor::RawIterator> SqlDatabase::Cursor::raw(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<RawIterator>(JSG_THIS);
}

kj::Maybe<kj::Array<SqlDatabase::Cursor::Value>> SqlDatabase::Cursor::rawIteratorNext(
    jsg::Lock& js, jsg::Ref<Cursor>& obj) {
  return iteratorImpl(js, obj,
      [&](State& state, uint i, Value&& value) {
    return kj::mv(value);
  });
}

template <typename Func>
auto SqlDatabase::Cursor::iteratorImpl(jsg::Lock& js, jsg::Ref<Cursor>& obj, Func&& func)
    -> kj::Maybe<kj::Array<
        decltype(func(kj::instance<State&>(), uint(), kj::instance<Value&&>()))>> {
  using Element = decltype(func(kj::instance<State&>(), uint(), kj::instance<Value&&>()));

  auto& state = *KJ_UNWRAP_OR(obj->state, {
    if (obj->canceled) {
      JSG_FAIL_REQUIRE(Error,
          "SQL cursor was closed because the same statement was executed again. If you need to "
          "run multiple copies of the same statement concurrently, you must create multiple "
          "prepared statement objects.");
    } else {
      // Query already done.
      return nullptr;
    }
  });

  if (state.isFirst) {
    // Little hack: We don't want to call query.nextRow() at the end of this method because it
    // may invalidate the backing buffers of StringPtrs that we haven't returned to JS yet.
    state.isFirst = false;
  } else {
    state.query.nextRow();
  }

  auto& query = state.query;
  if (query.isDone()) {
    // Clean up the query proactively.
    obj->state = nullptr;
    return nullptr;
  }

  auto results = kj::heapArrayBuilder<Element>(query.columnCount());
  for (auto i: kj::zeroTo(results.capacity())) {
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
    results.add(func(state, i, kj::mv(value)));
  }
  return results.finish();
}

SqlDatabase::Statement::Statement(SqliteDatabase::Statement&& statement)
    : statement(IoContext::current().addObject(
        kj::refcountedWrapper<SqliteDatabase::Statement>(kj::mv(statement)))) {}

kj::Array<const SqliteDatabase::Query::ValuePtr> SqlDatabase::Cursor::mapBindings(
    kj::ArrayPtr<BindingValue> values) {
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

jsg::Ref<SqlDatabase::Cursor> SqlDatabase::Statement::run(jsg::Arguments<BindingValue> bindings) {
  auto& statementRef = *statement;  // validate we're in the right IoContext

  KJ_IF_MAYBE(c, currentCursor) {
    // Invalidate previous cursor if it's still running. We have to do this because SQLite only
    // allows one execution of a statement at a time.
    //
    // If this is a problem, we could consider a scheme where we dynamically instantiate copies of
    // the statement as needed. However, that risks wasting memory if the app commonly leaves
    // cursors open and the GC doesn't run proactively enough.
    KJ_IF_MAYBE(s, c->state) {
      c->canceled = !(*s)->query.isDone();
      c->state = nullptr;
    }
    c->selfRef = nullptr;
    currentCursor = nullptr;
  }

  auto result = jsg::alloc<Cursor>(cachedColumnNames, statementRef, kj::mv(bindings));

  result->selfRef = currentCursor;
  currentCursor = *result;

  return result;
}

}  // namespace workerd::api
