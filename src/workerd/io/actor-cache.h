// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <workerd/io/actor-storage.h>
#include <kj/one-of.h>
#include <kj/map.h>
#include <kj/list.h>
#include <kj/time.h>
#include <kj/mutex.h>
#include <atomic>

namespace workerd {

using kj::byte;
using kj::uint;
class OutputGate;
class SqliteDatabase;

struct ActorCacheReadOptions {
  // If the entry is not already in cache and has to be read from disk, don't store the result in
  // cache, only return it to the caller.
  //
  // If there is already a matching entry in cache, that value will be returned as normal. Hence,
  // `noCache` does not affect consistency, only performance.
  bool noCache = false;
};

struct ActorCacheWriteOptions {
  // Instructs that the output gate should not wait for this write to be confirmed on disk. Write
  // failures will still break the output gate -- but the application could potentially return a
  // result before the failure is observed, leading to a prematurely confirmed write.
  bool allowUnconfirmed = false;

  // Once the value has been confirmed written to disk, immediately evict it from the cache.
  //
  // Until the value is safely on disk, the dirty value will be used to fulfill reads for the same
  // key.  Hence, `noCache` does not affect consistency, only performance.
  bool noCache = false;
};

// Common interface between ActorCache and ActorCache::Transaction.
class ActorCacheOps {
public:
  typedef kj::String Key;
  typedef kj::StringPtr KeyPtr;
  // Keys are text for now, but we could also change this to `Array<const byte>`.
  static inline Key cloneKey(KeyPtr ptr) { return kj::str(ptr); }

  // Values are raw bytes.
  typedef kj::Array<const byte> Value;
  typedef kj::ArrayPtr<const byte> ValuePtr;

  struct KeyValuePair {
    Key key;
    Value value;
  };
  struct KeyValuePtrPair {
    KeyPtr key;
    ValuePtr value;

    KeyValuePtrPair(const KeyValuePair& other): key(other.key), value(other.value) {}
    KeyValuePtrPair(KeyPtr key, ValuePtr value): key(key), value(value) {}
  };

  struct KeyRename {
    Key oldKey;
    Key newKey;
  };

  enum class CacheStatus {
    CACHED,
    UNCACHED
  };

  struct KeyValuePtrPairWithCache: public KeyValuePtrPair {
    CacheStatus status;

    KeyValuePtrPairWithCache(const KeyValuePtrPair& other, CacheStatus status)
        : KeyValuePtrPair(other), status(status) {}
    KeyValuePtrPairWithCache(const KeyValuePtrPairWithCache& other)
        : KeyValuePtrPair(other.key, other.value), status(other.status) {}
    KeyValuePtrPairWithCache(KeyPtr key, ValuePtr value, CacheStatus status)
        : KeyValuePtrPair(key, value), status(status) {}
  };

  // An iterable type where each element is a KeyValuePtrPair.
  class GetResultList;

  struct CleanAlarm{};

  struct DirtyAlarm {
    kj::Maybe<kj::Date> newTime;
  };

  using MaybeAlarmChange = kj::OneOf<CleanAlarm, DirtyAlarm>;

  struct DirtyAlarmWithOptions : public DirtyAlarm {
    ActorCacheWriteOptions options;
  };

  typedef ActorCacheReadOptions ReadOptions;

  // Get the values for some key, keys, or range of keys.
  //
  // Returns a Maybe<Value> or GetResultList if the result is immediately available from cache,
  // otherwise returns a Promise and fetches the results from storage.
  //
  // `listReverse()` lists in reverse, which turns out to require a subtly different implementation
  // of pretty much the entire algorithm.
  //
  // Passing a null key for `end` means list through the last key in the actor. For `begin`, you
  // can pass an empty string to list from the first key in the actor, since the empty string is
  // the first possible key.
  virtual kj::OneOf<kj::Maybe<Value>, kj::Promise<kj::Maybe<Value>>> get(
      Key key, ReadOptions options) = 0;
  virtual kj::OneOf<GetResultList, kj::Promise<GetResultList>> get(
      kj::Array<Key> keys, ReadOptions options) = 0;
  virtual kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> getAlarm(
      ReadOptions options) = 0;
  virtual kj::OneOf<GetResultList, kj::Promise<GetResultList>> list(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) = 0;
  virtual kj::OneOf<GetResultList, kj::Promise<GetResultList>> listReverse(
      Key begin, kj::Maybe<Key> end, kj::Maybe<uint> limit, ReadOptions options) = 0;

  typedef ActorCacheWriteOptions WriteOptions;

  // Writes a key/value into cache and schedules it to be flushed to disk later.
  //
  // The cache will automatically arrange to flush changes to disk, adding the flush to the
  // `OutputGate` passed to the constructor.
  //
  // This returns a promise for backpressure. If a promise is returned, the application should
  // delay further puts until the promise resolves. This happens when too much data is pinned in
  // cache because writes haven't been flushed to disk yet. Dropping this promise will not cancel
  // the put.
  virtual kj::Maybe<kj::Promise<void>> put(
      Key key, Value value, WriteOptions options) = 0;
  virtual kj::Maybe<kj::Promise<void>> put(
      kj::Array<KeyValuePair> pairs, WriteOptions options) = 0;

  // Writes a new alarm time into cache and schedules it to be flushed to disk later, same as put().
  virtual kj::Maybe<kj::Promise<void>> setAlarm(kj::Maybe<kj::Date> newTime, WriteOptions options) = 0;

  // Delete the gives keys.
  //
  // Returns a `bool` or `uint` if it can be immediately determined from cache how many keys were
  // present before the call. Otherwise, returns a promise which resolves after getting a response
  // from underlying storage. The promise also applies backpressure if needed, as with put().
  virtual kj::OneOf<bool, kj::Promise<bool>> delete_(
      Key key, WriteOptions options) = 0;
  virtual kj::OneOf<uint, kj::Promise<uint>> delete_(
      kj::Array<Key> keys, WriteOptions options) = 0;
};

// Abstract interface that is implemneted by ActorCache as well as ActorSqlite.
//
// This extends ActorCacheOps and adds some methods that don't make sense as part of
// ActorCache::Transaction.
class ActorCacheInterface: public ActorCacheOps {
public:
  // If the actor's storage is backed by SQLite, return the underlying database.
  virtual kj::Maybe<SqliteDatabase&> getSqliteDatabase() = 0;

  class Transaction: public ActorCacheOps {
  public:
    // Write all changes to the underlying ActorCache.
    //
    // If commit() is not called before the Transaction is destroyed, nothing is written.
    //
    // Returns a promise if backpressure needs to be applied (like ActorCache::put()).
    //
    // This will NOT detect conflicts, it will always just write blindly, because conflicts
    // inherently cannot happen.
    virtual kj::Maybe<kj::Promise<void>> commit() = 0;

    virtual kj::Promise<void> rollback() = 0;
  };

  virtual kj::Own<Transaction> startTransaction() = 0;

  // We split these up so client code that doesn't need the count doesn't have to
  // wait for it just to account for backpressure
  struct DeleteAllResults {
    kj::Maybe<kj::Promise<void>> backpressure;
    kj::Promise<uint> count;
  };

  // Delete everything in the actor's storage. This is not part of ActorCacheOps because it
  // is not supported as part of a transaction.
  //
  // The returned count only includes keys that were actually deleted from storage, not keys in
  // cache -- we only use the returned deleteAll count for billing, and not counting deletes of
  // entries that are only in cache is no problem for billing, those deletes don't cost us anything.
  virtual DeleteAllResults deleteAll(WriteOptions options) = 0;

  // Call each time the isolate lock is taken to evict stale entries. If this returns a promise,
  // then the caller must hold off on JavaScript execution until the promise resolves -- this
  // creates back pressure when the write queue is too deep.
  //
  // (This takes a Date rather than a TimePoint because it is based on Date.now(), to avoid
  // bypassing Spectre mitigations.)
  virtual kj::Maybe<kj::Promise<void>> evictStale(kj::Date now) = 0;

  virtual void shutdown(kj::Maybe<const kj::Exception&> maybeException) = 0;

  // Call when entering the alarm handler and attach the returned object to the promise representing
  // the alarm handler's execution.
  //
  // The returned object will schedule a write to clear the alarm time if no alarm writes have been
  // made while it exists. If kj::none is returned, the alarm run should be canceled.
  virtual kj::Maybe<kj::Own<void>> armAlarmHandler(
      kj::Date scheduledTime, bool noCache = false) = 0;

  virtual void cancelDeferredAlarmDeletion() = 0;

  virtual kj::Maybe<kj::Promise<void>> onNoPendingFlush() = 0;

  // Implements the respective PITR API calls. The default implementations throw JSG errors saying
  // PITR is not implemented.
  virtual kj::Promise<kj::String> getCurrentBookmark();
  virtual kj::Promise<kj::String> getBookmarkForTime(kj::Date timestamp);
  virtual kj::Promise<kj::String> onNextSessionRestoreBookmark(kj::StringPtr bookmark);
};

// An in-memory caching layer on top of ActorStorage.Stage RPC interface.
//
// This cache assumes that it is the only client of the underlying storage -- which is, of
// course, true for actors.
//
// Writes complete "instantly" -- but the OutputGate is told to block output until the write is
// confirmed durable.
//
// Ordering is carefully preserved. A read will always return results consistent with the time
// when it was called, never reflecting later writes -- even writes that are performed before
// the read actually completes. Writes are never committed out-of-order (this is accomplished by
// brute force -- the cache always performs a transaction committing all dirty keys at once).
//
// The cache implements LRU eviction triggered by both time and memory pressure. Memory usage is
// accounted across many actors (typically, all actors in the same isolate), so that the cache
// size limit can be set based on the per-isolate memory limit.
class ActorCache final: public ActorCacheInterface {
public:
  // Shared LRU for a whole isolate.
  class SharedLru;

  // Hooks that can be used to customize ActorCache behavior
  class Hooks {
  public:
    // Called when the alarm time is dirty when neverFlush is set and ensureFlushScheduled is called.
    virtual void updateAlarmInMemory(kj::Maybe<kj::Date> newAlarmTime) {};

    static Hooks DEFAULT;
  };

  static constexpr auto SHUTDOWN_ERROR_MESSAGE =
      "broken.ignored; jsg.Error: "
      "Durable Object storage is no longer accessible."_kj;

  ActorCache(rpc::ActorStorage::Stage::Client storage, const SharedLru& lru, OutputGate& gate,
      Hooks& hooks = Hooks::DEFAULT);
  ~ActorCache() noexcept(false);

  kj::Maybe<SqliteDatabase&> getSqliteDatabase() override { return kj::none; }
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
  kj::Maybe<kj::Promise<void>> setAlarm(kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) override;
  // See ActorCacheOps.

  kj::Own<ActorCacheInterface::Transaction> startTransaction() override;
  DeleteAllResults deleteAll(WriteOptions options) override;
  kj::Maybe<kj::Promise<void>> evictStale(kj::Date now) override;
  void shutdown(kj::Maybe<const kj::Exception&> maybeException) override;
  kj::Maybe<kj::Own<void>> armAlarmHandler(kj::Date scheduledTime, bool noCache = false) override;
  void cancelDeferredAlarmDeletion() override;
  kj::Maybe<kj::Promise<void>> onNoPendingFlush() override;
  // See ActorCacheInterface

  class Transaction;
  // Check for inconsistencies in the cache, e.g. redundant entries.
  void verifyConsistencyForTest();

private:
  // Backs the `kj::Own<void>` returned by `armAlarmHandler()`.
  class DeferredAlarmDeleter: public kj::Disposer {
  public:
    // The `Own<void>` returned by `armAlarmHandler()` is actually set up to point to the
    // `ActorCache` itself, but with an alterante disposer that deletes the alarm rather than
    // the whole object.
    void disposeImpl(void* pointer) const {
      auto p = reinterpret_cast<ActorCache*>(pointer);
      KJ_IF_SOME(d, p->currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
        d.status = DeferredAlarmDelete::Status::READY;
        p->ensureFlushScheduled(WriteOptions { .noCache = d.noCache });
      }
    }
  };

  enum class EntrySyncStatus : int8_t {
    // The value was set by the app via put() or delete(), and we have not yet initiated a write
    // to disk. The entry is appended to `dirtyList` whenever entering this state.
    //
    // Next state: FLUSHING (if we begin flushing to disk) or NOT_IN_CACHE (if a new put()/delete()
    // overwrites this entry first).
    DIRTY,

    // The value is dirty but an RPC is in-flight to write this value to disk. If the value is
    // overwritten in the meantime, or the write fails, then the state needs to change back to
    // DIRTY. If the write completes successfully and the state is still FLUSHING, then the state
    // can progress to CLEAN.
    //
    // The entry remains in `dirtyList` while in this state. Since newly-dirty entries are always
    // appended to `dirtyList`, it's guaranteed that all `FLUSHING` entries in `dirtyList` come
    // before all `DIRTY` entries.
    //
    // Next state: CLEAN (if the flush completes) or NOT_IN_CACHE (if a new put()/delete()
    // overwrites the entry first).
    FLUSHING,

    // The entry matches what is currently on disk. The entry is currently present in the LRU
    // queue.
    //
    // Next state: NOT_IN_CACHE (if a new put()/delete() overwrites the entry), or deleted (if
    // evicted due to memory pressure).
    CLEAN,

    // This entry is not currently in the cache -- it is an orphaned object. This happens e.g. if
    // a put() or delete() overwrote the entry, in which case the `Entry` object is removed from
    // the map and replaced with a new object. The old object may continue to exist if it is still
    // the subject of an outstanding get() which was initiated before the entry was overwritten.
    // This is not the only use of NOT_IN_CACHE, but in general, any Entry which is not in the
    // cache's `currentValues` map must have this state.
    //
    // Next state: deleted (Entry will be destroyed when the refcount reaches zero), or any
    //   other state if the entry is inserted into the cache.
    NOT_IN_CACHE
  };

  enum class EntryValueStatus : uint8_t {
    // This entry has a known value. (Note that while there is nothing wrong per say with a value
    // size of zero, v8 serialized data will always have a greater size.)
    PRESENT,

    // This entry is known to be absent.
    ABSENT,

    // This entry has not been fetched into cache yet, but the previous entry has `gapIsKnownEmpty =
    // true`. Such entries are created as a result of list() operations, to mark the endpoint of the
    // list range. List ranges are exclusive of their endpoint, hence the value associated with this
    // key is commonly unknown.
    UNKNOWN,
  };

  struct CountedDelete;

  struct Entry: public kj::AtomicRefcounted {
    // A cache entry.
    //
    // Entries are refcounted so that an operation which cares about a particular entry can keep
    // it live even after it has been evicted or overwritten. In particular, because read
    // operations are consistent with the time when read() was called, they may need to hold
    // strong references to the entries they are reading, so that if the entries are overwritten,
    // the read operation still has the original value from when it was called.
    //
    // The mutable content of an `Entry` is protected by the same mutex that protects
    // `lru.cleanList`. `key` and `value` are declared `const` so that they can safely be used
    // without a lock.

    Entry(ActorCache& cache, Key key, Value value);
    Entry(ActorCache& cache, Key key, EntryValueStatus status);
    Entry(Key key, Value value);
    Entry(Key key, EntryValueStatus status);
    ~Entry() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(Entry);

    kj::Maybe<ActorCache&> maybeCache;
    const Key key;

  private:
    // The value associated with this key. If our `valueStatus` below is `ABSENT` or `UNKNOWN`, it
    // will have size 0.
    //
    // `value` cannot change after the `Entry` is constructed. When a key is overwritten, the
    // existing `Entry` is removed from the map and replaced with a new one, so that `value` does
    // not need to be modified. This allows us to avoid copying `Entry` objects by refcounting
    // them instead, especially in the case of a read operation which is only partially fulfilled
    // from cache and needs to remember the original cached values even if they are overwritten
    // before the read completes.
    const Value value;
  public:
    EntryValueStatus valueStatus;
    kj::Maybe<ValuePtr> getValuePtr() const {
      if (valueStatus == EntryValueStatus::PRESENT) {
        return value.asPtr();
      } else {
        return kj::none;
      }
    }
    kj::Maybe<Value> getValue() const {
      KJ_IF_SOME(ptr, getValuePtr()) {
        return ptr.attach(kj::atomicAddRef(*this));
      } else {
        return kj::none;
      }
    }

    // This enum indicates how synchronized this entry is with storage.
    EntrySyncStatus syncStatus = EntrySyncStatus::NOT_IN_CACHE;

    bool isDirty() const {
      switch(syncStatus) {
        case EntrySyncStatus::DIRTY:
        case EntrySyncStatus::FLUSHING: {
          return true;
        }
        case EntrySyncStatus::CLEAN: {
          return false;
        }
        case EntrySyncStatus::NOT_IN_CACHE: {
          KJ_FAIL_ASSERT("NOT_IN_CACHE entries should not be in the map or flushing");
        }
      }
    }

    bool isStale = false;

    // If true, then a past list() operation covered the space between this entry and the following
    // entry, meaning that we know for sure that there are no other keys on disk between them.
    bool gapIsKnownEmpty = false;

    // If true, then this entry should be evicted from cache immediately when it becomes CLEAN.
    // The entry still needs to reside in cache while DIRTY/FLUSHING since we need to store it
    // somewhere, and so we might as well serve cache hits based on it in the meantime.
    bool noCache = false;

    // In the DIRTY or FLUSHING state, if this entry was originally created as the result of a
    // `delete()` call, and as such the caller needs to receive a count of deletions, then this
    // tracks that need. Note that only one caller could ever be waiting on this, because
    // subsequent delete() calls can be counted based on the cache content. This can be null
    // if no delete operations need a count from this entry.
    //
    // If an entry is overwritten, `countedDelete` needs to be inherited by the replacement entry,
    // so that the delete is still counted upon `flushImpl()`. (If the entry being replaced is
    // already flushing, and that flush succeeds, then countedDelete->fulfiller will be fulfilled.
    // In that case, it's no longer relevant to have `countedDelete` on the replacement entry,
    // because it's already fulfilled and so will be ignored anyway. However, in the unlikely case
    // that the flush failed, then it is actually important that the `countedDelete` has been moved
    // to the replacement entry, so that it can be retried.)
    kj::Maybe<kj::Own<CountedDelete>> countedDelete;

    // If CLEAN, the entry will be in the SharedLru's `cleanList`.
    //
    // If DIRTY or FLUSHING, the entry will be in `dirtyList`.
    kj::ListLink<Entry> link;

    size_t size() const {
      return sizeof(*this) + key.size() + value.size();
    }
  };

  // Callbacks for a kj::TreeIndex for a kj::Table<kj::Own<Entry>>.
  class EntryTableCallbacks {
  public:
    inline KeyPtr keyForRow(const kj::Own<Entry>& row) const { return row->key; }

    inline bool isBefore(const kj::Own<Entry>& row, KeyPtr key) const { return row->key < key; }
    inline bool isBefore(const kj::Own<Entry>& a, const kj::Own<Entry>& b) const {
      return a->key < b->key;
    }

    inline bool matches(const kj::Own<Entry>& row, KeyPtr key) const { return row->key == key; }
  };

  // When delete() is called with one or more keys that aren't in cache, we will need to get
  // feedback from the database in order to report a count of deletions back to the application.
  // Entries that were originally added to the cache as part of such a `delete()` will reference
  // a `CountedDelete`.
  //
  // This object can only be manipulated in the thread that owns the specific actor that made
  // the request. That works out fine since CountedDelete only ever exists for dirty entries,
  // which won't be touched cross-thread by the LRU.
  struct CountedDelete: public kj::Refcounted {
    // Running count of entries that existed before the delete.
    uint countDeleted = 0;

    // When `countOutstanding` reaches zero, fulfill this with `countDeleted`.
    kj::Own<kj::PromiseFulfiller<uint>> resultFulfiller;

    // During `flushImpl()`, when this CountedDelete is first encountered, `flushIndex` will be set
    // to track this delete batch. It will be set back to `kj::none` before `flushImpl()` returns.
    // This field exists here to avoid the need for a HashMap<CountedDelete*, ...> in `flushImpl()`.
    kj::Maybe<size_t> flushIndex;
  };

  rpc::ActorStorage::Stage::Client storage;
  const SharedLru& lru;
  OutputGate& gate;
  Hooks& hooks;
  const kj::MonotonicClock& clock;

  // Wrapper around kj::List that keeps track of the total size of all elements.
  class DirtyList {
  public:
    void add(Entry& entry) {
      inner.add(entry);
      innerSize += entry.size();
    }

    void remove(Entry& entry) {
      inner.remove(entry);
      innerSize -= entry.size();
    }

    size_t sizeInBytes() {
      return innerSize;
    }

    auto begin() { return inner.begin(); }
    auto end() { return inner.end(); }

  private:
    kj::List<Entry, &Entry::link> inner;
    size_t innerSize = 0;
  };

  // List of entries in DIRTY or FLUSHING state. New dirty entries are added to the end. If any
  // FLUSHING entries are present, they always appear strictly before DIRTY entries.
  DirtyList dirtyList;

  // Map of current known values for keys. Searchable by key, including ordered iteration.
  //
  // This map is protected by the same lock as lru.cleanList. ExternalMutexGuarded helps enforce
  // this.
  kj::ExternalMutexGuarded<kj::Table<kj::Own<Entry>, kj::TreeIndex<EntryTableCallbacks>>>
      currentValues;

  struct UnknownAlarmTime{};
  struct KnownAlarmTime{
    enum class Status { CLEAN, DIRTY, FLUSHING } status;
    kj::Maybe<kj::Date> time;
    bool noCache = false;
  };

  // Used by armAlarmHandler to know if a write needs to happen after the handler finishes
  // to clear the alarm time.
  struct DeferredAlarmDelete {
    enum class Status { WAITING, READY, FLUSHING } status;

    // Set to a time to pass as `timeToDelete` when making the delete call.
    kj::Date timeToDelete;

    // When the delete finishes, set to whether or not it succeeded.
    kj::Maybe<bool> wasDeleted;

    bool noCache = false;
  };

  kj::OneOf<UnknownAlarmTime, KnownAlarmTime, DeferredAlarmDelete> currentAlarmTime = UnknownAlarmTime{};

  struct ReadCompletionChain: public kj::Refcounted {
    kj::Maybe<kj::Own<ReadCompletionChain>> next;
    kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> fulfiller;
    ReadCompletionChain() = default;
    ~ReadCompletionChain() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ReadCompletionChain);
  };
  // Used to implement waitForPastReads(). See that function to understand how it works...
  kj::Own<ReadCompletionChain> readCompletionChain = kj::refcounted<ReadCompletionChain>();

  // True if ensureFlushScheduled() has been called but the flush has not started yet.
  bool flushScheduled = false;

  // When flushScheduled is true, indicates whether the output gate is already waiting on said
  // flush. The first write that does *not* set `allowUnconfirmed` causes the output gate to be
  // applied.
  bool flushScheduledWithOutputGate = false;

  // The count of the number of flushes that have been queued without yet resolving.
  size_t flushesEnqueued = 0;

  struct DeleteAllState {
    // If deleteAll() was called since the last flush, these are all the dirty entries that existed
    // in the cache immediately before the deleteAll(). Since deleteAll() cannot be part of a
    // transaction, in order to maintain ordering guarantees, we'll need to flush these entries
    // first, then perform the deleteAll(), then flush any entries that were dirtied after the
    // deleteAll().
    kj::Vector<kj::Own<Entry>> deletedDirty;
    kj::Own<kj::PromiseFulfiller<uint>> countFulfiller;
  };

  kj::Maybe<DeleteAllState> requestedDeleteAll;


  // Promise for the completion of the previous flush. We can only execute one flushImpl() at a time
  // because we can't allow out-of-order writes.
  kj::ForkedPromise<void> lastFlush = kj::Promise<void>(kj::READY_NOW).fork();
  // TODO(perf): If we could rely on e-order on the ActorStorage API, we could pipeline additional
  //   writes and not have to worry about this. However, at present, ActorStorage has automatic
  //   reconnect behavior at the supervisor layer which violates e-order.

  // Did we hit a problem that makes the ActorCache unusable? If so this is the exception that
  // describes the problem.
  kj::Maybe<kj::Exception> maybeTerminalException;

  // Will be canceled if and when `oomException` becomes non-null.
  kj::Canceler oomCanceler;

  // Type of a lock on `SharedLru::cleanList`. We use the same lock to protect `currentValues`.
  typedef kj::Locked<kj::List<Entry, &Entry::link>> Lock;

  // Indicate that an entry was observed by a read operation and so should be moved to the end of
  // the LRU queue (unless the options say otherwise).
  void touchEntry(Lock& lock, Entry& entry, const ReadOptions& options);

  // TODO(soon) This function mostly belongs on the SharedLru, not the ActorCache. Notably,
  // `removeEntry()` has to do with the shared clean list but `evictEntry()` has to do with
  // the non-shared map. It is like this for now because generalizing the SharedLru into an
  // IsolateCache is bigger work.
  void removeEntry(Lock& lock, Entry& entry);

  // Look for a key in cache, returning a strong reference on the matching entry.
  //
  // Note that the returned entry could have `EntryValueStatus::UNKNOWN` which means we do not know
  // if it is in storage or `EntryValueStatus::ABSENT` which means we know it is not in storage.
  kj::Own<Entry> findInCache(Lock& lock, KeyPtr key, const ReadOptions& options);

  // Add an entry to the cache, where the entry was the result of reading from storage. If another
  // entry with the same key has been inserted in the meantime, then the new entry will not be
  // inserted and will instead immediately have state NOT_IN_CACHE.
  //
  // Either way, a strong reference to the entry is returned.
  kj::Own<Entry> addReadResultToCache(Lock& lock, Key key, kj::Maybe<capnp::Data::Reader> value,
                                      const ReadOptions& readOptions);


  // Mark all gaps empty between the begin and end key.
  void markGapsEmpty(Lock& lock, KeyPtr begin, kj::Maybe<KeyPtr> end, const ReadOptions& options);

  // Implements put() or delete(). Multi-key variants call this for each key.
  void putImpl(Lock& lock, kj::Own<Entry> newEntry,
               const WriteOptions& options,  kj::Maybe<CountedDelete&> counted);

  kj::Promise<kj::Maybe<Value>> getImpl(kj::Own<Entry> entry, ReadOptions options);

  // Ensure that we will flush dirty entries soon.
  void ensureFlushScheduled(const WriteOptions& options);

  // Schedule a read RPC. The given function will be invoked and provided with an
  // ActorStorage::Operations::Client on which the read operation should be performed. The function
  // might be called multiple times. The first call may be synchronous.
  //
  // This method has two purposes:
  // - Retry operations that fail due to disconnects.
  // - Ensure that reads cannot be re-ordered after writes that were originally scheduled later.
  //
  // Note that `function()` must return a plain `Promise`, not a `capnp::RemotePromise`, because
  // it is necessary to `.attach()` something to it. Use `.dropPipeline()` to convert a
  // `RemotePromise` to a plain `Promise`.
  template <typename Func>
  kj::PromiseForResult<Func, rpc::ActorStorage::Operations::Client> scheduleStorageRead(
      Func&& function);

  // Wait until all read operations that are currently in-flight have completed or failed
  // (including exhausting all retries). Does not propagate the read exception, if any. This is
  // used for ordering, to make sure a write is not committed too early such that it interferes
  // with a previous read.
  kj::Promise<void> waitForPastReads();

  kj::Promise<void> flushImpl(uint retryCount = 0);
  kj::Promise<void> flushImplDeleteAll(uint retryCount = 0);

  struct FlushBatch {
    size_t pairCount = 0;
    size_t wordCount = 0;
  };
  struct PutFlush {
    kj::Vector<Entry*> entries;
    kj::Vector<FlushBatch> batches;
  };
  struct MutedDeleteFlush {
    kj::Vector<Entry*> entries;
    kj::Vector<FlushBatch> batches;
  };
  struct CountedDeleteFlush {
    kj::Own<CountedDelete> countedDelete;
    kj::Vector<Entry*> entries;
    kj::Vector<FlushBatch> batches;
  };
  using CountedDeleteFlushes = kj::Array<CountedDeleteFlush>;
  kj::Promise<void> flushImplUsingSinglePut(PutFlush putFlush);
  kj::Promise<void> flushImplUsingSingleMutedDelete(MutedDeleteFlush mutedFlush);
  kj::Promise<void> flushImplUsingSingleCountedDelete(CountedDeleteFlush countedFlush);
  kj::Promise<void> flushImplAlarmOnly(DirtyAlarm dirty);
  kj::Promise<void> flushImplUsingTxn(
      PutFlush putFlush, MutedDeleteFlush mutedDeleteFlush,
      CountedDeleteFlushes countedDeleteFlushes, MaybeAlarmChange maybeAlarmChange);

  // Carefully remove a clean entry from `currentValues`, making sure to update gaps.
  void evictEntry(Lock& lock, Entry& entry);

  // Drop the entire cache. Called during destructor and on OOM.
  void clear(Lock& lock);

  // Throws OOM exception if `oom` is true.
  void requireNotTerminal();

  // Evict cache entries as needed to reach the target memory usage. If the cache has exceeded the
  // hard limit, trigger an OOM, canceling all RPCs and breaking the output gate.
  void evictOrOomIfNeeded(Lock& lock);

  // If the LRU is currently over the soft limit, returns a promise that resolves when it is
  // back under the limit.
  kj::Maybe<kj::Promise<void>> getBackpressure();

  class GetMultiStreamImpl;
  class ForwardListStreamImpl;
  class ReverseListStreamImpl;
  friend class ActorCacheOps::GetResultList;
};

class ActorCacheOps::GetResultList {
  using Entry = ActorCache::Entry;
public:
  class Iterator {
  public:
    KeyValuePtrPairWithCache operator*() {
      KJ_IREQUIRE(ptr->get()->valueStatus == ActorCache::EntryValueStatus::PRESENT);
      return { ptr->get()->key, ptr->get()->getValuePtr().orDefault({}), *statusPtr };
    }
    Iterator& operator++() {
      ++ptr;
      ++statusPtr;
      return *this;
    }
    Iterator operator++(int) {
      auto copy = *this;
      ++ptr;
      ++statusPtr;
      return copy;
    }
    bool operator==(const Iterator& other) const {
      return ptr == other.ptr && statusPtr == other.statusPtr;
    }

  private:
    const kj::Own<Entry>* ptr;
    const CacheStatus* statusPtr;

    explicit Iterator(const kj::Own<Entry>* ptr, const CacheStatus* statusPtr)
        : ptr(ptr), statusPtr(statusPtr) {}
    friend class GetResultList;
  };

  Iterator begin() const { return Iterator(entries.begin(), cacheStatuses.begin()); }
  Iterator end() const { return Iterator(entries.end(), cacheStatuses.end()); }
  size_t size() const { return entries.size(); }

  // Construct a simple GetResultList from key-value pairs.
  explicit GetResultList(kj::Vector<KeyValuePair> contents);

private:
  kj::Vector<kj::Own<Entry>> entries;
  kj::Vector<CacheStatus> cacheStatuses;

  enum Order {
    FORWARD,
    REVERSE
  };

  // Merges `cachedEntries` and `fetchedEntries`, which should each already be sorted in the
  // given order. If a key exists in both, `cachedEntries` is preferred.
  //
  // After merging, if an entry's value is null, it is dropped.
  //
  // The final result is truncated to `limit`, if any.
  //
  // The idea is that `cachedEntries` is the set of entries that were loaded from cache while
  // `fetchedEntries` is the set read from storage.
  explicit GetResultList(kj::Vector<kj::Own<Entry>> cachedEntries,
                         kj::Vector<kj::Own<Entry>> fetchedEntries,
                         Order order, kj::Maybe<uint> limit = kj::none);

  friend class ActorCache;
};

// Options to ActorCache::SharedLru's constructor. Declared at top level so that it can be
// forward-declared elsewhere.
struct ActorCacheSharedLruOptions {
  // Memory usage that the LRU will try to stay under by evicting clean values.
  size_t softLimit;

  // Memory usage at which operations should start failing and actors should be killed for
  // exceeding memory limits.
  size_t hardLimit;

  // Time period after which a value that hasn't been accessed at all should be evicted even if
  // the total cache size is below `softLimit`.
  kj::Duration staleTimeout;

  // How many bytes in a particular ActorCache can be dirty before backpressure is applied on the
  // app.
  size_t dirtyListByteLimit;

  // Maximum number of keys in a single RPC message during a flush. If a message would be larger
  // than this, it'll be split into multiple calls.
  //
  // This should typically be set to ActorStorageClientImpl::MAX_KEYS from
  // supervisor/actor-storage.h.
  size_t maxKeysPerRpc;

  // If true, assume `noCache` for all operations.
  bool noCache = false;

  // If true, don't actually flush anything. This is used in preview sessions, since they keep
  // state strictly in memory.
  bool neverFlush = false;
};

class ActorCache::SharedLru {
public:
  using Options = ActorCacheSharedLruOptions;

  explicit SharedLru(Options options);

  ~SharedLru() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SharedLru);

  // Mostly for testing.
  size_t currentSize() const { return size.load(std::memory_order_relaxed); }

private:
  Options options;

  // List of clean values, across all caches, ordered from least-recently-used to
  // most-recently-used.
  kj::MutexGuarded<kj::List<Entry, &Entry::link>> cleanList;

  // Total byte size of everything that is cached, including dirty values that are not in `list`.
  mutable std::atomic<size_t> size = 0;

  // TimePoint when we should next evict stale entries. Represented as an int64_t of nanoseconds
  // instead of kj::TimePoint to allow for atomic operations.
  mutable std::atomic<int64_t> nextStaleCheckNs = 0;

  // Evict cache entries as needed according to the cache limits. Returns true if the hard limit
  // is exceeded and nothing can be evicted, in which case the caller should fail out in the
  // appropriate way for the kind of operation being performed.
  bool evictIfNeeded(Lock& lock) const KJ_WARN_UNUSED_RESULT;

  friend class ActorCache;
};

// A transaction represents a set of writes that haven't been committed. The transaction can be
// discarded without committing.
//
// ActorCache::Transaction intentionally does NOT detect conflicts with concurrent transactions.
// It is up to a higher layer to make sure that only one transaction occurs at a time, perhaps
// using critical sections.
class ActorCache::Transaction final: public ActorCacheInterface::Transaction {
public:
  Transaction(ActorCache& cache);
  ~Transaction() noexcept(false);

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
  kj::Maybe<kj::Promise<void>> setAlarm(kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) override;
  // Same interface as ActorCache.
  //
  // Read ops will reflect the previous writes made to the transaction even though they aren't
  // committed yet.

  kj::Maybe<kj::Promise<void>> commit() override;
  kj::Promise<void> rollback() override;
  // Implements ActorCacheInterface::Transaction.

private:
  ActorCache& cache;

  struct Change {
    kj::Own<Entry> entry;
    WriteOptions options;
  };

  // Callbacks for a kj::TreeIndex for a kj::Table<Change>.
  class ChangeTableCallbacks {
  public:
    inline KeyPtr keyForRow(const Change& row) const { return row.entry->key; }

    inline bool isBefore(const Change& row, KeyPtr key) const { return row.entry->key < key; }
    inline bool matches(const Change& row, KeyPtr key) const { return row.entry->key == key; }
  };

  kj::Table<Change, kj::TreeIndex<ChangeTableCallbacks>> entriesToWrite;

  kj::Maybe<DirtyAlarmWithOptions> alarmChange;

  // Merge the changes in the transaction with the results from reading from the underlying
  // ActorCache.
  kj::OneOf<GetResultList, kj::Promise<GetResultList>> merge(
      kj::Vector<kj::Own<Entry>> changedEntries,
      kj::OneOf<GetResultList, kj::Promise<GetResultList>> cacheRead,
      GetResultList::Order order);

  // Adds the given key/value pair to `changes`. If an existing entry is replaced, *count is
  // incremented if it was a positive entry. If no existing entry is replaced, then the key
  // is returned, indicating that if a count is needed, we'll need to inspect cache/disk.
  kj::Maybe<KeyPtr> putImpl(Lock& lock, kj::Own<Entry> entry,
                            const WriteOptions& options, kj::Maybe<uint&> count = kj::none);
};

}  // namespace workerd
