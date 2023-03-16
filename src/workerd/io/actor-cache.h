// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <workerd/io/actor-storage.h>
#include <workerd/io/io-context.h>
#include <kj/one-of.h>
#include <kj/map.h>
#include <kj/list.h>
#include <kj/time.h>
#include <kj/mutex.h>
#include <atomic>
#include <workerd/util/wait-list.h>

namespace workerd {

using kj::byte;
using kj::uint;
class OutputGate;

struct ActorCacheReadOptions {
  bool noCache = false;
  // If the entry is not already in cache and has to be read from disk, don't store the result in
  // cache, only return it to the caller.
  //
  // If there is already a matching entry in cache, that value will be returned as normal. Hence,
  // `noCache` does not affect consistency, only performance.
};

struct ActorCacheWriteOptions {
  bool allowUnconfirmed = false;
  // Instructs that the output gate should not wait for this write to be confirmed on disk. Write
  // failures will still break the output gate -- but the application could potentially return a
  // result before the failure is observed, leading to a prematurely confirmed write.

  bool noCache = false;
  // Once the value has been confirmed written to disk, immediately evict it from the cache.
  //
  // Until the value is safely on disk, the dirty value will be used to fulfill reads for the same
  // key.  Hence, `noCache` does not affect consistency, only performance.
};

class ActorCacheOps {
  // Common interface between ActorCache and ActorCache::Transaction.
public:
  typedef kj::String Key;
  typedef kj::StringPtr KeyPtr;
  static inline Key cloneKey(KeyPtr ptr) { return kj::str(ptr); }
  // Keys are text for now, but we could also change this to `Array<const byte>`.

  typedef kj::Array<const byte> Value;
  typedef kj::ArrayPtr<const byte> ValuePtr;
  // Values are raw bytes.

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

  class GetResultList;
  // An iterable type where each element is a KeyValuePtrPair.

  struct CleanAlarm{};

  struct DirtyAlarm {
    kj::Maybe<kj::Date> newTime;
  };

  using MaybeAlarmChange = kj::OneOf<CleanAlarm, DirtyAlarm>;

  struct DirtyAlarmWithOptions : public DirtyAlarm {
    ActorCacheWriteOptions options;
  };

  typedef ActorCacheReadOptions ReadOptions;

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

  typedef ActorCacheWriteOptions WriteOptions;

  virtual kj::Maybe<kj::Promise<void>> put(
      Key key, Value value, WriteOptions options) = 0;
  virtual kj::Maybe<kj::Promise<void>> put(
      kj::Array<KeyValuePair> pairs, WriteOptions options) = 0;
  // Writes a key/value into cache and schedules it to be flushed to disk later.
  //
  // The cache will automatically arrange to flush changes to disk, adding the flush to the
  // `OutputGate` passed to the constructor.
  //
  // This returns a promise for backpressure. If a promise is returned, the application should
  // delay further puts until the promise resolves. This happens when too much data is pinned in
  // cache because writes haven't been flushed to disk yet. Dropping this promise will not cancel
  // the put.

  virtual kj::Maybe<kj::Promise<void>> setAlarm(kj::Maybe<kj::Date> newTime, WriteOptions options) = 0;
  // Writes a new alarm time into cache and schedules it to be flushed to disk later, same as put().

  virtual kj::OneOf<bool, kj::Promise<bool>> delete_(
      Key key, WriteOptions options) = 0;
  virtual kj::OneOf<uint, kj::Promise<uint>> delete_(
      kj::Array<Key> keys, WriteOptions options) = 0;
  // Delete the gives keys.
  //
  // Returns a `bool` or `uint` if it can be immediately determined from cache how many keys were
  // present before the call. Otherwise, returns a promise which resolves after getting a response
  // from underlying storage. The promise also applies backpressure if needed, as with put().
};

class ActorCache final: public ActorCacheOps {
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

public:
  class SharedLru;
  // Shared LRU for a whole isolate.

  static constexpr auto SHUTDOWN_ERROR_MESSAGE =
      "broken.ignored; jsg.Error: "
      "Durable Object storage is no longer accessible."_kj;

  ActorCache(rpc::ActorStorage::Stage::Client storage, const SharedLru& lru, OutputGate& gate);
  ~ActorCache() noexcept(false);

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
  kj::Maybe<kj::Promise<void>> onNoPendingFlush();
  // See ActorCacheOps.

  struct DeleteAllResults {
    // We split these up so client code that doesn't need the count doesn't have to
    // wait for it just to account for backpressure
    kj::Maybe<kj::Promise<void>> backpressure;
    kj::Promise<uint> count;
  };

  DeleteAllResults deleteAll(WriteOptions options);
  // Delete everything in the actor's storage. This is not part of ActorCacheOps because it
  // is not supported as part of a transaction.
  //
  // The returned count only includes keys that were actually deleted from storage, not keys in
  // cache -- we only use the returned deleteAll count for billing, and not counting deletes of
  // entries that are only in cache is no problem for billing, those deletes don't cost us anything.

  class Transaction;
  void verifyConsistencyForTest();
  // Check for inconsistencies in the cache, e.g. redundant entries.

  kj::Maybe<kj::Promise<void>> evictStale(kj::Date now);
  // Call each time the isolate lock is taken to evict stale entries. If this returns a promise,
  // then the caller must hold off on JavaScript execution until the promise resolves -- this
  // creates back pressure when the write queue is too deep.
  //
  // (This takes a Date rather than a TimePoint because it is based on Date.now(), to avoid
  // bypassing Spectre mitigations.)

  void shutdown(kj::Maybe<const kj::Exception&> maybeException);

private:
  class DeferredAlarmDeleter {
  public:
    DeferredAlarmDeleter(ActorCache& parent): parent(parent) {}
    DeferredAlarmDeleter(DeferredAlarmDeleter&& other)
        : parent(other.parent) { other.parent = nullptr; }
    KJ_DISALLOW_COPY(DeferredAlarmDeleter);

    ~DeferredAlarmDeleter() noexcept(false) {
      KJ_IF_MAYBE(p, parent) {
        KJ_IF_MAYBE(d, p->currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
          d->status = DeferredAlarmDelete::Status::READY;
          p->ensureFlushScheduled(WriteOptions { .noCache = d->noCache });
        }
      }
    }

  private:
    kj::Maybe<ActorCache&> parent;
  };

public:
  kj::Maybe<DeferredAlarmDeleter> armAlarmHandler(kj::Date scheduledTime, bool noCache = false);
  // Call when entering the alarm handler and attach the returned object to the promise representing
  // the alarm handler's execution.
  //
  // The returned object will schedule a write to clear the alarm time if no alarm writes have been
  // made while it exists. If nullptr is returned, the alarm run should be canceled.

  void cancelDeferredAlarmDeletion() {
    KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
      currentAlarmTime = KnownAlarmTime {
        .status = KnownAlarmTime::Status::CLEAN,
        .time = deferredDelete->timeToDelete,
        .noCache = deferredDelete->noCache
      };
    }
  }

private:
  enum EntryState {
    // States that a cache entry may be in.

    DIRTY,
    // The value was set by the app via put() or delete(), and we have not yet initiated a write
    // to disk. The entry is appended to `dirtyList` whenever entering this state.
    //
    // Next state: FLUSHING (if we begin flushing to disk) or NOT_IN_CACHE (if a new put()/delete()
    // overwrites this entry first).

    FLUSHING,
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

    CLEAN,
    // The entry matches what is currently on disk. The entry is currently present in the LRU
    // queue.
    //
    // Next state: STALE (if not accessed for a while), NOT_IN_CACHE (if a new put()/delete()
    // overwrites the entry), or deleted (if evicted due to memory pressure).

    STALE,
    // Same as CLEAN, except that the entry has not been accessed since the last staleness check.
    // If it is not accessed before the next check, it will time out and be evicted.
    //
    // Next state: CLEAN (if accessed again before eviction), NOT_IN_CACHE (if a new put()/delete()
    // overwrites the entry), or deleted (if evicted due to either timeout or memory pressure).

    END_GAP,
    // This entry exists solely to mark the end of a known-empty gap. The value for this entry
    // is not known, but the previous entry has `gapIsKnownEmpty = true`. Such entries are
    // created as a result of list() operations, to mark the endpoint of the list range. List
    // ranges are exclusive of their endpoint, hence the value associated with this key is commonly
    // unknown.
    //
    // Next state: NOT_IN_CACHE, if someone get()s or put()s the real value.

    NOT_IN_CACHE
    // This entry is not currently in the cache -- it is an orphaned object. This happens e.g. if
    // a put() or delete() overwrote the entry, in which case the `Entry` object is removed from
    // the map and replaced with a new object. The old object may continue to exist if it is still
    // the subject of an outstanding get() which was initiated before the entry was overwritten.
    // This is not the only use of NOT_IN_CACHE, but in general, any Entry which is not in the
    // cache's `currentValues` map must have this state.
    //
    // Next state: deleted (Entry will be destroyed when the refcount reaches zero), or any
    //   other state if the entry is inserted into the cache.
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

    Entry(kj::Badge<ActorCache>, ActorCache& cache, Key keyParam,
          kj::Maybe<Value> valueParam, EntryState state);
    // Use makeEntry() to construct! (The Badge<> is a reminder, though obviously doesn't enforce
    // anything since Entry is already private to ActorCache.)

    Entry(kj::Badge<GetResultList>, Key keyParam, kj::Maybe<Value> valueParam);
    // Makes an entry not tied to a cache. Used by GetResultList<kj::Vector<KeyValuePair>>
    // constructor.

    ~Entry() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(Entry);

    kj::Maybe<ActorCache&> cache;
    const Key key;

    const kj::Maybe<Value> value;
    // The value associated with this key. null means the key is known not to be set -- except in
    // the END_GAP state, where `value` is always null, and this doesn't indicate any knowledge
    // about what's on disk.
    //
    // `value` cannot change after the `Entry` is constructed. When a key is overwritten, the
    // existing `Entry` is removed from the map and replaced with a new one, so that `value` does
    // not need to be modified. This allows us to avoid copying `Entry` objects by refcounting
    // them instead, especially in the case of a read operation which is only partially fulfilled
    // from cache and needs to remember the original cached values even if they are overwritten
    // before the read completes.

    EntryState state;
    // State of this key/value pair.

    bool gapIsKnownEmpty = false;
    // If true, then a past list() operation covered the space between this entry and the following
    // entry, meaning that we know for sure that there are no other keys on disk between them.

    bool noCache = false;
    // If true, then this entry should be evicted from cache immediately when it becomes CLEAN.
    // The entry still needs to reside in cache while DIRTY/FLUSHING since we need to store it
    // somewhere, and so we might as well serve cache hits based on it in the meantime.

    kj::Maybe<kj::Own<CountedDelete>> countedDelete;
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

    kj::ListLink<Entry> link;
    // If CLEAN or STALE, the entry will be in the SharedLru's `cleanList`.
    //
    // If DIRTY or FLUSHING, the entry will be in `dirtyList`.

    size_t size() const {
      size_t result = sizeof(*this) + key.size();
      KJ_IF_MAYBE(v, value) result += v->size();
      return result;
    }
  };

  class EntryTableCallbacks {
    // Callbacks for a kj::TreeIndex for a kj::Table<kj::Own<Entry>>.
  public:
    inline KeyPtr keyForRow(const kj::Own<Entry>& row) const { return row->key; }

    inline bool isBefore(const kj::Own<Entry>& row, KeyPtr key) const { return row->key < key; }
    inline bool isBefore(const kj::Own<Entry>& a, const kj::Own<Entry>& b) const {
      return a->key < b->key;
    }

    inline bool matches(const kj::Own<Entry>& row, KeyPtr key) const { return row->key == key; }
  };

  struct CountedDelete: public kj::Refcounted {
    // When delete() is called with one or more keys that aren't in cache, we will need to get
    // feedback from the database in order to report a count of deletions back to the application.
    // Entries that were originally added to the cache as part of such a `delete()` will reference
    // a `CountedDelete`.
    //
    // This object can only be manipulated in the thread that owns the specific actor that made
    // the request. That works out fine since CountedDelete only ever exists for dirty entries,
    // which won't be touched cross-thread by the LRU.

    uint countDeleted = 0;
    // Running count of entries that existed before the delete.

    kj::Own<kj::PromiseFulfiller<uint>> resultFulfiller;
    // When `countOutstanding` reaches zero, fulfill this with `countDeleted`.

    kj::Maybe<size_t> flushIndex;
    // During `flushImpl()`, when this CountedDelete is first encountered, `flushIndex` will be set
    // to track this delete batch. It will be set back to `nullptr` before `flushImpl()` returns.
    // This field exists here to avoid the need for a HashMap<CountedDelete*, ...> in `flushImpl()`.
  };

  rpc::ActorStorage::Stage::Client storage;
  const SharedLru& lru;
  OutputGate& gate;

  class DirtyList {
    // Wrapper around kj::List that keeps track of the total size of all elements.
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

  DirtyList dirtyList;
  // List of entries in DIRTY or FLUSHING state. New dirty entries are added to the end. If any
  // FLUSHING entries are present, they always appear strictly before DIRTY entries.

  kj::ExternalMutexGuarded<kj::Table<kj::Own<Entry>, kj::TreeIndex<EntryTableCallbacks>>>
      currentValues;
  // Map of current known values for keys. Searchable by key, including ordered iteration.
  //
  // This map is protected by the same lock as lru.cleanList. ExternalMutexGuarded helps enforce
  // this.

  struct UnknownAlarmTime{};
  struct KnownAlarmTime{
    enum class Status { CLEAN, DIRTY, FLUSHING } status;
    kj::Maybe<kj::Date> time;
    bool noCache = false;
  };
  struct DeferredAlarmDelete {
    // Used by armAlarmHandler to know if a write needs to happen after the handler finishes
    // to clear the alarm time.
    enum class Status { WAITING, READY, FLUSHING } status;
    kj::Date timeToDelete;
    // Set to a time to pass as `timeToDelete` when making the delete call.

    kj::Maybe<bool> wasDeleted;
    // When the delete finishes, set to whether or not it succeeded.

    bool noCache = false;
  };

  kj::OneOf<UnknownAlarmTime, KnownAlarmTime, DeferredAlarmDelete> currentAlarmTime = UnknownAlarmTime{};
  kj::Maybe<kj::Promise<void>> maybeAlarmPreviewTask;

  struct ReadCompletionChain: public kj::Refcounted {
    kj::Maybe<kj::Own<ReadCompletionChain>> next;
    kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> fulfiller;
    ReadCompletionChain() = default;
    ~ReadCompletionChain() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(ReadCompletionChain);
  };
  kj::Own<ReadCompletionChain> readCompletionChain = kj::refcounted<ReadCompletionChain>();
  // Used to implement waitForPastReads(). See that function to understand how it works...

  bool flushScheduled = false;
  // True if ensureFlushScheduled() has been called but the flush has not started yet.

  bool flushScheduledWithOutputGate = false;
  // When flushScheduled is true, indicates whether the output gate is already waiting on said
  // flush. The first write that does *not* set `allowUnconfirmed` causes the output gate to be
  // applied.

  size_t flushesEnqueued = 0;
  // The count of the number of flushes that have been queued without yet resolving.

  struct DeleteAllState {
    kj::Vector<kj::Own<Entry>> deletedDirty;
    // If deleteAll() was called since the last flush, these are all the dirty entries that existed
    // in the cache immediately before the deleteAll(). Since deleteAll() cannot be part of a
    // transaction, in order to maintain ordering guarantees, we'll need to flush these entries
    // first, then perform the deleteAll(), then flush any entries that were dirtied after the
    // deleteAll().
    kj::Own<kj::PromiseFulfiller<uint>> countFulfiller;
  };

  kj::Maybe<DeleteAllState> requestedDeleteAll;


  kj::ForkedPromise<void> lastFlush = kj::Promise<void>(kj::READY_NOW).fork();
  // Promise for the completion of the previous flush. We can only execute one flushImpl() at a time
  // because we can't allow out-of-order writes.
  //
  // TODO(perf): If we could rely on e-order on the ActorStorage API, we could pipeline additional
  //   writes and not have to worry about this. However, at present, ActorStorage has automatic
  //   reconnect behavior at the supervisor layer which violates e-order.

  kj::Maybe<kj::Exception> maybeTerminalException;
  // Did we hit a problem that makes the ActorCache unusable? If so this is the exception that
  // describes the problem.

  kj::Canceler oomCanceler;
  // Will be canceled if and when `oomException` becomes non-null.

  typedef kj::Locked<kj::List<Entry, &Entry::link>> Lock;
  // Type of a lock on `SharedLru::cleanList`. We use the same lock to protect `currentValues`.

  kj::Own<Entry> makeEntry(Lock& lock, EntryState state, Key keyParam, kj::Maybe<Value> valueParam);

  void touchEntry(Lock& lock, Entry& entry, const ReadOptions& options);
  // Indicate that an entry was observed by a read operation and so should be moved to the end of
  // the LRU queue (unless the options say otherwise).

  kj::Maybe<kj::Own<Entry>> findInCache(Lock& lock, KeyPtr key, const ReadOptions& options);
  // Look for a key in cache, returning a strong reference on the matching entry.
  //
  // Returns null if the key isn't in cache, and must be looked up on disk.
  //
  // This will never return an entry of type END_GAP -- it'll return null instead.
  //
  // In cases where the key doesn't match a specific entry, but does match a gap between entries
  // which is known to be empty, and thus the key is known not to exist on disk but has no specific
  // entry in cache, this will return a new temporary object with null `value` instead, so that
  // the calling code doesn't need to think about this case.

  kj::Own<Entry> addReadResultToCache(Lock& lock, Key key, kj::Maybe<capnp::Data::Reader> value,
                                      const ReadOptions& readOptions);
  // Add an entry to the cache, where the entry was the result of reading from storage. If another
  // entry with the same key has been inserted in the meantime, then the new entry will not be
  // inserted and will instead immediately have state NOT_IN_CACHE.
  //
  // Either way, a strong reference to the entry is returned.

  void markGapsEmpty(Lock& lock, KeyPtr begin, kj::Maybe<KeyPtr> end, const ReadOptions& options);
  // Mark all gaps empty between the begin and end key.

  void putImpl(Lock& lock, Key key, kj::Maybe<Value> value,
               const WriteOptions& options, kj::Maybe<CountedDelete&> counted);
  void putImpl(Lock& lock, kj::Own<Entry> newEntry,
               const WriteOptions& options,  kj::Maybe<CountedDelete&> counted);
  // Implements put() or delete(). Multi-key variants call this for each key.

  void ensureFlushScheduled(const WriteOptions& options);
  // Ensure that we will flush dirty entries soon.

  template <typename Func>
  kj::PromiseForResult<Func, rpc::ActorStorage::Operations::Client> scheduleStorageRead(
      Func&& function);
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

  kj::Promise<void> waitForPastReads();
  // Wait until all read operations that are currently in-flight have completed or failed
  // (including exhausting all retries). Does not propagate the read exception, if any. This is
  // used for ordering, to make sure a write is not committed too early such that it interferes
  // with a previous read.

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

  void evictEntry(Lock& lock, Entry& entry);
  // Carefully remove a clean entry from `currentValues`, making sure to update gaps.

  void clear(Lock& lock);
  // Drop the entire cache. Called during destructor and on OOM.

  void requireNotTerminal();
  // Throws OOM exception if `oom` is true.

  void evictOrOomIfNeeded(Lock& lock);
  // Evict cache entries as needed to reach the target memory usage. If the cache has exceeded the
  // hard limit, trigger an OOM, canceling all RPCs and breaking the output gate.

  kj::Maybe<kj::Promise<void>> getBackpressure();
  // If the LRU is currently over the soft limit, returns a promise that resolves when it is
  // back under the limit.

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
      KJ_IREQUIRE(ptr->get()->value != nullptr);
      return { ptr->get()->key, ptr->get()->value.orDefault(nullptr), *statusPtr };
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

  explicit GetResultList(kj::Vector<KeyValuePair> contents);
  // Construct a simple GetResultList from key-value pairs.

private:
  kj::Vector<kj::Own<Entry>> entries;
  kj::Vector<CacheStatus> cacheStatuses;

  enum Order {
    FORWARD,
    REVERSE
  };

  explicit GetResultList(kj::Vector<kj::Own<Entry>> cachedEntries,
                         kj::Vector<kj::Own<Entry>> fetchedEntries,
                         Order order, kj::Maybe<uint> limit = nullptr);
  // Merges `cachedEntries` and `fetchedEntries`, which should each already be sorted in the
  // given order. If a key exists in both, `cachedEntries` is preferred.
  //
  // After merging, if an entry's value is null, it is dropped.
  //
  // The final result is truncated to `limit`, if any.
  //
  // The idea is that `cachedEntries` is the set of entries that were loaded from cache while
  // `fetchedEntries` is the set read from storage.

  friend class ActorCache;
};

struct ActorCacheSharedLruOptions {
  // Options to ActorCache::SharedLru's constructor. Declared at top level so that it can be
  // forward-declared elsewhere.

  size_t softLimit;
  // Memory usage that the LRU will try to stay under by evicting clean values.

  size_t hardLimit;
  // Memory usage at which operations should start failing and actors should be killed for
  // exceeding memory limits.

  kj::Duration staleTimeout;
  // Time period after which a value that hasn't been accessed at all should be evicted even if
  // the total cache size is below `softLimit`.

  size_t dirtyListByteLimit;
  // How many bytes in a particular ActorCache can be dirty before backpressure is applied on the
  // app.

  size_t maxKeysPerRpc;
  // Maximum number of keys in a single RPC message during a flush. If a message would be larger
  // than this, it'll be split into multiple calls.
  //
  // This should typically be set to ActorStorageClientImpl::MAX_KEYS from
  // supervisor/actor-storage.h.

  bool noCache = false;
  // If true, assume `noCache` for all operations.

  bool neverFlush = false;
  // If true, don't actually flush anything. This is used in preview sessions, since they keep
  // state strictly in memory.
};

class ActorCache::SharedLru {
public:
  using Options = ActorCacheSharedLruOptions;

  explicit SharedLru(Options options);

  ~SharedLru() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SharedLru);

  size_t currentSize() const { return size.load(std::memory_order_relaxed); }
  // Mostly for testing.

private:
  Options options;

  kj::MutexGuarded<kj::List<Entry, &Entry::link>> cleanList;
  // List of clean values, across all caches, ordered from least-recently-used to
  // most-recently-used. Note that due to the ordering, all STALE entries will appear before all
  // CLEAN entries.

  mutable std::atomic<size_t> size = 0;
  // Total byte size of everything that is cached, including dirty values that are not in `list`.

  mutable std::atomic<int64_t> nextStaleCheckNs = 0;
  // TimePoint when we should next evict stale entries. Represented as an int64_t of nanoseconds
  // instead of kj::TimePoint to allow for atomic operations.

  bool evictIfNeeded(Lock& lock) const KJ_WARN_UNUSED_RESULT;
  // Evict cache entries as needed according to the cache limits. Returns true if the hard limit
  // is exceeded and nothing can be evicted, in which case the caller should fail out in the
  // appropriate way for the kind of operation being performed.

  friend class ActorCache;
};

class ActorCache::Transaction final: public ActorCacheOps {
  // A transaction represents a set of writes that haven't been committed. The transaction can be
  // discarded without committing.
  //
  // ActorCache::Transaction intentionally does NOT detect conflicts with concurrent transactions.
  // It is up to a higher layer to make sure that only one transaction occurs at a time, perhaps
  // using critical sections.

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

  kj::Maybe<kj::Promise<void>> commit();
  // Write all changes to the underlying ActorCache.
  //
  // If commit() is not called before the Transaction is destroyed, nothing is written.
  //
  // Returns a promise if backpressure needs to be applied (like ActorCache::put()).
  //
  // This will NOT detect conflicts, it will always just write blindly, because conflicts
  // inherently cannot happen.

  kj::Promise<void> rollback();

private:
  ActorCache& cache;

  struct Change {
    kj::Own<Entry> entry;
    WriteOptions options;
  };

  class ChangeTableCallbacks {
    // Callbacks for a kj::TreeIndex for a kj::Table<Change>.
  public:
    inline KeyPtr keyForRow(const Change& row) const { return row.entry->key; }

    inline bool isBefore(const Change& row, KeyPtr key) const { return row.entry->key < key; }
    inline bool matches(const Change& row, KeyPtr key) const { return row.entry->key == key; }
  };

  kj::Table<Change, kj::TreeIndex<ChangeTableCallbacks>> entriesToWrite;

  kj::Maybe<DirtyAlarmWithOptions> alarmChange;

  kj::OneOf<GetResultList, kj::Promise<GetResultList>> merge(
      kj::Vector<kj::Own<Entry>> changedEntries,
      kj::OneOf<GetResultList, kj::Promise<GetResultList>> cacheRead,
      GetResultList::Order order);
  // Merge the changes in the transaction with the results from reading from the underlying
  // ActorCache.

  kj::Maybe<KeyPtr> putImpl(Lock& lock, Key key, kj::Maybe<Value> value,
                            const WriteOptions& options, kj::Maybe<uint&> count = nullptr);
  // Adds the given key/value pair to `changes`. If an existing entry is replaced, *count is
  // incremented if it was a positive entry. If no existing entry is replaced, then the key
  // is returned, indicating that if a count is needed, we'll need to inspect cache/disk.
};

}  // namespace workerd
