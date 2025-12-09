// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "actor-cache.h"

#include <workerd/io/trace.h>
#include <workerd/util/sqlite-kv.h>
#include <workerd/util/sqlite-metadata.h>

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
    // Makes a request to the alarm manager to run the alarm handler at the given time, returning
    // a promise that resolves when the scheduling has succeeded. `priorTask` is any work we must
    // wait on prior to scheduling the new request, as of this writing, this would be the
    // alarmLaterChain, which holds promises to move the alarm time "later" than is currently set.
    virtual kj::Promise<void> scheduleRun(
        kj::Maybe<kj::Date> newAlarmTime, kj::Promise<void> priorTask);

    static const Hooks DEFAULT;

    static constexpr inline Hooks& getDefaultHooks() {
      // Hooks has no member variables, so const_cast is acceptable.
      return const_cast<Hooks&>(Hooks::DEFAULT);
    }
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
      Hooks& hooks = Hooks::getDefaultHooks(),
      bool debugAlarmSync = false);

  bool isCommitScheduled() {
    return !currentTxn.is<NoTxn>() || deleteAllCommitScheduled;
  }

  kj::Maybe<SqliteDatabase&> getSqliteDatabase() override {
    return *db;
  }

  kj::Maybe<SqliteKv&> getSqliteKv() override {
    requireNotBroken();
    return kv;
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
  kj::OneOf<CancelAlarmHandler, RunAlarmHandler> armAlarmHandler(
      kj::Date scheduledTime, bool noCache = false, kj::StringPtr actorId = "") override;
  void cancelDeferredAlarmDeletion() override;
  kj::Maybe<kj::Promise<void>> onNoPendingFlush(SpanParent parentSpan) override;
  kj::Promise<kj::String> getCurrentBookmark(SpanParent parentSpan) override;
  kj::Promise<void> waitForBookmark(kj::StringPtr bookmark, SpanParent parentSpan) override;
  // See ActorCacheInterface

 private:
  kj::Own<SqliteDatabase> db;
  OutputGate& outputGate;
  kj::Function<kj::Promise<void>()> commitCallback;
  Hooks& hooks;
  SqliteKv kv;
  SqliteMetadata metadata;

  // Define a SqliteDatabase::Regulator that is similar to TRUSTED but turns certain SQLite errors
  // into application errors as appropriate when committing an implicit transaction.
  class TxnCommitRegulator: public SqliteDatabase::Regulator {
   public:
    void onError(kj::Maybe<int> sqliteErrorCode, kj::StringPtr message) const override;
  };
  static constexpr TxnCommitRegulator TRUSTED_TXN_COMMIT;

  SqliteDatabase::Statement beginTxn = db->prepare("BEGIN TRANSACTION");
  SqliteDatabase::Statement commitTxn = db->prepare(TRUSTED_TXN_COMMIT, "COMMIT TRANSACTION");

  kj::Maybe<kj::Exception> broken;

  struct NoTxn {};

  class ImplicitTxn {
   public:
    explicit ImplicitTxn(ActorSqlite& parent);
    ~ImplicitTxn() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ImplicitTxn);

    void commit();
    void rollback();

    void setSomeWriteConfirmed(bool someWriteConfirmed);
    bool isSomeWriteConfirmed() const;

   private:
    ActorSqlite& parent;

    bool committed = false;

    // True if any of the writes in this commit are confirmed writes.
    bool someWriteConfirmed = false;
  };

  class ExplicitTxn: public ActorCacheInterface::Transaction, public kj::Refcounted {
   public:
    ExplicitTxn(ActorSqlite& actorSqlite);
    ~ExplicitTxn() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ExplicitTxn);

    bool getAlarmDirty();
    void setAlarmDirty();

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
    bool alarmDirty = false;

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

  // If true, then a commit is scheduled as a result of deleteAll() having been called.
  bool deleteAllCommitScheduled = false;

  // State for tracking completion of all commits (both confirmed and unconfirmed) for implementing
  // sync() in onNoPendingFlush.
  kj::ForkedPromise<void> lastCommit = kj::Promise<void>(kj::READY_NOW).fork();

  // Backs the `kj::Own<void>` returned by `armAlarmHandler()`.
  class DeferredAlarmDeleter: public kj::Disposer {
   public:
    // The `Own<void>` returned by `armAlarmHandler()` is actually set up to point to the
    // `ActorSqlite` itself, but with an alternate disposer that deletes the alarm rather than
    // the whole object.
    void disposeImpl(void* pointer) const override {
      reinterpret_cast<ActorSqlite*>(pointer)->maybeDeleteDeferredAlarm();
    }
  };

  // We need to track some additional alarm state to guarantee at-least-once alarm delivery:
  // Within an alarm handler, we want the observable alarm state to look like the running alarm
  // was deleted at the start of the handler (when armAlarmHandler() is called), but we don't
  // actually want to persist that deletion until after the handler has successfully completed.
  bool haveDeferredDelete = false;

  // Some state only used for tracking calling invariants.
  bool inAlarmHandler = false;

  // The alarm state for which we last received confirmation that the db was durably stored.
  kj::Maybe<kj::Date> lastConfirmedAlarmDbState;

  // The latest time we'd expect a scheduled alarm to fire, given the current set of in-flight
  // scheduling requests, without yet knowing if any of them succeeded or failed.  We use this
  // value to maintain the invariant that the scheduled alarm is always equal to or earlier than
  // the alarm value in the persisted database state.
  kj::Maybe<kj::Date> alarmScheduledNoLaterThan;

  // A promise for an in-progress alarm notification update and database commit.
  kj::Maybe<kj::ForkedPromise<void>> pendingCommit;

  kj::TaskSet commitTasks;

  // Promise chain for serializing "move alarm later" operations to prevent races
  // at the alarm manager. Each update waits for the previous one to complete.
  kj::ForkedPromise<void> alarmLaterChain = kj::Promise<void>(kj::READY_NOW).fork();

  // Version counter that increments on every alarm change. Used to detect if another commit
  // modified the alarm while we were async, allowing us to skip redundant post-commit alarm
  // syncs. This provides automatic coalescing of rapid alarm changes.
  uint64_t alarmVersion = 0;

  // Debug flag for tracing alarm synchronization issues for specific namespaces
  bool debugAlarmSync = false;

  void startImplicitTxn();

  void onWrite(bool allowUnconfirmed);

  void onCriticalError(kj::StringPtr errorMessage, kj::Maybe<kj::Exception> maybeException);

  // Issues a request to the alarm scheduler for the given time, returning a promise that resolves
  // when the request is confirmed.
  kj::Promise<void> requestScheduledAlarm(
      kj::Maybe<kj::Date> requestedTime, kj::Promise<void> priorTask);

  struct PrecommitAlarmState {
    // Promise for the completion of precommit alarm scheduling
    kj::Maybe<kj::Promise<void>> schedulingPromise;
  };

  // To be called just before committing the local sqlite db, to synchronously start any necessary
  // alarm scheduling:
  PrecommitAlarmState startPrecommitAlarmScheduling();

  // Performs the rest of the asynchronous commit, to be waited on after committing the local
  // sqlite db.  Should be called in the same turn of the event loop as
  // startPrecommitAlarmScheduling() and passed the state that it returned.
  kj::Promise<void> commitImpl(PrecommitAlarmState precommitAlarmState);

  void taskFailed(kj::Exception&& exception) override;

  void requireNotBroken();

  // Called when DeferredAlarmDeleter is destroyed, to delete alarm if not reset or cancelled
  // during handler.
  void maybeDeleteDeferredAlarm();
};

}  // namespace workerd
