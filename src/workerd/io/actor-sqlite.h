// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-cache.h"
#include <workerd/util/sqlite-kv.h>

namespace workerd {

// An implementation of ActorCacheOps that is backed by SqliteKv.
class ActorSqlite final: public ActorCacheInterface, private kj::TaskSet::ErrorHandler {
  // TODO(perf): This interface is not designed ideally for wrapping SqliteKv. In particular, we
  //   end up allocating extra copies of all the results. It would be nicer if we could actually
  //   parse the V8-serialized values directly from the blob pointers that SQLite spits out.
  //   However, that probably requires rewriting `DurableObjectStorageOperations`. For now, hooking
  //   here is easier and not too costly.

public:
  // Hooks to configure ActorSqlite behavior, right now only used to allow plugging in a backend
  // for alarm operations.
  class Hooks {
  public:
    virtual kj::Promise<kj::Maybe<kj::Date>> getAlarm();
    virtual kj::Promise<void> setAlarm(kj::Maybe<kj::Date> newAlarmTime);
    virtual kj::Maybe<kj::Own<void>> armAlarmHandler(kj::Date scheduledTime, bool noCache);
    virtual void cancelDeferredAlarmDeletion();

    static const Hooks DEFAULT;
  };

  // Constructs ActorSqlite, arranging to honor the output gate, that is, any writes to the
  // database which occur without any `await`s in between will automatically be combined into a
  // single atomic write. This is accomplished using transactions. In addition to ensuring
  // atomicity, this tends to improve performance, as SQLite is able to coalesce writes across
  // statements that modify the same page.
  //
  // `commitCallback` will be invoked after committing a transaction. The output gate will block on
  // the returned promise. This can be used e.g. when the database needs to be replicated to other
  // machines before being considered durable.
  explicit ActorSqlite(kj::Own<SqliteDatabase> dbParam,
      OutputGate& outputGate,
      kj::Function<kj::Promise<void>()> commitCallback,
      Hooks& hooks = const_cast<Hooks&>(Hooks::DEFAULT));

  bool isCommitScheduled() {
    return !currentTxn.is<NoTxn>();
  }

  kj::Maybe<SqliteDatabase&> getSqliteDatabase() override {
    return *db;
  }

  kj::OneOf<kj::Maybe<Value>, kj::Promise<kj::Maybe<Value>>> get(
      Key key, ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> get(
      kj::Array<Key> keys, ReadOptions options) override;
  kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> getAlarm(
      ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> list(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> listReverse(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
  kj::Maybe<kj::Promise<void>> put(Key key, Value value, WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> put(kj::Array<KeyValuePair> pairs, WriteOptions options) override;
  kj::OneOf<bool, kj::Promise<bool>> delete_(Key key, WriteOptions options) override;
  kj::OneOf<uint, kj::Promise<uint>> delete_(kj::Array<Key> keys, WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> setAlarm(
      kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) override;
  // See ActorCacheOps.

  kj::Own<ActorCacheInterface::Transaction> startTransaction() override;
  DeleteAllResults deleteAll(WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> evictStale(kj::Date now) override;
  void shutdown(kj::Maybe<const kj::Exception&> maybeException) override;
  kj::Maybe<kj::Own<void>> armAlarmHandler(kj::Date scheduledTime, bool noCache = false) override;
  void cancelDeferredAlarmDeletion() override;
  kj::Maybe<kj::Promise<void>> onNoPendingFlush() override;
  // See ActorCacheInterface

private:
  kj::Own<SqliteDatabase> db;
  OutputGate& outputGate;
  kj::Function<kj::Promise<void>()> commitCallback;
  Hooks& hooks;
  SqliteKv kv;

  SqliteDatabase::Statement beginTxn = db->prepare("BEGIN TRANSACTION");
  SqliteDatabase::Statement commitTxn = db->prepare("COMMIT TRANSACTION");

  kj::Maybe<kj::Exception> broken;

  struct NoTxn {};

  class ImplicitTxn {
  public:
    explicit ImplicitTxn(ActorSqlite& parent);
    ~ImplicitTxn() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ImplicitTxn);

    void commit();

  private:
    ActorSqlite& parent;

    bool committed = false;
  };

  class ExplicitTxn: public ActorCacheInterface::Transaction, public kj::Refcounted {
  public:
    ExplicitTxn(ActorSqlite& actorSqlite);
    ~ExplicitTxn() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ExplicitTxn);

    kj::Maybe<kj::Promise<void>> commit() override;
    kj::Promise<void> rollback() override;
    // Implements ActorCacheInterface::Transaction.

    kj::OneOf<kj::Maybe<Value>, kj::Promise<kj::Maybe<Value>>> get(
        Key key, ReadOptions options) override;
    kj::OneOf<GetResultList, kj::Promise<GetResultList>> get(
        kj::Array<Key> keys, ReadOptions options) override;
    kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> getAlarm(
        ReadOptions options) override;
    kj::OneOf<GetResultList, kj::Promise<GetResultList>> list(
        Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
    kj::OneOf<GetResultList, kj::Promise<GetResultList>> listReverse(
        Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) override;
    kj::Maybe<kj::Promise<void>> put(Key key, Value value, WriteOptions options) override;
    kj::Maybe<kj::Promise<void>> put(kj::Array<KeyValuePair> pairs, WriteOptions options) override;
    kj::OneOf<bool, kj::Promise<bool>> delete_(Key key, WriteOptions options) override;
    kj::OneOf<uint, kj::Promise<uint>> delete_(kj::Array<Key> keys, WriteOptions options) override;
    kj::Maybe<kj::Promise<void>> setAlarm(
        kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) override;
    // Implements ActorCacheOps. These will all forward to the ActorSqlite instance.

  private:
    ActorSqlite& actorSqlite;
    kj::Maybe<kj::Own<ExplicitTxn>> parent;
    uint depth = 0;
    bool hasChild = false;
    bool committed = false;

    void rollbackImpl();
  };

  // When set to NoTxn, there is no transaction outstanding.
  //
  // When set to `ImplicitTxn*`, an implicit transaction is currently open, owned by `commitTasks`.
  // If there is a need to commit this early, e.g. to start an explicit transaction, that can be
  // done through this reference.
  //
  // When set to `ExplicitTxn*`, an explicit transaction is currently open, so no implicit
  // transactions should be used in the meantime.
  kj::OneOf<NoTxn, ImplicitTxn*, ExplicitTxn*> currentTxn = NoTxn();

  kj::TaskSet commitTasks;

  void onWrite();

  void taskFailed(kj::Exception&& exception) override;

  void requireNotBroken();
};

}  // namespace workerd
