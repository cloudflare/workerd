// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-sqlite.h"
#include <algorithm>
#include <workerd/jsg/jsg.h>
#include "io-gate.h"

namespace workerd {

ActorSqlite::ActorSqlite(kj::Own<SqliteDatabase> dbParam, OutputGate& outputGate,
                         kj::Function<kj::Promise<void>()> commitCallback,
                         Hooks& hooks)
    : db(kj::mv(dbParam)), outputGate(outputGate), commitCallback(kj::mv(commitCallback)),
      hooks(hooks), kv(*db), commitTasks(*this) {
  db->onWrite(KJ_BIND_METHOD(*this, onWrite));
}

ActorSqlite::ImplicitTxn::ImplicitTxn(ActorSqlite& parent)
    : parent(parent) {
  KJ_REQUIRE(parent.currentTxn.is<NoTxn>());
  parent.beginTxn.run();
  parent.currentTxn = this;
}
ActorSqlite::ImplicitTxn::~ImplicitTxn() noexcept(false) {
  KJ_IF_MAYBE(c, parent.currentTxn.tryGet<ImplicitTxn*>()) {
    if (*c == this) {
      parent.currentTxn.init<NoTxn>();
    }
  }
  if (!committed && parent.broken == nullptr) {
    // Failed to commit, so roll back.
    //
    // This should only happen in cases of catastrophic error. Since this is rarely actually
    // executed, we don't prepare a statement for it.
    parent.db->run("ROLLBACK TRANSACTION");
  }
}

void ActorSqlite::ImplicitTxn::commit() {
  // Ignore redundant commit()s.
  if (!committed) {
    parent.commitTxn.run();
    committed = true;
  }
}

void ActorSqlite::onWrite() {
  if (currentTxn.is<NoTxn>()) {
    auto txn = kj::heap<ImplicitTxn>(*this);

    commitTasks.add(outputGate.lockWhile(kj::evalLater(
        [this, txn = kj::mv(txn)]() mutable -> kj::Promise<void> {
      // Don't commit if shutdown() has been called.
      requireNotBroken();

      try {
        txn->commit();
      } catch (...) {
        // HACK: If we became broken during `COMMIT TRANSACTION` then throw the broken exception
        // instead of whatever SQLite threw.
        requireNotBroken();

        // No, we're not broken, so propagate the exception as-is.
        throw;
      }

      // The callback is only expected to commit writes up until this point. Any new writes that
      // occur while the callback is in progress are NOT included, therefore require a new commit
      // to be scheduled. So, we should drop `txn` to cause `currentTxn` to become NoTxn now,
      // rather than after the callback.
      { auto drop = kj::mv(txn); }

      return commitCallback();
    })));
  }
}

void ActorSqlite::taskFailed(kj::Exception&& exception) {
  // The output gate should already have been broken since it wraps all commits tasks. So, we
  // don't have to report anything here, the exception will already propagate elsewhere. We
  // should block further operations, though.
  if (broken == nullptr) {
    broken = kj::mv(exception);
  }
}

void ActorSqlite::requireNotBroken() {
  KJ_IF_MAYBE(e, broken) {
    kj::throwFatalException(kj::cp(*e));
  }
}

// =======================================================================================
// ActorCacheInterface implementation

kj::OneOf<kj::Maybe<ActorCacheOps::Value>,
          kj::Promise<kj::Maybe<ActorCacheOps::Value>>>
    ActorSqlite::get(Key key, ReadOptions options) {
  requireNotBroken();

  kj::Maybe<ActorCacheOps::Value> result;
  kv.get(key, [&](ValuePtr value) {
    result = kj::heapArray(value);
  });
  return result;
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::get(kj::Array<Key> keys, ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  for (auto& key: keys) {
    kv.get(key, [&](ValuePtr value) {
      results.add(KeyValuePair { kj::mv(key), kj::heapArray(value) });
    });
  }
  std::sort(results.begin(), results.end(),
      [](auto& a, auto& b) { return a.key < b.key; });
  return GetResultList(kj::mv(results));
}

kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorSqlite::getAlarm(
    ReadOptions options) {
  requireNotBroken();

  return hooks.getAlarm();
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::list(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::FORWARD, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair { kj::str(key), kj::heapArray(value) });
  });

  // Already guaranteed sorted.
  return GetResultList(kj::mv(results));
}

kj::OneOf<ActorCacheOps::GetResultList, kj::Promise<ActorCacheOps::GetResultList>>
    ActorSqlite::listReverse(Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit,
                               ReadOptions options) {
  requireNotBroken();

  kj::Vector<KeyValuePair> results;
  kv.list(begin, end, limit, SqliteKv::REVERSE, [&](KeyPtr key, ValuePtr value) {
    results.add(KeyValuePair { kj::str(key), kj::heapArray(value) });
  });

  // Already guaranteed sorted (reversed).
  return GetResultList(kj::mv(results));
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(Key key, Value value, WriteOptions options) {
  requireNotBroken();

  kv.put(key, value);
  return nullptr;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options) {
  requireNotBroken();

  for (auto& pair: pairs) {
    kv.put(pair.key, pair.value);
  }
  return nullptr;
}

kj::OneOf<bool, kj::Promise<bool>> ActorSqlite::delete_(Key key, WriteOptions options) {
  requireNotBroken();

  return kv.delete_(key);
}

kj::OneOf<uint, kj::Promise<uint>> ActorSqlite::delete_(
    kj::Array<Key> keys, WriteOptions options) {
  requireNotBroken();

  uint count = 0;
  for (auto& key: keys) {
    count += kv.delete_(key);
  }
  return count;
}

kj::Maybe<kj::Promise<void>> ActorSqlite::setAlarm(
    kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) {
  requireNotBroken();

  return hooks.setAlarm(newAlarmTime);
}

kj::Own<ActorCacheInterface::Transaction> ActorSqlite::startTransaction() {
  requireNotBroken();

  // TODO(sqlite): Implement transactions.
  JSG_FAIL_REQUIRE(Error, "transaction() not yet implemented for SQLite-backed storage");
}

ActorCacheInterface::DeleteAllResults ActorSqlite::deleteAll(WriteOptions options) {
  requireNotBroken();

  uint count = kv.deleteAll();
  return {
    .backpressure = nullptr,
    .count = count,
  };
}

kj::Maybe<kj::Promise<void>> ActorSqlite::evictStale(kj::Date now) {
  // This implementation never needs to apply backpressure.
  return nullptr;
}

void ActorSqlite::shutdown(kj::Maybe<const kj::Exception&> maybeException) {
  // TODO(cleanup): Logic copied from ActorCache::shutdown(). Should they share somehow?

  if (broken == nullptr) {
    auto exception = [&]() {
      KJ_IF_MAYBE(e, maybeException) {
        // We were given an exception, use it.
        return kj::cp(*e);
      }

      // Use the direct constructor so that we can reuse the constexpr message variable for testing.
      auto exception = kj::Exception(
          kj::Exception::Type::OVERLOADED, __FILE__, __LINE__,
          kj::heapString(ActorCache::SHUTDOWN_ERROR_MESSAGE));

      // Add trace info sufficient to tell us which operation caused the failure.
      exception.addTraceHere();
      exception.addTrace(__builtin_return_address(0));
      return exception;
    }();

    // Any scheduled flushes will fail once `flushImpl()` is invoked and notices that
    // `maybeTerminalException` has a value. Any in-flight flushes will continue to run in the
    // background. Remember that these in-flight flushes may or may not be awaited by the worker,
    // but they still hold the output lock as long as `allowUnconfirmed` wasn't used.
    broken.emplace(kj::mv(exception));

    // We explicitly do not schedule a flush to break the output gate. This means that if a request
    // is ongoing after the actor cache is shutting down, the output gate is only broken if they
    // had to send a flush after shutdown, either from a scheduled flush or a retry after failure.
  } else {
    // We've already experienced a terminal exception either from shutdown or oom, there should
    // already be a flush scheduled that will break the output gate.
  }
}

kj::Maybe<kj::Own<void>> ActorSqlite::armAlarmHandler(kj::Date scheduledTime, bool noCache) {
  return hooks.armAlarmHandler(scheduledTime, noCache);
}

void ActorSqlite::cancelDeferredAlarmDeletion() {
  hooks.cancelDeferredAlarmDeletion();
}

kj::Maybe<kj::Promise<void>> ActorSqlite::onNoPendingFlush() {
  // This implements sync().
  //
  // TODO(sqlite): When we implement `allowUnconfirmed`, this implementation becomes incorrect
  //   because sync() should wait on all writes, even ones with that flag, whereas the output
  //   gate is not blocked by `allowUnconfirmed` writes. At present we haven't actually
  //   implemented `allowUnconfirmed` yet.
  return outputGate.wait();
}

ActorSqlite::Hooks ActorSqlite::Hooks::DEFAULT = ActorSqlite::Hooks{};

kj::Maybe<kj::Own<void>> ActorSqlite::Hooks::armAlarmHandler(kj::Date scheduledTime, bool noCache) {
  JSG_FAIL_REQUIRE(Error, "alarms are not yet implemented for SQLite-backed Durable Objects");
}

void ActorSqlite::Hooks::cancelDeferredAlarmDeletion() {
  JSG_FAIL_REQUIRE(Error, "alarms are not yet implemented for SQLite-backed Durable Objects");
}

kj::Promise<kj::Maybe<kj::Date>> ActorSqlite::Hooks::getAlarm() {
  JSG_FAIL_REQUIRE(Error, "getAlarm() is not yet implemented for SQLite-backed Durable Objects");
}

kj::Promise<void> ActorSqlite::Hooks::setAlarm(kj::Maybe<kj::Date>) {
  JSG_FAIL_REQUIRE(Error, "setAlarm() is not yet implemented for SQLite-backed Durable Objects");
}

}  // namespace workerd
