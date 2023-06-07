// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-cache.h"
#include <algorithm>

#include <kj/debug.h>

#include <workerd/jsg/jsg.h>
#include <workerd/io/io-gate.h>
#include <workerd/util/sentry.h>

namespace workerd {

static constexpr size_t MAX_ACTOR_STORAGE_RPC_WORDS = (16u << 20) / sizeof(capnp::word);
// Max size, in words, of a storage RPC request. Set to 16MiB because our storage backend has a
// hard limit of 16MiB per operation.
//
// (Also, at 64MiB we'd hit the Cap'n Proto message size limit.)
//
// Note that in practice, the key size limit (options.maxKeysPerRpc) will kick in long before we
// hit this limit, so this is just a sanity check.

ActorCache::Hooks ActorCache::Hooks::DEFAULT;

ActorCache::ActorCache(rpc::ActorStorage::Stage::Client storage, const SharedLru& lru,
                       OutputGate& gate, Hooks& hooks)
    : storage(kj::mv(storage)), lru(lru), gate(gate), hooks(hooks),
      currentValues(lru.cleanList.lockExclusive()) {}

ActorCache::~ActorCache() noexcept(false) {
  // Need to remove all entries from any lists they might be in.
  auto lock = lru.cleanList.lockExclusive();
  clear(lock);
}

void ActorCache::clear(Lock& lock) {
  for (auto& entry: currentValues.get(lock)) {
    if (entry->link.isLinked()) {
      switch (entry->state) {
        case DIRTY:
        case FLUSHING:
          dirtyList.remove(*entry);
          break;
        case CLEAN:
        case STALE:
          lock->remove(*entry);
          break;
        case END_GAP:
          KJ_FAIL_REQUIRE("END_GAP entry can't be linked");
        case NOT_IN_CACHE:
          KJ_FAIL_REQUIRE("NOT_IN_CACHE entry can't be linked");
      }
    }

    // Mark entry NOT_IN_CACHE to indicate it isn't in the map anymore.
    entry->state = NOT_IN_CACHE;
  }
  currentValues.get(lock).clear();
}

kj::Own<ActorCache::Entry> ActorCache::makeEntry(
    Lock& lock, EntryState state, Key key, kj::Maybe<Value> value) {
  auto result = kj::atomicRefcounted<Entry>(
    kj::Badge<ActorCache>(), *this, kj::mv(key), kj::mv(value), state);

  lru.size.fetch_add(result->size(), std::memory_order_relaxed);

  return result;
}

ActorCache::Entry::Entry(kj::Badge<ActorCache>,ActorCache& cache, Key keyParam,
                         kj::Maybe<Value> valueParam, EntryState state)
    : cache(cache), key(kj::mv(keyParam)), value(kj::mv(valueParam)), state(state) {}

ActorCache::Entry::Entry(kj::Badge<GetResultList>, Key keyParam, kj::Maybe<Value> valueParam)
    : key(kj::mv(keyParam)), value(kj::mv(valueParam)), state(NOT_IN_CACHE) {}

ActorCache::Entry::~Entry() noexcept(false) {
  KJ_IF_MAYBE(c, cache) {
    size_t size = this->size();

    size_t before = c->lru.size.fetch_sub(size, std::memory_order_relaxed);

    if (KJ_UNLIKELY(before < size)) {
      // underflow -- shouldn't happen, but just in case, let's fix
      KJ_LOG(ERROR, "SharedLru size tracking inconsistency detected",
            before, size, kj::getStackTrace());
      c->lru.size.store(0, std::memory_order_relaxed);
    }

    KJ_REQUIRE(!link.isLinked(), "must remove Entry from lists before destroying", state);
  }
}

ActorCache::SharedLru::SharedLru(Options options): options(options) {}

ActorCache::SharedLru::~SharedLru() noexcept(false) {
  KJ_REQUIRE(cleanList.getWithoutLock().empty(),
      "ActorCache::SharedLru destroyed while an ActorCache still exists?");
  if (size.load(std::memory_order_relaxed) != 0) {
    KJ_LOG(ERROR, "SharedLru destroyed while cache entries still exist, "
        "this will lead to use-after-free");
  }
}

kj::Maybe<kj::Promise<void>> ActorCache::evictStale(kj::Date now) {
  int64_t nowNs = (now - kj::UNIX_EPOCH) / kj::NANOSECONDS;
  int64_t oldValue = lru.nextStaleCheckNs.load(std::memory_order_relaxed);

  if (nowNs >= oldValue) {
    int64_t newValue = nowNs + lru.options.staleTimeout / kj::NANOSECONDS;
    if (lru.nextStaleCheckNs.compare_exchange_strong(oldValue, newValue)) {
      auto lock = lru.cleanList.lockExclusive();
      for (auto& entry: *lock) {
        if (entry.state == STALE) {
          entry.state = NOT_IN_CACHE;
          lock->remove(entry);
          KJ_ASSERT_NONNULL(entry.cache).evictEntry(lock, entry);
        } else {
          KJ_ASSERT(entry.state == CLEAN);
          entry.state = STALE;
        }
      }
    }
  }

  // Apply backpressure if we're over the soft limit.
  return getBackpressure();
}

kj::Maybe<kj::Own<void>> ActorCache::armAlarmHandler(kj::Date scheduledTime, bool noCache) {
  noCache = noCache || lru.options.noCache;

  KJ_ASSERT(!currentAlarmTime.is<DeferredAlarmDelete>());
  bool alarmDeleteNeeded = true;
  KJ_IF_MAYBE(t, currentAlarmTime.tryGet<KnownAlarmTime>()) {
    if (t->time != scheduledTime) {
      if (t->status == KnownAlarmTime::Status::CLEAN) {
        // If there's a clean scheduledTime that is different from ours, this run should be
        // canceled.
        return nullptr;
      } else {
        // There's a alarm write that hasn't been set yet pending for a time different than ours --
        // We won't cancel the alarm because it hasn't been confirmed, but we shouldn't delete
        // the pending write.
        alarmDeleteNeeded = false;
      }
    }
  }

  if (alarmDeleteNeeded) {
    currentAlarmTime = DeferredAlarmDelete {
      .status = DeferredAlarmDelete::Status::WAITING,
      .timeToDelete = scheduledTime,
      .noCache = noCache,
      };
  }
  static const DeferredAlarmDeleter disposer;
  return kj::Own<void>(this, disposer);
}

void ActorCache::cancelDeferredAlarmDeletion() {
  KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
    currentAlarmTime = KnownAlarmTime {
      .status = KnownAlarmTime::Status::CLEAN,
      .time = deferredDelete->timeToDelete,
      .noCache = deferredDelete->noCache
    };
  }
}

kj::Maybe<kj::Promise<void>> ActorCache::getBackpressure() {
  if (dirtyList.sizeInBytes() > lru.options.dirtyListByteLimit && !lru.options.neverFlush) {
    // Wait for dirty entries to be flushed.
    return lastFlush.addBranch().then([this]() -> kj::Promise<void> {
      KJ_IF_MAYBE(p, getBackpressure()) {
        return kj::mv(*p);
      } else {
        return kj::READY_NOW;
      }
    });
  }

  // At one point, we tried applying backpressure if the total cache size was greater than
  // `softLimit`. This turned out to be a bad idea. If the cache is over the limit due to dirty
  // entries waiting to be flushed, then `dirtyListByteLimit` will actually kick in first (since
  // it's by default 8MB of data). So if the cache is over the soft limit (which is typically more
  // like 16MB), it could only be because a very large read operation has loaded a bunch of entries
  // into memory but hasn't delivered them to the app yet. In this case, if we apply backpressure,
  // then the app cannot make progress and therefore cannot receive the result of these reads! So it
  // will just deadlock.
  //
  // Hence, it only makes sense to wait for dirty entries to be flushed, not to wait for overall
  // size to go down.
  return nullptr;
}

void ActorCache::requireNotTerminal() {
  KJ_IF_MAYBE(e, maybeTerminalException) {
    if (!gate.isBroken()) {
      // We've tried to use storage after shutdown, break the output gate via `flushImpl()` so that
      // we don't let the worker return stale state. This isn't strictly necessary but it does
      // mirror previous behavior wherein we would use disabled storage via `flushImpl()` and break
      // the output gate.
      ensureFlushScheduled({});
    }

    kj::throwFatalException(kj::cp(*e));
  }
}

void ActorCache::evictOrOomIfNeeded(Lock& lock) {
  if (lru.evictIfNeeded(lock)) {
    auto exception = KJ_EXCEPTION(OVERLOADED,
        "broken.exceededMemory; jsg.Error: Durable Object's isolate exceeded its memory limit due to overflowing the "
        "storage cache. This could be due to writing too many values to storage without stopping "
        "to wait for writes to complete, or due to reading too many values in a single operation "
        "(e.g. a large list()). All objects in the isolate were reset.");

    // Add trace info sufficient to tell us which operation caused the failure.
    exception.addTraceHere();
    exception.addTrace(__builtin_return_address(0));

    if (maybeTerminalException == nullptr) {
      maybeTerminalException.emplace(kj::cp(exception));
    } else {
      // We've already experienced a terminal exception either from shutdown or oom. Note that we
      // still schedule the flush since shutdown does not.
    }

    clear(lock);
    oomCanceler.cancel(exception);

    if (!gate.isBroken()) {
      // We want to break the OutputGate. We can't quite just do `gate.lockWhile(exception)` because
      // that returns a promise which we'd then have to put somewhere so that we don't immediately
      // cancel it. Instead, we can ensure that a flush has been scheduled. `flushImpl()`, when
      // called, will throw an exception which breaks the gate.
      ensureFlushScheduled(WriteOptions());
    }

    kj::throwFatalException(kj::mv(exception));
  }
}

bool ActorCache::SharedLru::evictIfNeeded(Lock& lock) const {
  for (;;) {
    size_t current = size.load(std::memory_order_relaxed);
    if (current <= options.softLimit) {
      // All good.
      return false;
    }

    // We're over the limit, let's evict stuff.
    if (lock->empty()) {
      // Nothing to evict.
      return current > options.hardLimit;
    }

    Entry& entry = lock->front();
    entry.state = NOT_IN_CACHE;
    lock->remove(entry);
    KJ_ASSERT_NONNULL(entry.cache).evictEntry(lock, entry);
  }
}

void ActorCache::touchEntry(Lock& lock, Entry& entry, const ReadOptions& options) {
  if (!options.noCache) {
    if (entry.state == CLEAN || entry.state == STALE) {
      entry.state = CLEAN;
      lock->remove(entry);
      lock->add(entry);
    }

    // If this is a dirty entry previously marked no-cache, remove that mark. This results in the
    // same end state as if the entry had been flushed and evicted before the read -- it would have
    // been read back, and then into cache.
    entry.noCache = false;
  }
}

void ActorCache::evictEntry(Lock& lock, Entry& entry) {
  auto& map = currentValues.get(lock);
  auto ordered = map.ordered();
  auto iter = map.seek(entry.key);

  KJ_ASSERT(iter != ordered.end() && iter->get() == &entry);

  // If the previous entry has gapIsKnownEmpty, we need to set that false, because when we delete
  // this entry, the previous entry's "gap" will now extend to the *next* entry. We definitely know
  // that that the new gap is non-empty because we're evicting an entry inside that very gap.
  //
  // TODO(perf): Maybe we should instead replace the evicted entry with an END_GAP entry in this
  //   case? The problem is, when the app accesses a key in the gap, the LRU time of the previous
  //   entry gets bumped, but the _next_ entry does not get bumped. Hence these acccesses won't
  //   prevent the next entry from being evicted, and when it is, the gap effectively gets evicted
  //   too, leading to a cache miss on a key that had been recently accessed. This is a pretty
  //   obscure scenario, though, and after one cache miss the key would then be in cache again.
  if (iter != ordered.begin()) {
    auto prev = iter;
    --prev;
    prev->get()->gapIsKnownEmpty = false;
  }

  // If this entry has gapIsKnownEmpty and the next entry is END_GAP, we should delete the
  // END_GAP, because it no longer serves a purpose.
  kj::Maybe<KeyPtr> eraseLater;
  if (iter->get()->gapIsKnownEmpty) {
    auto next = iter;
    ++next;
    if (next != ordered.end() && next->get()->state == END_GAP) {
      // Erasing invalidates iterators, so we have to delay...
      eraseLater = next->get()->key;
    }
  }

  map.erase(*iter);

  KJ_IF_MAYBE(k, eraseLater) {
    map.eraseMatch(*k);
  }
}

void ActorCache::verifyConsistencyForTest() {
  auto lock = lru.cleanList.lockExclusive();
  currentValues.get(lock).verify();  // verify the table's BTreeIndex
  bool prevGapIsKnownEmpty = false;
  kj::Maybe<kj::StringPtr> prevKey = nullptr;
  for (auto& entry: currentValues.get(lock).ordered()) {
    KJ_IF_MAYBE(p, prevKey) {
      KJ_ASSERT(entry->key > *p, "keys out of order?", *p, entry->key);
    }
    prevKey = entry->key;

    switch (entry->state) {
      case DIRTY:
      case FLUSHING:
        KJ_ASSERT(entry->link.isLinked());
        break;
      case CLEAN:
      case STALE:
        KJ_ASSERT(entry->link.isLinked());
        KJ_ASSERT(!(prevGapIsKnownEmpty && entry->gapIsKnownEmpty && entry->value == nullptr),
            "clean negative entry in the middle of a known-empty gap is redundant", entry->key);
        break;
      case END_GAP:
        // Verify that this actually marks the end of a known-empty gap.
        KJ_ASSERT(prevGapIsKnownEmpty,
            "END_GAP entry should only appear after known-empty gap", entry->key);
        KJ_ASSERT(!entry->gapIsKnownEmpty,
            "END_GAP entry can't be followed by known-empty gap", entry->key);
        break;
      case NOT_IN_CACHE:
        KJ_FAIL_ASSERT("NOT_IN_CACHE entry should not appear in map", entry->key);
        break;
    }

    prevGapIsKnownEmpty = entry->gapIsKnownEmpty;
  }
}

// =======================================================================================
// read operations

kj::OneOf<kj::Maybe<ActorCache::Value>, kj::Promise<kj::Maybe<ActorCache::Value>>>
    ActorCache::get(Key key, ReadOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  auto lock = lru.cleanList.lockExclusive();
  KJ_IF_MAYBE(entry, findInCache(lock, key, options)) {
    return entry->get()->value.map([&](ValuePtr value) {
      return value.attach(kj::mv(*entry));
    });
  } else {
    return scheduleStorageRead([key=KeyPtr(key)](rpc::ActorStorage::Operations::Client client) {
      auto req = client.getRequest(
          capnp::MessageSize { 4 + key.size() / sizeof(capnp::word), 0 });
      req.setKey(key.asBytes());
      return req.send().dropPipeline();
    }).then([this,key=kj::mv(key),options]
            (capnp::Response<rpc::ActorStorage::Operations::GetResults> response) mutable
          -> kj::Maybe<ActorCache::Value> {
      kj::Maybe<capnp::Data::Reader> value;
      if (response.hasValue()) {
        value = response.getValue();
      }
      auto lock = lru.cleanList.lockExclusive();
      auto entry = addReadResultToCache(lock, kj::mv(key), value, options);
      evictOrOomIfNeeded(lock);
      return entry->value.map([&](ValuePtr value) {
        return value.attach(kj::mv(entry));
      });
    });
  }
}

class ActorCache::GetMultiStreamImpl final: public rpc::ActorStorage::ListStream::Server {
public:
  GetMultiStreamImpl(ActorCache& cache, kj::Vector<kj::Own<Entry>> cachedEntries,
                     kj::Vector<Key> keysToFetchParam,
                     kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller,
                     const ReadOptions& options)
      : cache(cache), cachedEntries(kj::mv(cachedEntries)),
        keysToFetch(kj::mv(keysToFetchParam)), fulfiller(kj::mv(fulfiller)),
        options(options) {
    nextExpectedKey = keysToFetch.begin();
  }

  kj::Promise<void> values(ValuesContext context) override {
    if (!fulfiller->isWaiting()) {
      // The original caller stopped listening. Try to cancel the stream by throwing.
      return KJ_EXCEPTION(DISCONNECTED, "canceled");
    }

    auto lock = cache.lru.cleanList.lockExclusive();
    auto params = context.getParams();
    kj::String prevKey = nullptr;
    for (auto kv: params.getList()) {
      KJ_ASSERT(kv.hasValue());  // values that don't exist aren't listed!
      KJ_ASSERT(nextExpectedKey != keysToFetch.end());

      // TODO(perf): This copy of the key is not really needed, we use the key from `keysToFetch`
      //   instead. But the capnp representation is a byte array which isn't null-terminated
      //   which would make the code difficult below.
      auto key = kj::str(kv.getKey().asChars());

      KJ_ASSERT(key >= prevKey, "storage returned keys in non-sorted order?");

      // Find matching key in keysToFetch, possibly marking missing keys as absent.
      for (;;) {
        if (nextExpectedKey == keysToFetch.end() || key < *nextExpectedKey) {
          // This may be a duplicate due to a retry. Ignore it.
          break;
        } else if (key == *nextExpectedKey) {
          fetchedEntries.add(cache.addReadResultToCache(
              lock, kj::mv(*nextExpectedKey), kv.getValue(), options));
          ++nextExpectedKey;
          break;
        }

        // It seems the list results have moved past `nextExpectedKey`, meaning it wasn't present
        // on disk. Write a negative cache entry.
        cache.addReadResultToCache(lock, kj::mv(*nextExpectedKey), nullptr, options);
        ++nextExpectedKey;
      }

      if (nextExpectedKey == keysToFetch.end()) {
        fulfill();
      }

      prevKey = kj::mv(key);
    }
    cache.evictOrOomIfNeeded(lock);
    return kj::READY_NOW;
  }

  kj::Promise<void> end(EndContext context) override {
    if (!fulfiller->isWaiting()) {
      // Just ignore end() if we've already stopped waiting.
      return kj::READY_NOW;
    }

    if (nextExpectedKey < keysToFetch.end()) {
      // Some trailing keys weren't seen, better mark them as not present.
      auto lock = cache.lru.cleanList.lockExclusive();
      while (nextExpectedKey < keysToFetch.end()) {
        cache.addReadResultToCache(lock, kj::mv(*nextExpectedKey++), nullptr, options);
      }
      cache.evictOrOomIfNeeded(lock);
    }

    fulfill();

    return kj::READY_NOW;
  }

  void fulfill() {
    // We return results in sorted order. You might argue that it could make sense to return
    // results in the same order as the keys were originally specified. Even though we return
    // a `Map` in JavaScript, the iteration order of a `Map` is defined to be the order of
    // insertion, therefore the order in which we return results here is actually observable by
    // the application. Trying to match the input order, however, almost certainly wouldn't be
    // useful to apps. The only plausible way it could be useful is if the app could do e.g.
    // `[...map.values()]` and end up with an array of values that exactly corresponds to the
    // input array of keys. However, it won't exactly correspond for two reasons:
    // - Keys that weren't present on disk aren't listed at all. To meaningfully change this,
    //   we would need to say that the Map object returned to JavaScript would contain entries
    //   even for missing keys, where the value is explicitly set to `undefined`. However,
    //   changing that would be a breaking change.
    // - Keys that were listed twice in the input list won't be reported twice. This is an
    //   inherent limitation of the fact that we return a `Map`.
    //
    // Hence, applications that tried to depend on this ordering would be shooting themselves
    // in the foot. We do, however, want to produce a consistent ordering for reproducibility's
    // sake, but any consistent ordering will due. Sorted order is as good as anything else, and
    // happens to be nice and easy for us.
    fulfiller->fulfill(GetResultList(
        kj::mv(cachedEntries), kj::mv(fetchedEntries), GetResultList::FORWARD));
  }

  void cancel() {
    // Indicates that the operation is being canceled. Proactively drops all entries. This
    // is important because the destructor of an `Entry` updates the cache's accounting of memory
    // usage, so it's important that an `Entry` cannot be held beyond the lifetime of the cache
    // itself.
    KJ_ASSERT(!fulfiller->isWaiting());  // proves further RPCs will be ignored
    cachedEntries.clear();
    fetchedEntries.clear();
  }

  ActorCache& cache;
  kj::Vector<kj::Own<Entry>> cachedEntries;
  kj::Vector<kj::Own<Entry>> fetchedEntries;
  kj::Vector<Key> keysToFetch;
  Key* nextExpectedKey;
  kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller;
  ReadOptions options;
};

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::get(kj::Array<Key> keys, ReadOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  std::sort(keys.begin(), keys.end());

  kj::Vector<kj::Own<Entry>> cachedEntries(keys.size());
  // Entries satisfying the requested keys.

  kj::Vector<Key> keysToFetch(keys.size());
  // Keys that were not satisfied from cache.

  capnp::MessageSize sizeHint { 4, 1 };

  {
    auto lock = lru.cleanList.lockExclusive();
    for (auto& key: keys) {
      KJ_IF_MAYBE(entry, findInCache(lock, key, options)) {
        cachedEntries.add(kj::mv(*entry));
      } else {
        // +1 word for padding, +1 word for the pointer in the key list.
        sizeHint.wordCount += key.size() / sizeof(capnp::word) + 2;
        keysToFetch.add(kj::mv(key));
      }
    }
  }

  if (keysToFetch.empty()) {
    // All satisfied, return early.
    return GetResultList(kj::mv(cachedEntries), {}, GetResultList::FORWARD);
  }

  auto paf = kj::newPromiseAndFulfiller<GetResultList>();
  auto streamServer = kj::heap<GetMultiStreamImpl>(
      *this, kj::mv(cachedEntries), kj::mv(keysToFetch), kj::mv(paf.fulfiller), options);
  auto& streamServerRef = *streamServer;

  rpc::ActorStorage::ListStream::Client streamClient = kj::mv(streamServer);

  auto sendPromise = scheduleStorageRead(
      [sizeHint,streamClient,&streamServerRef]
      (rpc::ActorStorage::Operations::Client client) mutable -> kj::Promise<void> {
    if (streamServerRef.nextExpectedKey == streamServerRef.keysToFetch.end()) {
      // No more keys expected, must have finished listing on a previous try.
      return kj::READY_NOW;
    }
    auto req = client.getMultipleRequest(sizeHint);
    auto keysToFetch = kj::arrayPtr(streamServerRef.nextExpectedKey,
                                    streamServerRef.keysToFetch.end());
    auto list = req.initKeys(keysToFetch.size());
    for (auto i: kj::indices(keysToFetch)) {
      list.set(i, keysToFetch[i].asBytes());
    }
    req.setStream(streamClient);
    return req.send().ignoreResult();
  });

  // Wait on the RPC only until stream.end() is called, then report the results. We prevent
  // `stream` from being destroyed until we have a result so that if the RPC throws an exception,
  // we don't accidentally report "PromiseFulfiller not fulfilled" instead of the exception.
  auto promise = sendPromise
      .then([&streamServerRef]() -> kj::Promise<ActorCache::GetResultList> {
    if (streamServerRef.fulfiller->isWaiting()) {
      return KJ_EXCEPTION(FAILED, "getMultiple() never called stream.end()");
    } else {
      // We'll be canceled momentarily...
      return kj::NEVER_DONE;
    }
  });
  return paf.promise.exclusiveJoin(kj::mv(promise))
      .attach(kj::defer([client = kj::mv(streamClient), &streamServerRef]() {
    streamServerRef.cancel();
  }));
}

kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorCache::getAlarm(
    ReadOptions options) {
  options.noCache = options.noCache || lru.options.noCache;

  // If in cache return time
  // Else schedule alarm read
  KJ_SWITCH_ONEOF(currentAlarmTime) {
    KJ_CASE_ONEOF(entry, ActorCache::DeferredAlarmDelete) {
        // An alarm handler is currently running, and a new alarm time has not been set yet.
        // We need to return that there is no alarm.
        return kj::Maybe<kj::Date>(nullptr);
    }
    KJ_CASE_ONEOF(entry, ActorCache::KnownAlarmTime) {
      return entry.time;
    }
    KJ_CASE_ONEOF(_, ActorCache::UnknownAlarmTime) {
      return scheduleStorageRead([](rpc::ActorStorage::Operations::Client client) {
        auto req = client.getAlarmRequest();
        return req.send().dropPipeline();
      }).then([this, options]
              (capnp::Response<rpc::ActorStorage::Operations::GetAlarmResults> response) mutable
            -> kj::Maybe<kj::Date> {
        auto scheduledTimeMs = response.getScheduledTimeMs();
        auto result = [&]() -> kj::Maybe<kj::Date> {
          if (scheduledTimeMs == 0) {
            return nullptr;
          } else {
            return scheduledTimeMs * kj::MILLISECONDS + kj::UNIX_EPOCH;
          }
        }();

        if (!options.noCache && currentAlarmTime.is<UnknownAlarmTime>()) {
          // If we don't end up in this branch, the time that's already in currentAlarmTime must
          // be at least as fresh as the one we just read.
          //
          // If it was created by a setAlarm(), then it is actually fresher. If it was created
          // by a concurrent getAlarm(), then it should be exactly the same time.

          currentAlarmTime = ActorCache::KnownAlarmTime {
            ActorCache::KnownAlarmTime::Status::CLEAN,
            result
          };
        }

        return result;
      });
    }
  }

  KJ_UNREACHABLE;
}

// -----------------------------------------------------------------------------

namespace {
// To simplify the handling of Maybe<Key> representing the end point of a list range, we define
// these operators to allow comparison between a Key and a Maybe<Key>, where a null Maybe<Key>
// sorts after all other keys.

inline bool operator==(const ActorCache::Key& a, const kj::Maybe<ActorCache::Key>& b) {
  KJ_IF_MAYBE(bb, b) {
    return a == *bb;
  } else {
    return false;
  }
}
inline bool operator<(const ActorCache::Key& a, const kj::Maybe<ActorCache::Key>& b) {
  KJ_IF_MAYBE(bb, b) {
    return a < *bb;
  } else {
    return true;
  }
}
inline bool operator>=(const ActorCache::Key& a, const kj::Maybe<ActorCache::Key>& b) {
  KJ_IF_MAYBE(bb, b) {
    return a >= *bb;
  } else {
    return false;
  }
}
inline bool operator>(const ActorCache::Key& a, const kj::Maybe<ActorCache::KeyPtr>& b) {
  KJ_IF_MAYBE(bb, b) {
    return a > *bb;
  } else {
    return false;
  }
}

inline auto seekOrEnd(auto& map, kj::Maybe<ActorCache::KeyPtr> key) {
  KJ_IF_MAYBE(k, key) {
    return map.seek(*k);
  } else {
    return map.ordered().end();
  }
}

}  // namespace

class ActorCache::ForwardListStreamImpl final: public rpc::ActorStorage::ListStream::Server {
public:
  ForwardListStreamImpl(ActorCache& cache, Key beginKey, kj::Maybe<Key> endKey,
                        kj::Vector<kj::Own<Entry>> cachedEntries,
                        kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller,
                        kj::Maybe<uint> originalLimit, kj::Maybe<uint> adjustedLimit,
                        bool beginKeyIsKnown, const ReadOptions& options)
      : cache(cache), beginKey(kj::mv(beginKey)), endKey(kj::mv(endKey)),
        cachedEntries(kj::mv(cachedEntries)), fulfiller(kj::mv(fulfiller)),
        originalLimit(originalLimit), adjustedLimit(adjustedLimit),
        beginKeyIsKnown(beginKeyIsKnown), options(options) {}

  kj::Promise<void> values(ValuesContext context) override {
    if (!fulfiller->isWaiting()) {
      // The original caller stopped listening. Try to cancel the stream by throwing.
      return KJ_EXCEPTION(DISCONNECTED, "canceled");
    }

    {
      auto lock = cache.lru.cleanList.lockExclusive();
      auto list = context.getParams().getList();

      bool insertedAny = false;

      for (auto kv: list) {
        Key key = kj::str(kv.getKey().asChars());

        if (!beginKeyIsKnown) {
          if (key != beginKey) {
            // This is the first set of results we've received, and it does not include the start
            // point of the list. Therefore, we should insert an entry with a null value, to make
            // sure the whole range can be marked as empty. We'll end up marking this entry as
            // part of markGapsEmpty(), later.
            markBeginAsEmpty(lock);
          }
        } else {
          if (key <= beginKey) {
            // Out-of-order result. This is probably the result of restarting the list operation
            // due to a disconnect. We assume this is actually a duplicate of a result we
            // received earlier. Ignore it.
            continue;
          }
        }

        KJ_ASSERT(kv.hasValue());  // values that don't exist aren't listed!
        auto entry = cache.addReadResultToCache(lock, kj::mv(key), kv.getValue(), options);
        fetchedEntries.add(kj::mv(entry));
        insertedAny = true;
      }

      if (insertedAny) {
        // Update `gapIsKnownEmpty` on the whole range.
        cache.markGapsEmpty(lock, beginKey, fetchedEntries.back()->key.asPtr(), options);
        beginKey = cloneKey(fetchedEntries.back()->key);
        beginKeyIsKnown = true;
      }

      cache.evictOrOomIfNeeded(lock);
    }

    if (fetchedEntries.size() >= adjustedLimit.orDefault(kj::maxValue)) {
      // Oh we're already done.
      fulfill();
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> end(EndContext context) override {
    if (!fulfiller->isWaiting()) {
      // Just ignore end() if we've already stopped waiting. In particular this happens in
      // limit requests that reach the limit -- the last call to values() will have already
      // fulfilled the fulfiller.
      return kj::READY_NOW;
    }

    // Mark the rest of the range as empty.
    {
      auto lock = cache.lru.cleanList.lockExclusive();

      if (!beginKeyIsKnown) {
        // We received no results at all, so the start of the list is definitely not in storage.
        markBeginAsEmpty(lock);
      }

      if (fetchedEntries.size() < adjustedLimit.orDefault(kj::maxValue)) {
        // We didn't reach the limit, so the rest of the range must be empty.
        cache.markGapsEmpty(lock, beginKey, endKey, options);
      }

      cache.evictOrOomIfNeeded(lock);
    }

    fulfill();

    return kj::READY_NOW;
  }

  void fulfill() {
    fulfiller->fulfill(GetResultList(kj::mv(cachedEntries), kj::mv(fetchedEntries),
                                     GetResultList::FORWARD, originalLimit));
  };

  void markBeginAsEmpty(Lock& lock) {
    // Mark the start of the list operation will a null entry, because we did not see it listed.
    //
    // Note that this insertion attempt will be ignored in two cases:
    // 1. An entry already exists with this key, perhaps as the result of a put(). This is
    //    fine, because the existing entry means we have something to mark.
    // 2. The entry doesn't exist, but the previous entry has `gapIsKnownEmpty = true`, and
    //    so the insertion of a new null entry is ignored for being redundant. This case is
    //    fine too, as the gap is already marked. Our markGapsEmpty() call will start with the
    //    following entry.
    cache.addReadResultToCache(lock, cloneKey(beginKey), nullptr, options);
  }

  void cancel() {
    // Indicates that the operation is being canceled. Proactively drops all entries. This
    // is important because the destructor of an `Entry` updates the cache's accounting of memory
    // usage, so it's important that an `Entry` cannot be held beyond the lifetime of the cache
    // itself.
    KJ_ASSERT(!fulfiller->isWaiting());  // proves further RPCs will be ignored
    cachedEntries.clear();
    fetchedEntries.clear();
  }

  ActorCache& cache;

  Key beginKey;
  // Either:
  // - No prefix of the list is known yet, and `beginKey` is the original begin point passed to
  //   list().
  // - Some prefix is already satisfied, either from cache or from a previous batch of results
  //   streamed from storage, and `beginKey` is the key of the last known entry in this prefix.

  kj::Maybe<Key> endKey;
  // The end of the list range, as originally passed to list().

  kj::Vector<kj::Own<Entry>> cachedEntries;
  // Entries we gathered from cache.

  kj::Vector<kj::Own<Entry>> fetchedEntries;
  // Entries that have streamed in from disk.

  kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller;
  // Fulfiller for the final results.

  kj::Maybe<uint> originalLimit;
  // The original requested limit, if any.

  kj::Maybe<uint> adjustedLimit;
  // The limit we sent to storage.

  bool beginKeyIsKnown;
  // Does `beginKey` point to a key where we already know the associated value? This is
  // especially true when `beginKey` points to the last entry of a previous batch received via
  // a call to `values()`.

  ReadOptions options;
};

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::list(Key beginKey, kj::Maybe<Key> endKey,
                     kj::Maybe<uint> limit, ReadOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  // We start by scanning the cache for entries satisfying the list range. If we can fully satisfy
  // the list using these, then we're done! Otherwise, we make a storage request to get the rest.
  // When the storage request produces results, we must discard any that conflict with what was
  // in cache before hand, since what's in cache could have come from a put() that wasn't flushed
  // yet. However, we need to be careful NOT to use any entries that were put() *after* the list()
  // operation started.

  kj::Vector<kj::Own<Entry>> cachedEntries;
  size_t positiveCount = 0;  // number of positive entries in `cachedEntries`
  if (limit.orDefault(kj::maxValue) == 0 || beginKey >= endKey) {
    // No results in these cases, just return.
    return ActorCache::GetResultList(kj::mv(cachedEntries), {}, GetResultList::FORWARD);
  }

  uint limitAdjustment = 0;
  // When requesting to storage, we need to adjust the limit to increase it by the number of cached
  // negative entries in the range, since each of those negative entries could potentially negate a
  // positive entry read from disk.

  auto lock = lru.cleanList.lockExclusive();
  auto& map = currentValues.get(lock);
  auto ordered = map.ordered();

  kj::Maybe<KeyPtr> storageListStart;
  // If we must do a storage operation, what key shall it start at?
  //
  // Note that we never do more than one storage operation, even if we have a patchwork of cache
  // entries matching different subsets of the list. Trying to split the operation into multiple
  // smaller list operations to avoid re-listing things we already know seems like too much work to
  // be worth it. So, we only track the first key which we know needs to be listed, and then we
  // list the rest of the space from there.

  bool storageListStartIsKnown = false;
  // Does `storageListStart` point to a key for which we already know the value? If so we can
  // avoid listing that key specifically.

  uint knownPrefixSize = 0;
  // How many keys were matched from cache before (and not including) `storageListStart`? We will
  // use this to reduce the `limit` we pass in the storage op (if there is one).

  // Let's iterate over the cache starting from `beginKey`.
  auto iter = map.seek(beginKey);

  // We need some special logic to handle the starting point with regard to gaps.
  if (iter != ordered.end() && iter->get()->key == beginKey) {
    // There is an entry specifically for `beginKey`, so we'll start there.
  } else {
    // `beginKey` does not match an entry, but we can check if it is in a known-empty gap.
    if (iter == ordered.begin()) {
      // No, because there is no previous entry. Oh well. We will have to start the storage list
      // from `beginKey`.
      storageListStart = beginKey;
      storageListStartIsKnown = false;
    } else {
      // There is a previous key in cache, let's take a look.
      auto prev = iter;
      --prev;
      if (prev->get()->gapIsKnownEmpty) {
        // `beginKey` is in a known-empty gap, so we know that this key simply doesn't exist in
        // storage.
      } else {
        // We don't know if `beginKey` exists in storage so we'll have to start the storage list
        // there.
        storageListStart = beginKey;
        storageListStartIsKnown = false;
      }
    }
  }

  // Now we can start scanning normally. We need to scan entries within the list range to build
  // a list of possible results, as well as to determine whether we need to do a storage request.
  // Even if we end up having to go to disk to find more data, we don't need to scan more than
  // `limit` entries from cache because any entries beyond that couldn't possibly end up in the
  // final results anyway.
  //
  // Note that we must keep scanning the cache *even if* we've seen an empty gap and
  // `storageListStart` is non-null. This is because our results must include recent put()s, which
  // may still be DIRTY so won't be returned when we list the database. Later on we'll merge the
  // entries we find in cache with those we get from disk.
  for (; iter != ordered.end() && iter->get()->key < endKey &&
         positiveCount < limit.orDefault(kj::maxValue); ++iter) {
    Entry& entry = **iter;

    switch (entry.state) {
      case CLEAN:
      case STALE:
      case DIRTY:
      case FLUSHING:
        touchEntry(lock, entry, options);

        // Note that we need to add even negative entries to `cachedEntries` so that they override
        // whatever we read from storage later. However, they should not count against the limit.
        cachedEntries.add(kj::atomicAddRef(entry));
        if (entry.value == nullptr) {
          if (storageListStart != nullptr && (entry.state == DIRTY || entry.state == FLUSHING)) {
            // This negative entry could negate something read from storage later, so we need to
            // increase the storage list limit.
            ++limitAdjustment;
          }
        } else {
          ++positiveCount;
          if (storageListStart == nullptr) {
            ++knownPrefixSize;
          }
        }
        break;

      case END_GAP:
        // Ignore entry that exists only to mark a previous list range.
        break;
      case NOT_IN_CACHE:
        KJ_FAIL_ASSERT("NOT_IN_CACHE entry should have been removed from map");
    }

    if (storageListStart == nullptr && !entry.gapIsKnownEmpty) {
      // The gap after this entry is not cached so we'll have to start our list operation here.
      storageListStart = entry.key;
      storageListStartIsKnown = entry.state != END_GAP;
    }
  }

  if (storageListStart == nullptr || knownPrefixSize >= limit.orDefault(kj::maxValue)) {
    // We fully satisfied the list operation from cache.
    return GetResultList(kj::mv(cachedEntries), {}, GetResultList::FORWARD, limit);
  }

  auto adjustedLimit = limit.map([&](uint orig) {
    return orig + limitAdjustment - knownPrefixSize;
  });

  auto paf = kj::newPromiseAndFulfiller<GetResultList>();
  auto streamServer = kj::heap<ForwardListStreamImpl>(
      *this, cloneKey(KJ_ASSERT_NONNULL(storageListStart)), kj::mv(endKey),
      kj::mv(cachedEntries), kj::mv(paf.fulfiller), limit, adjustedLimit,
      storageListStartIsKnown, options);
  auto& streamServerRef = *streamServer;

  rpc::ActorStorage::ListStream::Client streamClient = kj::mv(streamServer);

  auto sendPromise = scheduleStorageRead(
      [&streamServerRef,streamClient]
      (rpc::ActorStorage::Operations::Client client) mutable -> kj::Promise<void> {
    auto req = client.listRequest(capnp::MessageSize {
        8 + streamServerRef.beginKey.size() / sizeof(capnp::word) +
        streamServerRef.endKey
            .map([](KeyPtr k) { return k.size() / sizeof(capnp::word); })
            .orDefault(0), 1 });

    if (streamServerRef.beginKeyIsKnown) {
      // `streamServerRef.beginKey` points to a key for which we already know the value, either
      // because it was already in cache when we started, or because we are retrying and a previous
      // call to `values()` produced this key. Querying it again would be redundant. But, list
      // operations are inclusive of the start key. So, we compute the successor of the start key,
      // which is the key with a zero byte appended.
      auto buffer = req.initStart(streamServerRef.beginKey.size() + 1);
      memcpy(buffer.begin(), streamServerRef.beginKey.begin(), buffer.size() - 1);
      // Technically capnp is zero-initialized so this is redundant, but just for safety and
      // clarity...
      buffer[buffer.size() - 1] = 0;
    } else {
      if (streamServerRef.beginKey.size() > 0) {
        req.setStart(streamServerRef.beginKey.asBytes());
      }
    }

    KJ_IF_MAYBE(e, streamServerRef.endKey) {
      req.setEnd(e->asBytes());
    }

    KJ_IF_MAYBE(l, streamServerRef.adjustedLimit) {
      if (streamServerRef.fetchedEntries.size() >= *l) {
        // Oh it turns out we actually satisfied the limit already so we don't actually have to
        // retry. The fulfiller would have already been fulfilled.
        return kj::READY_NOW;
      }
      req.setLimit(*l - streamServerRef.fetchedEntries.size());
    }

    req.setStream(streamClient);
    return req.send().ignoreResult();
  });

  // Wait on the RPC only until stream.end() is called, then report the results. We prevent
  // `stream` from being destroyed until we have a result so that if the RPC throws an exception,
  // we don't accidentally report "PromiseFulfiller not fulfilled" instead of the exception.
  auto promise = sendPromise
      .then([&streamServerRef]() -> kj::Promise<ActorCache::GetResultList> {
    if (streamServerRef.fulfiller->isWaiting()) {
      return KJ_EXCEPTION(FAILED, "list() never called stream.end()");
    } else {
      // We'll be canceled momentarily...
      return kj::NEVER_DONE;
    }
  });

  return paf.promise.exclusiveJoin(kj::mv(promise))
      .attach(kj::defer([client = kj::mv(streamClient), &streamServerRef]() {
    streamServerRef.cancel();
  }));
}

// -----------------------------------------------------------------------------

class ActorCache::ReverseListStreamImpl final: public rpc::ActorStorage::ListStream::Server {
public:
  ReverseListStreamImpl(ActorCache& cache, Key beginKey, kj::Maybe<Key> endKey,
                        kj::Vector<kj::Own<Entry>> cachedEntries,
                        kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller,
                        kj::Maybe<uint> originalLimit, kj::Maybe<uint> adjustedLimit,
                        ReadOptions options)
      : cache(cache), beginKey(kj::mv(beginKey)), endKey(kj::mv(endKey)),
        cachedEntries(kj::mv(cachedEntries)), fulfiller(kj::mv(fulfiller)),
        originalLimit(originalLimit), adjustedLimit(adjustedLimit), options(options) {}

  kj::Promise<void> values(ValuesContext context) override {
    if (!fulfiller->isWaiting()) {
      // The original caller stopped listening. Try to cancel the stream by throwing.
      return KJ_EXCEPTION(DISCONNECTED, "canceled");
    }

    {
      auto lock = cache.lru.cleanList.lockExclusive();
      auto list = context.getParams().getList();

      bool insertedAny = false;

      for (auto kv: list) {
        Key key = kj::str(kv.getKey().asChars());

        if (key >= endKey) {
          // Out-of-order result. This is probably the result of restarting the list operation
          // due to a disconnect. We assume this is actually a duplicate of a result we
          // received earlier. Ignore it.
          continue;
        }

        KJ_ASSERT(kv.hasValue());  // values that don't exist aren't listed!
        auto entry = cache.addReadResultToCache(lock, kj::mv(key), kv.getValue(), options);
        fetchedEntries.add(kj::mv(entry));
        insertedAny = true;
      }

      if (insertedAny) {
        // Update `gapIsKnownEmpty` on the whole range.
        cache.markGapsEmpty(lock, fetchedEntries.back()->key, endKey, options);
        endKey = cloneKey(fetchedEntries.back()->key);
      }

      cache.evictOrOomIfNeeded(lock);
    }

    if (fetchedEntries.size() >= adjustedLimit.orDefault(kj::maxValue) || beginKey == endKey) {
      // Oh we're already done.
      fulfill();
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> end(EndContext context) override {
    if (!fulfiller->isWaiting()) {
      // Just ignore end() if we've already stopped waiting. In particular this happens in
      // limit requests that reach the limit, or when we see an entry matching the beginning
      // key of the list range -- in both cases, the last call to values() will have already
      // fulfilled the fulfiller.
      return kj::READY_NOW;
    }

    // Mark the rest of the range as empty.
    {
      auto lock = cache.lru.cleanList.lockExclusive();

      if (fetchedEntries.size() < adjustedLimit.orDefault(kj::maxValue)) {
        // We didn't reach the limit, so the rest of the range must be empty.

        // We may need to insert a negative entry at the beginning of the list range, since we
        // didn't see it, implying it's not present on disk. addResultToCache() will conveniently
        // avoid adding anything if it turns out this is already in a known-empty gap.
        auto beginEntry = cache.addReadResultToCache(lock, cloneKey(beginKey), nullptr, options);

        // And we need to mark gaps empty from there to the final entry we actually saw.
        cache.markGapsEmpty(lock, beginEntry->key, endKey, options);
      }

      cache.evictOrOomIfNeeded(lock);
    }

    fulfill();

    return kj::READY_NOW;
  }

  void fulfill() {
    fulfiller->fulfill(GetResultList(
        kj::mv(cachedEntries), kj::mv(fetchedEntries), GetResultList::REVERSE, originalLimit));
  }

  void cancel() {
    // Indicates that the operation is being canceled. Proactively drops all entries. This
    // is important because the destructor of an `Entry` updates the cache's accounting of memory
    // usage, so it's important that an `Entry` cannot be held beyond the lifetime of the cache
    // itself.
    KJ_ASSERT(!fulfiller->isWaiting());  // proves further RPCs will be ignored
    cachedEntries.clear();
    fetchedEntries.clear();
  }

  ActorCache& cache;

  Key beginKey;
  // The beginning of the list range, as originally passed to list().

  kj::Maybe<Key> endKey;
  // Either:
  // - No suffix of the list is known yet, and `endKey` is the original end point passed to
  //   list().
  // - Some suffix is already satisfied , either from cache or from a previous batch of results
  //   streamed from storage, and `endKey` is the key of the first known entry in this suffix.

  kj::Vector<kj::Own<Entry>> cachedEntries;
  // Entries we gathered from cache.

  kj::Vector<kj::Own<Entry>> fetchedEntries;
  // Entries that have streamed in from disk.

  kj::Own<kj::PromiseFulfiller<GetResultList>> fulfiller;
  // Fulfiller for the final results.

  kj::Maybe<uint> originalLimit;
  // The original requested limit, if any.

  kj::Maybe<uint> adjustedLimit;
  // The limit we sent to storage.

  ReadOptions options;
};

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::listReverse(Key beginKey, kj::Maybe<Key> endKey,
                            kj::Maybe<uint> limit, ReadOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  // Alas, everything needs to be done slightly differently when listing in reverse. This function
  // is an adjusted version of the previous function.

  kj::Vector<kj::Own<Entry>> cachedEntries;
  size_t positiveCount = 0;  // number of positive entries in `cachedEntries`
  if (limit.orDefault(kj::maxValue) == 0 || beginKey >= endKey) {
    // No results in these cases, just return.
    return ActorCache::GetResultList(kj::mv(cachedEntries), {}, GetResultList::REVERSE);
  }

  uint limitAdjustment = 0;
  // When requesting to storage, we need to adjust the limit to increase it by the number of cached
  // negative entries in the range, since each of those negative entries could potentially negate a
  // positive entry read from disk.

  auto lock = lru.cleanList.lockExclusive();
  auto& map = currentValues.get(lock);
  auto ordered = map.ordered();

  kj::Maybe<KeyPtr> storageListEnd;
  // If we must do a storage operation, what key shall it end at?
  //
  // As an extra hack, if the Maybe is non-null but the KeyPtr is null, this indicates there is
  // no end. It's impossible for storageListEnd to point at a null key and intend this to mean that
  // the end should be the empty-string key because this would suggest an empty list range.

  uint knownSuffixSize = 0;
  // How many keys were matched from cache after (and including) `storageListEnd`? We will
  // use this to reduce the `limit` we pass in the storage op (if there is one).

  // Let's iterate backwards over the cache starting from `endKey`. Iterating backwards is a
  // bit mind-bendy.
  //
  // Note that we must keep scanning the cache *even if* we've seen an empty gap and
  // `storageListEnd` is non-null. This is because our results must include recent put()s, which
  // may still be DIRTY so won't be returned when we list the database. Later on we'll merge the
  // entries we find in cache with those we get from disk.
  KeyPtr nextKey = endKey.orDefault(nullptr);  // "the last key we saw in backwards order"
  for (auto iter = seekOrEnd(map, endKey); positiveCount < limit.orDefault(kj::maxValue); ) {
    if (iter == ordered.begin()) {
      // No earlier entries, treat same as if previous entry were before beginKey and had
      // gapIsKnownEmpty = false.
      if (storageListEnd == nullptr) storageListEnd = nextKey;
      break;
    }

    // Step backwards.
    --iter;
    auto& entry = **iter;

    // If the gap after this entry is not known empty, then we've exhausted our known-suffix and
    // will need to cover this gap using a storage RPC.
    if (storageListEnd == nullptr && !entry.gapIsKnownEmpty) {
      storageListEnd = nextKey;
    }

    if (entry.key < beginKey) {
      // We've traversed past the beginning of our range so exit the loop here.
      break;
    }

    switch (entry.state) {
      case CLEAN:
      case STALE:
      case DIRTY:
      case FLUSHING:
        touchEntry(lock, entry, options);

        // Note that we need to add even negative entries to `cachedEntries` so that they override
        // whatever we read from storage later. However, they should not count against the limit.
        cachedEntries.add(kj::atomicAddRef(entry));
        if (entry.value == nullptr) {
          if (storageListEnd != nullptr && (entry.state == DIRTY || entry.state == FLUSHING)) {
            // This negative entry could negate something read from storage later, so we need to
            // increase the storage list limit.
            ++limitAdjustment;
          }
        } else {
          ++positiveCount;
          if (storageListEnd == nullptr) {
            ++knownSuffixSize;
          }
        }
        break;

      case END_GAP:
        // Ignore entry that exists only to mark a previous list range.
        break;
      case NOT_IN_CACHE:
        KJ_FAIL_ASSERT("NOT_IN_CACHE entry should have been removed from map");
    }

    if (entry.key == beginKey) {
      // We've traversed through the beginning of our range so exit the loop here.
      break;
    }

    nextKey = entry.key;
  }

  if (storageListEnd == nullptr || knownSuffixSize >= limit.orDefault(kj::maxValue)) {
    // We fully satisfied the list operation from cache.
    return GetResultList(kj::mv(cachedEntries), {}, GetResultList::REVERSE, limit);
  }

  {
    KeyPtr k = KJ_ASSERT_NONNULL(storageListEnd);
    if (k == nullptr) {
      // Empty string inside non-null storageListEnd means that our endpoint is the end of the
      // keyspace. (It couldn't possibly mean that our endpoint is the *beginning* of the keyspace,
      // because that would mean that we're listing a zero-sized range, in which case we would have
      // returned earlier.)
      endKey = nullptr;
    } else {
      endKey = cloneKey(k);
    }
  }

  auto adjustedLimit = limit.map([&](uint orig) {
    return orig + limitAdjustment - knownSuffixSize;
  });

  auto paf = kj::newPromiseAndFulfiller<GetResultList>();
  auto streamServer = kj::heap<ReverseListStreamImpl>(
      *this, kj::mv(beginKey), kj::mv(endKey),
      kj::mv(cachedEntries), kj::mv(paf.fulfiller), limit, adjustedLimit, options);
  auto& streamServerRef = *streamServer;

  rpc::ActorStorage::ListStream::Client streamClient = kj::mv(streamServer);

  auto sendPromise = scheduleStorageRead(
      [&streamServerRef,streamClient]
      (rpc::ActorStorage::Operations::Client client) mutable -> kj::Promise<void> {
    auto req = client.listRequest(capnp::MessageSize {
        8 + streamServerRef.beginKey.size() / sizeof(capnp::word) +
        streamServerRef.endKey
            .map([](KeyPtr k) { return k.size() / sizeof(capnp::word); })
            .orDefault(0), 1 });
    if (streamServerRef.beginKey.size() > 0) {
      req.setStart(streamServerRef.beginKey.asBytes());
    }
    KJ_IF_MAYBE(e, streamServerRef.endKey) {
      req.setEnd(e->asBytes());
    }
    req.setReverse(true);
    KJ_IF_MAYBE(l, streamServerRef.adjustedLimit) {
      if (streamServerRef.fetchedEntries.size() >= *l) {
        // Oh it turns out we actually satisfied the limit already so we don't actually have to
        // retry. The fulfiller would have already been fulfilled.
        return kj::READY_NOW;
      }
      req.setLimit(*l - streamServerRef.fetchedEntries.size());
    }
    req.setStream(streamClient);
    return req.send().ignoreResult();
  });

  // Wait on the RPC only until stream.end() is called, then report the results. We prevent
  // `stream` from being destroyed until we have a result so that if the RPC throws an exception,
  // we don't accidentally report "PromiseFulfiller not fulfilled" instead of the exception.
  auto promise = sendPromise
      .then([&streamServerRef]() -> kj::Promise<ActorCache::GetResultList> {
    if (streamServerRef.fulfiller->isWaiting()) {
      return KJ_EXCEPTION(FAILED, "list() never called stream.end()");
    } else {
      // We'll be canceled momentarily...
      return kj::NEVER_DONE;
    }
  });

  return paf.promise.exclusiveJoin(kj::mv(promise))
      .attach(kj::defer([client = kj::mv(streamClient), &streamServerRef]() {
    streamServerRef.cancel();
  }));
}

// -----------------------------------------------------------------------------
// Helpers for read operations

kj::Maybe<kj::Own<ActorCache::Entry>> ActorCache::findInCache(
    Lock& lock, KeyPtr key, const ReadOptions& options) {
  auto& map = currentValues.get(lock);
  auto iter = map.seek(key);
  auto ordered = map.ordered();

  if (iter != ordered.end() && iter->get()->key == key) {
    // Found exact matching entry.
    Entry& entry = **iter;
    switch (entry.state) {
      case CLEAN:
      case STALE:
      case DIRTY:
      case FLUSHING:
        touchEntry(lock, entry, options);
        return kj::atomicAddRef(entry);

      case END_GAP:
        return nullptr;
      case NOT_IN_CACHE:
        KJ_FAIL_ASSERT("NOT_IN_CACHE entry should have been removed from map");
    }
    KJ_UNREACHABLE;
  } else {
    // Key is not in the map, but we have to check for outstanding list() operations by checking
    // the previous entry's gapState.

    if (iter != ordered.begin()) {
      Entry& prev = **--iter;
      if (prev.gapIsKnownEmpty) {
        // A previous list() operation covered this section of the key space and did not find this
        // key, so we know it's not present. Return a dummy entry saying this.
        return makeEntry(lock, CLEAN, cloneKey(key), nullptr);
      }
    }

    // We don't know whether this key exists in storage.
    return nullptr;
  }
}

kj::Own<ActorCache::Entry> ActorCache::addReadResultToCache(
    Lock& lock, Key key, kj::Maybe<capnp::Data::Reader> value, const ReadOptions& options) {
  kj::Own<Entry> entry = makeEntry(lock, CLEAN, kj::mv(key),
      value.map([](capnp::Data::Reader reader) { return kj::heapArray(reader); }));

  if (options.noCache) {
    // We don't actually want to add this to the cache, just return the entry.
    entry->state = NOT_IN_CACHE;
    return kj::mv(entry);
  }

  auto& map = currentValues.get(lock);

  if (value == nullptr) {
    // Inserting a negative entry. Let's check if the new insertion is redundant due to the
    // previous entry having `gapIsKnownEmpty`.
    auto iter = map.seek(entry->key);
    auto ordered = map.ordered();
    if ((iter == ordered.end() || iter->get()->key != entry->key) && iter != ordered.begin()) {
      // We did not find an exact match for the key, so we got an iterator pointing to the next
      // entry after the key. It's not the first entry, so we can back it up one to get the
      // entry before the key.
      --iter;

      if (iter->get()->gapIsKnownEmpty) {
        // This entry is redundant, so we won't insert it. Mark it NOT_IN_CACHE instead of CLEAN to
        // indicate it never entered the map.
        entry->state = NOT_IN_CACHE;
        return kj::mv(entry);
      }
    }

    // TODO(perf): It's a little sad that we are going to do a findOrCreate() below that is going
    //   to repeat the same lookup that produced `iter`. Maybe we could extend kj::Table with a
    //   way to provide an existing iterator as a hint when inserting?
  }

  // At this point, we know we definitely want there to exist an entry matching this key. So now
  // try to insert it.
  auto& slot = map.findOrCreate(entry->key, [&]() {
    // No existing entry has this key, so insert our new entry.
    //
    // Note that it's definitely guaranteed that the entry *before* the one we're inserting cannot
    // possibly have `gapIsKnownEmpty = true`, because:
    // 1. If our new entry has a null value, then we could have returned early above in this case.
    // 2. If our new entry has a non-null value, then it would be inconsistent for a previous
    //    entry to claim that the gap is empty -- this new entry proves it was not! Remember that
    //    we are inserting an entry that was the result of reading from disk, so it *must* be
    //    consistent with any existing knowledge about the state of disk -- unless we have a bug in
    //    the caching logic.
    //
    // Because of this, we know it is correct to leave `gapIsKnownEmpty = false` on our new entry.
    return kj::atomicAddRef(*entry);
  });

  if (slot.get() != entry.get()) {
    // There was a pre-existing entry with the key, so ours wasn't inserted.
    if (slot->state == END_GAP) {
      // Oh, it's just a marker for the end of a list range. Go ahead and insert our new entry into
      // the same slot.
      KJ_ASSERT(!slot->gapIsKnownEmpty);  // END_GAP entry should never have gapIsKnownEmpty.
      slot->state = NOT_IN_CACHE;
      slot = kj::atomicAddRef(*entry);
    } else {
      // The entry that's already in the map must be at least as fresh as the one we just created.
      // If it was created by a put() or delete(), then it is actually fresher. If it was created
      // by a concurrent get() or list() that fetched the same key, then it should be exactly the
      // same value. So, either way, our new entry isn't needed. We mark it NOT_IN_CACHE since it
      // won't be placed in the map.
      //
      // NOTE: You might be tempted to say that if the existing entry is DIRTY, but its value
      //   matches the value that we just read off disk, then we can cancel the write, because
      //   we've discovered it is redundant. Unfortunately, this is NOT true, because it's possible
      //   something else has been written in between. Specifically, we could currently be in the
      //   process of building a transaction that wrote some other value to this specific key, but
      //   hasn't been committed yet, probably because it is waiting for this read operation to
      //   complete. Meanwhile, another put() or delete() could have just been performed
      //   momentarily ago that changed the FLUSHING entry back to DIRTY and changed its value to
      //   one that coincidentally matches what we pulled off disk. However, the open transaction
      //   is still going to be committed, writing the intermediate value, so we still need to plan
      //   to write this value again in the next transaction.
      entry->state = NOT_IN_CACHE;
    }
  }

  if (entry->state == CLEAN) {
    lock->add(*entry);
  }

  return kj::mv(entry);
}

void ActorCache::markGapsEmpty(Lock& lock, KeyPtr beginKey, kj::Maybe<KeyPtr> endKey,
                               const ReadOptions& options) {
  // Set `gapIsKnownEmpty` across the range covered by a new batch of entries arriving from
  // storage via a list() operation. Since we just listed this range, we know that all the gaps
  // between entries in this range can now be marked as empty.
  //
  // You might ask: "But what if an entry was evicted from the cache between when list() was
  // called and now, creating a gap?"
  //
  // There are two possibilities:
  // 1. The evicted entry was clean at the time list() was called. In this case, the list()
  //    operation will have returned it, so it would have been re-added to the cache just
  //    before this method call.
  // 2. The evicted entry was dirty at the time list() was called. This can't cause a problem
  //    because we ensure that any flush is ordered after all previous read operations, so such
  //    entries could not possibly be marked clean until after the list operation completes.
  //    And, they cannot be evicted until they are marked clean. So these entries could not
  //    have been evicted yet.

  if (options.noCache) {
    // Oops, never mind. We're not caching the list() results, so we can't mark anything
    // known-empty.
    return;
  }

  auto& map = currentValues.get(lock);

  auto endIter = seekOrEnd(map, endKey);
  {
    auto ordered = map.ordered();
    if (endIter == ordered.end() || endIter->get()->key > endKey) {
      // The key that we're marking up *to* is not in the map.
      if (endIter == ordered.begin()) {
        // Whoops, it appears we don't actually have any entries in the marking range. This could
        // happen during a forward list() due to entries from previous values() calls having
        // already been evicted before end() was called. In this case, nothing would actually be
        // marked below. But then our END_GAP entry would be inconsistent, so we'd better not
        // insert it at all.
        //
        // Note that this does NOT happen as a result of a list() returning no results, because
        // in that case the list operation would have inserted a negative entry at the beginning
        // of the range. The only reason why we wouldn't have found that negative entry here is
        // because it has since been evicted.
        return;
      }

      --endIter;
      if (endIter->get()->key < beginKey) {
        // Same as above, it appears we have no suitable entries to mark, so we can't insert an
        // END_GAP.
        return;
      }

      if (endIter->get()->gapIsKnownEmpty) {
        // The end key is in an already-known-empty gap, so there's no need to insert an END_GAP.
        // We intentionally leave `endIter` pointing to the start of the gap even though it's not
        // the end of our list range, because we know the stuff from there to the end of the range
        // is already marked.
      } else {
        // We must insert an END_GAP entry to cap our range.
        KJ_IF_MAYBE(k, endKey) {
          map.insert(makeEntry(lock, END_GAP, cloneKey(*k), nullptr));
        } else {
          // No END_GAP needed since the end is actually the end of the key space.
        }

        // Oops, that invalidated our iterator, so find it again.
        endIter = seekOrEnd(map, endKey);
      }
    }
  }

  kj::Vector<KeyPtr> keysToErase;
  auto beginIter = map.seek(beginKey);
  auto mapEnd = map.ordered().end();
  for (auto iter = beginIter; iter != mapEnd; ++iter) {
    auto& entry = **iter;

    if (entry.value == nullptr &&
        (entry.state == END_GAP || entry.state == CLEAN || entry.state == STALE) &&
        (iter != endIter || iter->get()->gapIsKnownEmpty)) {
      // Either:
      // (a) This is an END_GAP entry.
      // (b) This is a clean negative entry.
      //
      // And either:
      // (a) This is not the last entry, so we're about to set `gapIsKnownEmpty` on it.
      // (b) It is the last entry, and it is already `gapIsKnownEmpty`.
      //
      // Either way, if the *previous* entry also has `gapIsKnownEmpty`, then *this* entry
      // becomes redundant. In that case we need to delete it instead.
      //
      // Note that a negative entry that is DIRTY is not necessarily redundant, because it
      // could be that a different value was written to that entry and then deleted between
      // when the list() was initiated and the current state of the cache. A negative DIRTY
      // entry will become redundant once it becomes CLEAN, so we'll have to deal with it then.

      bool prevGapIsEmpty;
      if (iter == beginIter) {
        // This is the first entry in the range, so we have to check if the previous entry
        // was marked.
        if (iter == map.ordered().begin()) {
          prevGapIsEmpty = false;
        } else {
          auto prev = iter;
          --prev;
          prevGapIsEmpty = prev->get()->gapIsKnownEmpty;
        }
      } else {
        // This isn't the first entry we've iterated over so we must have marked the previous
        // one with gapIsKnownEmpty.
        prevGapIsEmpty = true;
      }

      if (prevGapIsEmpty) {
        // Unfortunately erasing from the map will invalidate our iterator, so we need to make
        // a second pass to erase, below.
        keysToErase.add(entry.key);
      }
    }

    if (iter == endIter) {
      // We didn't check for `iter == endIter` earlier because the conditional above -- which
      // potentially deletes redundant entries -- can actually apply to the end of the range, even
      // though that entry itself isn't considered part of the range. Marking the range could cause
      // the entry immediately after the end to become redundant.
      //
      // We do want to break here, though, because we do not want to mark an entry that is past
      // the end of the range.
      break;
    }

    entry.gapIsKnownEmpty = true;
  }

  for (auto& key: keysToErase) {
    auto& entry = KJ_ASSERT_NONNULL(map.find(key));
    switch (entry->state) {
      case CLEAN:
      case STALE:
        lock->remove(*entry);
        break;
      case END_GAP:
        break;
      default:
        KJ_UNREACHABLE;
    }
    entry->state = NOT_IN_CACHE;
    map.erase(entry);
  }
}

ActorCache::GetResultList::GetResultList(kj::Vector<KeyValuePair> contents)
    : entries(contents.size()), cacheStatuses(contents.size()) {
  // TODO(perf): Allocating an `Entry` object for every key/value pair is lame but to avoid it
  //   we'd have to make the common case worse...
  for (auto& kv: contents) {
    entries.add(kj::heap<Entry>(kj::Badge<GetResultList>(), kj::mv(kv.key), kj::mv(kv.value)));
    cacheStatuses.add(CacheStatus::UNCACHED);
  }
}

ActorCache::GetResultList::GetResultList(
    kj::Vector<kj::Own<Entry>> cachedEntries, kj::Vector<kj::Own<Entry>> fetchedEntries,
    Order order, kj::Maybe<uint> maybeLimit) {
  // Merges `cachedEntries` and `fetchedEntries`, which should each already be sorted in the
  // given order. If a key exists in both, `cachedEntries` is preferred.
  //
  // After merging, if an entry's value is null, it is dropped.
  //
  // The final result is truncated to `limit`, if any.
  //
  // The idea is that `cachedEntries` is the set of entries that were loaded from cache while
  // `fetchedEntries` is the set read from storage.

  uint limit = maybeLimit.orDefault(kj::maxValue);
  entries.reserve(kj::min(cachedEntries.size() + fetchedEntries.size(), limit));

  auto cachedIter = cachedEntries.begin();
  auto fetchedIter = fetchedEntries.begin();

  auto add = [&](kj::Own<ActorCache::Entry>&& entry, CacheStatus status) {
    // Remove null values.
    if (entry->value != nullptr) {
      entries.add(kj::mv(entry));
      cacheStatuses.add(status);
    }
  };

  while ((cachedIter != cachedEntries.end() || fetchedIter != fetchedEntries.end()) &&
        entries.size() < limit) {
    if (cachedIter == cachedEntries.end()) {
      add(kj::mv(*fetchedIter++), CacheStatus::UNCACHED);
    } else if (fetchedIter == fetchedEntries.end()) {
      add(kj::mv(*cachedIter++), CacheStatus::CACHED);
    } else if (order == REVERSE
          ? cachedIter->get()->key > fetchedIter->get()->key
          : cachedIter->get()->key < fetchedIter->get()->key) {
      add(kj::mv(*cachedIter++), CacheStatus::CACHED);
    } else if (cachedIter->get()->key == fetchedIter->get()->key) {
      // Same key in both. Prefer the cached entry because it will reflect the state as of when the
      // operation began.
      // Uncached status because we still fetched from disk.
      add(kj::mv(*cachedIter++), CacheStatus::UNCACHED);
      ++fetchedIter;
    } else {
      add(kj::mv(*fetchedIter++), CacheStatus::UNCACHED);
    }
  }

#ifdef KJ_DEBUG
  // Verify sort.
  kj::Maybe<KeyPtr> prev;
  for (auto& entry: entries) {
    KJ_IF_MAYBE(p, prev) {
      if (order == REVERSE) {
        KJ_ASSERT(entry->key < *p);
      } else {
        KJ_ASSERT(entry->key > *p);
      }
    }
    prev = entry->key;
  }
#endif
}

template <typename Func>
kj::PromiseForResult<Func, rpc::ActorStorage::Operations::Client> ActorCache::scheduleStorageRead(
    Func&& function) {
  // This is basically kj::retryOnDisconnect() except that we make the first call synchronously.
  // For our use case, this is safe, and I wanted to make sure reads get sent concurrently with
  // futher JavaScript execution if possible.
  auto promise = kj::evalNow([&]() mutable {
    return function(storage);
  });
  return oomCanceler.wrap(promise.catch_(
      [this, function = kj::mv(function)](kj::Exception&& e) mutable
      -> kj::PromiseForResult<Func, rpc::ActorStorage::Operations::Client> {
    if (e.getType() == kj::Exception::Type::DISCONNECTED) {
      return function(storage);
    } else {
      return kj::mv(e);
    }
  }).attach(kj::addRef(*readCompletionChain)));
}

kj::Promise<void> ActorCache::waitForPastReads() {
  if (!readCompletionChain->isShared()) {
    // No reads are in flight right now.
    return kj::READY_NOW;
  }

  // Create a new chain link.
  auto next = kj::refcounted<ReadCompletionChain>();

  // Update previous chain so that when it is destroyed, it'll fulfill us and also drop its
  // reference on the next link.
  auto paf = kj::newPromiseAndFulfiller<void>();
  readCompletionChain->fulfiller = kj::mv(paf.fulfiller);
  readCompletionChain->next = kj::addRef(*next);

  // Make `next` the current link.
  readCompletionChain = kj::mv(next);

  return kj::mv(paf.promise);
}

ActorCache::ReadCompletionChain::~ReadCompletionChain() noexcept(false) {
  KJ_IF_MAYBE(f, fulfiller) {
    f->get()->fulfill();
  }
}

// =======================================================================================
// write operations

kj::Maybe<kj::Promise<void>> ActorCache::put(Key key, Value value, WriteOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();
  {
    auto lock = lru.cleanList.lockExclusive();
    putImpl(lock, kj::mv(key), kj::mv(value), options, nullptr);
    evictOrOomIfNeeded(lock);
  }
  return getBackpressure();
}

kj::Maybe<kj::Promise<void>> ActorCache::put(kj::Array<KeyValuePair> pairs, WriteOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();
  {
    auto lock = lru.cleanList.lockExclusive();
    for (auto& pair: pairs) {
      putImpl(lock, kj::mv(pair.key), kj::mv(pair.value), options, nullptr);
    }
    evictOrOomIfNeeded(lock);
  }
  return getBackpressure();
}

kj::Maybe<kj::Promise<void>> ActorCache::setAlarm(kj::Maybe<kj::Date> newAlarmTime, WriteOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  KJ_IF_MAYBE(time, currentAlarmTime.tryGet<KnownAlarmTime>()) {
    // If we're in the alarm handler and haven't set the time yet,
    // we can't perform this optimization as currentAlarmTime will be equal
    // to the currently running time but we indicate to the actor in getAlarm() that there
    // is no alarm set, therefore we need to act like that in setAlarm().
    //
    // After the first write in the handler occurs, which would set KnownAlarmTime,
    // the logic here is correct again as currentAlarmTime would match what we are reporting
    // to the user from getAlarm().
    //
    // So, we only apply this for KnownAlarmTime.

    if (time->time == newAlarmTime) {
      // No change! May as well skip the storage operation.
      return nullptr;
    }
  }

  currentAlarmTime = ActorCache::KnownAlarmTime {
    ActorCache::KnownAlarmTime::Status::DIRTY,
    newAlarmTime,
    options.noCache
  };

  ensureFlushScheduled(options);

  return getBackpressure();
}

kj::OneOf<bool, kj::Promise<bool>> ActorCache::delete_(Key key, WriteOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  auto countedDelete = kj::refcounted<CountedDelete>();
  {
    auto lock = lru.cleanList.lockExclusive();
    putImpl(lock, kj::mv(key), nullptr, options, *countedDelete);
    evictOrOomIfNeeded(lock);
  }

  if (countedDelete->isShared()) {
    // The entry kept a reference to `countedDelete`, so it must be waiting on an RPC. Set up a
    // fulfiller.
    auto paf = kj::newPromiseAndFulfiller<uint>();
    countedDelete->resultFulfiller = kj::mv(paf.fulfiller);
    KJ_IF_MAYBE(p, getBackpressure()) {
      return p->then([promise = kj::mv(paf.promise)]() mutable {
        return promise.then([](uint i) { return i > 0; });
      });
    } else {
      return paf.promise.then([](uint i) { return i > 0; });
    }
  } else {
    // It looks like there was a pre-existing cache entry for this key, so we already know whether
    // there was a value to delete.
    bool result = countedDelete->countDeleted > 0;
    KJ_IF_MAYBE(p, getBackpressure()) {
      return p->then([result]() mutable {
        return result;
      });
    } else {
      return result;
    }
  }
}

kj::OneOf<uint, kj::Promise<uint>> ActorCache::delete_(kj::Array<Key> keys, WriteOptions options) {
  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  auto countedDelete = kj::refcounted<CountedDelete>();
  {
    auto lock = lru.cleanList.lockExclusive();
    for (auto& key: keys) {
      putImpl(lock, kj::mv(key), nullptr, options, *countedDelete);
    }
    evictOrOomIfNeeded(lock);
  }

  if (countedDelete->isShared()) {
    // At least one entry kept a reference to `countedDelete`, so it must be waiting on an RPC.
    // Set up a fulfiller.
    auto paf = kj::newPromiseAndFulfiller<uint>();
    countedDelete->resultFulfiller = kj::mv(paf.fulfiller);
    KJ_IF_MAYBE(p, getBackpressure()) {
      return p->then([promise = kj::mv(paf.promise)]() mutable {
        return kj::mv(promise);
      });
    } else {
      return kj::mv(paf.promise);
    }
  } else {
    // It looks like the count of deletes is fully known based on cache content, so we don't need
    // to wait.
    uint result = countedDelete->countDeleted;
    KJ_IF_MAYBE(p, getBackpressure()) {
      return p->then([result]() mutable {
        return result;
      });
    } else {
      return result;
    }
  }
}

kj::Own<ActorCacheInterface::Transaction> ActorCache::startTransaction() {
  return kj::heap<Transaction>(*this);
}

ActorCache::DeleteAllResults ActorCache::deleteAll(WriteOptions options) {
  // Since deleteAll() cannot be performed as part of another transaction, in order to maintain
  // our ordering guarantees, we will have to complete all writes that occurred prior to the
  // deleteAll(), then submit the deleteAll(), then do any writes afterwards. Conveniently, though,
  // a deleteAll() invalidates the whole map. So, we can take all the dirty entries out and place
  // them off to the side for the moment, so that overwrites won't affect them. (Otherwise, an
  // overwritten entry would be moved to the end of the dirty list, which might mean it is
  // committed in the wrong order with respect to the deleteAll().)

  options.noCache = options.noCache || lru.options.noCache;
  requireNotTerminal();

  kj::Promise<uint> result { (uint)0 };

  {
    auto lock = lru.cleanList.lockExclusive();
    auto& map = currentValues.get(lock);

    kj::Vector<kj::Own<Entry>> deletedDirty;
    for (auto& entry: map) {
      switch (entry->state) {
        case DIRTY:
        case FLUSHING:
          // Note that we intentionally keep entry->state as-is because if an entry is currently
          // flushing, we'll need the opportunity to remove it from `requestedDeleteAll` when that
          // flush completes... it turns out it's not _really_ necessary to set an entry to
          // NOT_IN_CACHE state when it's not in the cache.
          dirtyList.remove(*entry);
          deletedDirty.add(kj::mv(entry));
          break;
        case CLEAN:
        case STALE:
          entry->state = NOT_IN_CACHE;
          lock->remove(*entry);
          break;
        case END_GAP:
          entry->state = NOT_IN_CACHE;
          break;
        case NOT_IN_CACHE:
          KJ_FAIL_REQUIRE("NOT_IN_CACHE entry can't be linked");
      }
    }
    map.clear();

    // Insert a dummy entry with an empty key and gapIsKnownEmpty = true to indicate that
    // everything is empty.
    map.findOrCreate(Key(nullptr), [&]() {
      auto entry = makeEntry(lock, CLEAN, Key(nullptr), nullptr);
      lock->add(*entry);
      entry->gapIsKnownEmpty = true;
      return entry;
    });

    if (requestedDeleteAll == nullptr) {
      auto paf = kj::newPromiseAndFulfiller<uint>();
      result = kj::mv(paf.promise);
      requestedDeleteAll = DeleteAllState {
        .deletedDirty = kj::mv(deletedDirty),
        .countFulfiller = kj::mv(paf.fulfiller)
      };
      ensureFlushScheduled(options);
    } else {
      // A previous deleteAll() was scheduled and hasn't been committed yet. This means that we
      // can actually coalesce the two, and there's no need to commit any writes that happened
      // between them. So we can throw away `deletedDirty`.
      // We also don't want to double-bill for a coalesced deleteAll, so we don't update
      // result in this branch.
    }

    // This is called for consistency, but deleteAll() strictly reduces cache usage, so it's not
    // entirely necessary.
    evictOrOomIfNeeded(lock);
  }

  return DeleteAllResults {
    .backpressure = getBackpressure(),
    .count = kj::mv(result)
  };
}

void ActorCache::putImpl(Lock& lock, Key key, kj::Maybe<Value> value,
                         const WriteOptions& options, kj::Maybe<CountedDelete&> countedDelete) {
  // Use NOT_IN_CACHE state until the entry is actually inserted into the map.
  return putImpl(lock, makeEntry(lock, NOT_IN_CACHE, kj::mv(key), kj::mv(value)),
                 options, countedDelete);
}

void ActorCache::putImpl(Lock& lock, kj::Own<Entry> newEntry,
                         const WriteOptions& options, kj::Maybe<CountedDelete&> countedDelete) {
  auto& map = currentValues.get(lock);
  auto ordered = map.ordered();

  // This gets a little complicated because we want to avoid redundant insertions.

  newEntry->noCache = options.noCache;

  auto iter = map.seek(newEntry->key);
  if (iter != ordered.end() && iter->get()->key == newEntry->key) {
    // Exact same entry already exists.
    auto& slot = *iter;

    if (slot->state != END_GAP && slot->value == newEntry->value) {
      // No change! The entry already had this value. Might as well skip the whole storage
      // operation.
      return;
    }

    // Inherit gap state.
    newEntry->gapIsKnownEmpty = slot->gapIsKnownEmpty;

    switch (slot->state) {
      case DIRTY:
      case FLUSHING:
        dirtyList.remove(*slot);
        // Entry may have `countedDelete` indicating we're still waiting to get a count from a
        // previous delete operation. If so, we'll need to inherit it in case that delete operation
        // fails and we end up retrying. Note that the new entry could be a positive entry rather
        // than a negative one (a `put()` rather than a `delete()`). That is OK -- in `flushImpl()`
        // we will still see the presence of `countedDelete` and realize we have to issue a delete
        // on the key before we issue a put, just for the sake of counting it.
        newEntry->countedDelete = kj::mv(slot->countedDelete);
        break;
      case CLEAN:
      case STALE:
        lock->remove(*slot);
        break;
      case END_GAP:
        // Overwrote an entry representing the end of a previous list range.
        break;
      case NOT_IN_CACHE:
        KJ_FAIL_ASSERT("NOT_IN_CACHE entry shouldn't be in map");
        break;
    }

    // Handle our own `countedDelete`.
    KJ_IF_MAYBE(c, countedDelete) {
      if (slot->state == END_GAP) {
        // Despite an entry being present, we don't know if the key exists, because it's just an
        // END_GAP entry. So we will still have to arrange to count the delete later.
        newEntry->countedDelete = kj::addRef(*c);
      } else {
        // Overwrote an entry that was in cache, so we can count it now.
        if (slot->value != nullptr) {
          ++c->countDeleted;
        }
      }
    }

    // Swap in the new entry.
    slot->state = NOT_IN_CACHE;
    KJ_DASSERT(slot->key == newEntry->key);
    slot = kj::mv(newEntry);
    slot->state = DIRTY;
    dirtyList.add(*slot);

  } else {
    // No exact matching entry exists, insert a new one.

    // Does the previous entry have a known-empty gap?
    bool previousGapKnownEmpty = false;
    if (iter != ordered.begin()) {
      --iter;
      previousGapKnownEmpty = iter->get()->gapIsKnownEmpty;
    }
    if (previousGapKnownEmpty && newEntry->value == nullptr) {
      // No change! The entry is already known not to exist, and we're trying to delete it. Might
      // as well skip the whole storage operation.
      return;
    }

    // Create the new entry.
    // TODO(perf): Extend kj::TreeIndex to allow supplying the existing iterator as a hint when
    //   inserting a new entry, to avoid repeating the lookup.
    auto& slot = map.insert(kj::mv(newEntry));
    slot->gapIsKnownEmpty = previousGapKnownEmpty;
    KJ_IF_MAYBE(c, countedDelete) {
      slot->countedDelete = kj::addRef(*c);
    }
    slot->state = DIRTY;
    dirtyList.add(*slot);
  }

  ensureFlushScheduled(options);
}

void ActorCache::ensureFlushScheduled(const WriteOptions& options) {
  if (lru.options.neverFlush) {
    // Skip all flushes. Used for preview sessions where data is strictly kept in memory.

    // Also, we need to handle scheduling or canceling any alarm changes locally.
    KJ_SWITCH_ONEOF(currentAlarmTime) {
      KJ_CASE_ONEOF(knownAlarmTime, ActorCache::KnownAlarmTime) {
        if (knownAlarmTime.status == KnownAlarmTime::Status::DIRTY) {
          knownAlarmTime.status = KnownAlarmTime::Status::CLEAN;
          hooks.updateAlarmInMemory(knownAlarmTime.time);
        }
      }
      KJ_CASE_ONEOF(deferredDelete, ActorCache::DeferredAlarmDelete) {
        if (deferredDelete.status == DeferredAlarmDelete::Status::READY) {
          currentAlarmTime = KnownAlarmTime {
            .status = KnownAlarmTime::Status::CLEAN,
            .time = nullptr,
          };
          hooks.updateAlarmInMemory(nullptr);
        }
      }
      KJ_CASE_ONEOF(_, UnknownAlarmTime){}
    }

    return;
  }

  if (!flushScheduled) {
    flushScheduled = true;
    auto flushPromise = lastFlush.addBranch().attach(kj::defer([this]() {
      flushScheduled = false;
      flushScheduledWithOutputGate = false;
    })).then([this]() {
      ++flushesEnqueued;
      return kj::evalNow([this](){
        // `flushImpl()` can throw, so we need to wrap it in `evalNow()` to observe all pathways.
        return flushImpl();
      }).attach(kj::defer([this](){
        --flushesEnqueued;
      }));
    });

    if (options.allowUnconfirmed) {
      // Don't apply output gate. But, if an exception is thrown, we still want to break the gate,
      // so arrange for that.
      flushPromise = flushPromise.catch_([this](kj::Exception&& e) {
        return gate.lockWhile(kj::Promise<void>(kj::mv(e)));
      });
    } else {
      flushPromise = gate.lockWhile(kj::mv(flushPromise));
      flushScheduledWithOutputGate = true;
    }

    lastFlush = flushPromise.fork();
  } else if (!flushScheduledWithOutputGate && !options.allowUnconfirmed) {
    // The flush has already been scheduled without the output gate, but we want to upgrade it to
    // use the output gate now.
    lastFlush = gate.lockWhile(lastFlush.addBranch()).fork();
    flushScheduledWithOutputGate = true;
  }
}

kj::Maybe<kj::Promise<void>> ActorCache::onNoPendingFlush() {
  // This function returns a Maybe<Promise> because a falsy maybe allows the jsg interface to make
  // a resolved jsg::Promise. This is meaningfully different from a ready kj::Promise because it
  // allows the next continuation to run immediately on the microtask queue instead of returning to
  // the kj event loop and fulfilling a resolver that enqueues the continuation.

  if (lru.options.neverFlush) {
    // We won't ever flush (usually because we're a preview session), so return a falsy maybe.
    return nullptr;
  }

  if (flushScheduled) {
    // There is a flush that is currently scheduled but not yet running, we need to wait for that
    // flush to complete before resolving the jsg::Promise.
    return lastFlush.addBranch();
  }

  if (flushesEnqueued > 0) {
    // There is no flush that is scheduled but there is one running, we need to wait for that flush
    // to complete before resolving the jsg::Promise.
    return lastFlush.addBranch();
  }

  // There are no scheduled or in-flight flushes (and there may never have been any), we can return
  // a false maybee.
  return nullptr;
}

void ActorCache::shutdown(kj::Maybe<const kj::Exception&> maybeException) {
  if (maybeTerminalException == nullptr) {
    auto exception = [&]() {
      KJ_IF_MAYBE(e, maybeException) {
        // We were given an exception, use it.
        return kj::cp(*e);
      }

      // Use the direct constructor so that we can reuse the constexpr message variable for testing.
      auto exception = kj::Exception(
          kj::Exception::Type::OVERLOADED, __FILE__, __LINE__,
          kj::heapString(SHUTDOWN_ERROR_MESSAGE));

      // Add trace info sufficient to tell us which operation caused the failure.
      exception.addTraceHere();
      exception.addTrace(__builtin_return_address(0));
      return exception;
    }();

    // Any scheduled flushes will fail once `flushImpl()` is invoked and notices that
    // `maybeTerminalException` has a value. Any in-flight flushes will continue to run in the
    // background. Remember that these in-flight flushes may or may not be awaited by the worker,
    // but they still hold the output lock as long as `allowUnconfirmed` wasn't used.
    maybeTerminalException.emplace(kj::mv(exception));

    // We explicitly do not schedule a flush to break the output gate. This means that if a request
    // is ongoing after the actor cache is shutting down, the output gate is only broken if they
    // had to send a flush after shutdown, either from a scheduled flush or a retry after failure.
  } else {
    // We've already experienced a terminal exception either from shutdown or oom, there should
    // already be a flush scheduled that will break the output gate.
  }
}

constexpr size_t bytesToWordsRoundUp(size_t bytes) {
  return (bytes + sizeof(capnp::word) - 1) / sizeof(capnp::word);
}

namespace {
using RpcPutRequest = capnp::Request<rpc::ActorStorage::Operations::PutParams,
    rpc::ActorStorage::Operations::PutResults>;

using RpcDeleteRequest = capnp::Request<rpc::ActorStorage::Operations::DeleteParams,
    rpc::ActorStorage::Operations::DeleteResults>;
}

kj::Promise<void> ActorCache::flushImpl(uint retryCount) {
  KJ_IF_MAYBE(e, maybeTerminalException) {
    // If we have a terminal exception, throw here to break the output gate and prevent any calls
    // to storage. This does not use `requireNotTerminal()` so that we don't recursively schedule
    // flushes.
    kj::throwFatalException(kj::cp(*e));
  }

  // Whenever we flush, we MUST write ALL dirty entries in a single transaction. This is necessary
  // because our cache design doesn't necessarily remember the order in which writes were
  // originally initiated, and thus it's not possible to choose a consistent prefix of writes
  // to transact at once. In particular, when two writes occur on the same key with no (successful)
  // flush in between, the first value is thrown away and never written at all. If we then wanted
  // to perform a partial write that brings storage up-to-date with some point in time between the
  // first and second puts, we wouldn't be able to, because we don't have the old value.
  //
  // Perhaps this would be possible to fix by adding more complex logic. But, it doesn't seem
  // like a big deal to require all flushes to be complete flushes.

  // We don't take a lock on `lru.cleanList` here, because we don't need it. We only access
  // `dirtyList`, which is only ever accessed within the actor's thread, so it's safe. We know
  // that `SharedLru` will only ever mess with CLEAN entries, which we don't look at here.

  // We have three kinds of writes: Puts, counted deletes, and muted deletes. Counted deletes are
  // delete operations for which the application still wants to know exactly how many keys are
  // actually deleted. We must make a separate RPC call for each counted delete, in order to get
  // the counts back. But we may also have deletes where the application doesn't need to know the
  // count, either because it discarded the promise already, or because we were able to determine
  // the count based on cache. We call these "muted" deletes, and we can batch them all together.
  // We can also batch all the puts together, because applications don't expect puts to return
  // anything.
  //
  // There's another wrinkle, which is that we don't want to send more than 128 keys per batch.
  // This per-batch limit is historically enforced by our storage back-end
  // (supervisor/actor-storage.c++). Truth be told, the limit is artificial and the original
  // motivations for it don't apply anymore. However, splitting huge batches into smaller ones is
  // beneficial to avoid writing overly large capnp messages and other reasons. So, for puts and
  // muted deletes, we go ahead and construct batches of no more than 128 keys. They all end up
  // being part of the same transaction in the end, though.
  //
  // TODO(perf): Currently we send all the batches at the same time. If the batches are large,
  //   it could be worth spacing them out a bit so we don't saturate the connection. However, we
  //   still need to make sure that the whole transaction represents a consistent snapshot in time,
  //   so getting this right, without making a copy of everything upfront, could get complicated.
  //   Punting for now.

  PutFlush putFlush;
  MutedDeleteFlush mutedDeleteFlush;

  auto includeInCurrentBatch = [this](kj::Vector<FlushBatch>& batches, size_t words) {
    KJ_ASSERT(words < MAX_ACTOR_STORAGE_RPC_WORDS);

    if (batches.empty()) {
      // This is the first one, let's just set up a current batch.
      batches.add(FlushBatch{});
    } else if (auto& tailBatch = batches.back();
        tailBatch.pairCount >= lru.options.maxKeysPerRpc
        || ((tailBatch.wordCount + words) > MAX_ACTOR_STORAGE_RPC_WORDS)) {
      // We've filled this batch, add a new one.
      batches.add(FlushBatch{});
    }

    auto& batch = batches.back();
    ++batch.pairCount;
    batch.wordCount += words;
  };

  kj::Vector<CountedDeleteFlush> countedDeleteFlushes;

  auto countEntry = [&](Entry& entry) {
    // Counts up the number of operations and RPC message sizes we'll need to cover this entry.

    entry.state = FLUSHING;

    auto keySizeInWords = bytesToWordsRoundUp(entry.key.size());

    KJ_IF_MAYBE(c, entry.countedDelete) {
      if (c->get()->resultFulfiller->isWaiting()) {
        // A delete() call on this key is waiting to find out if the key existed in storage. Since
        // each delete() call needs to return the count of keys deleted, we must issue
        // corresponding delete calls to storage with the same batching, so that storage returns
        // the right counts to us. We can't batch all the deletes into a single delete operation
        // since then we'd only get a single count back and we wouldn't know how to split that up
        // to satisfy all the callers.
        //
        // Note that a subsequent put() call could have set entry.value to non-null, but we still
        // have to perform the delete first in order to determine the count that the delete() call
        // should return.
        CountedDeleteFlush* countedDeleteFlush;
        KJ_IF_MAYBE(i, c->get()->flushIndex) {
          countedDeleteFlush = &countedDeleteFlushes[*i];
        } else {
          c->get()->flushIndex = countedDeleteFlushes.size();
          countedDeleteFlush = &countedDeleteFlushes.add(CountedDeleteFlush{
            .countedDelete = kj::addRef(**c).attach(kj::defer([&cd = *c->get()]() mutable {
              // Note that this is attached to the `Own`, not the value. We actually want this,
              // because it allows us to reset the `flushIndex` when *this flush* finishes,
              // regardless of if we need to retry.
              cd.flushIndex = nullptr;
            })),
          });
        }
        auto words = keySizeInWords + 1;
        includeInCurrentBatch(countedDeleteFlush->batches, words);
        countedDeleteFlush->entries.add(&entry);
      } else {
        // No one is waiting on this `CountedDelete` anymore so we can just drop it.
        entry.countedDelete = nullptr;
      }
    }

    KJ_IF_MAYBE(v, entry.value) {
      auto words = keySizeInWords + bytesToWordsRoundUp(v->size()) +
          capnp::sizeInWords<rpc::ActorStorage::KeyValue>();
      includeInCurrentBatch(putFlush.batches, words);
      putFlush.entries.add(&entry);
    } else if (entry.countedDelete == nullptr) {
      auto words = keySizeInWords + 1;
      includeInCurrentBatch(mutedDeleteFlush.batches, words);
      mutedDeleteFlush.entries.add(&entry);
    }
  };

  MaybeAlarmChange maybeAlarmChange = CleanAlarm{};
  KJ_SWITCH_ONEOF(currentAlarmTime) {
    KJ_CASE_ONEOF(knownAlarmTime, ActorCache::KnownAlarmTime) {
      if (knownAlarmTime.status == KnownAlarmTime::Status::DIRTY ||
          knownAlarmTime.status == KnownAlarmTime::Status::FLUSHING) {
        knownAlarmTime.status = KnownAlarmTime::Status::FLUSHING;
        maybeAlarmChange = DirtyAlarm { knownAlarmTime.time };
      }
    }
    KJ_CASE_ONEOF(deferredDelete, ActorCache::DeferredAlarmDelete) {
      if (deferredDelete.status == DeferredAlarmDelete::Status::READY ||
          deferredDelete.status == DeferredAlarmDelete::Status::FLUSHING) {
        deferredDelete.status = DeferredAlarmDelete::Status::FLUSHING;
        maybeAlarmChange = DirtyAlarm { nullptr };
      }
    }
    KJ_CASE_ONEOF(_, UnknownAlarmTime){}
  }

  // We have to remember _before_ waiting for the flush whether or not it was a pre-deleteAll()
  // flush. Otherwise, if it wasn't, but someone calls deleteAll() while we're flushing, then
  // `requestedDeleteAll` might be non-null afterwards, but that would not indicate that we were
  // ready to issue the delete-all.
  bool deleteAllUpcoming = requestedDeleteAll != nullptr;
  KJ_IF_MAYBE(r, requestedDeleteAll) {
    for (auto& entry: r->deletedDirty) {
      countEntry(*entry);
    }
  } else {
    for (auto& entry: dirtyList) {
      countEntry(entry);
    }
  }

  // Actually flush out the changes.
  kj::Promise<void> flushProm = nullptr;
  auto useTransactionToFlush = [&]() {
    flushProm = flushImplUsingTxn(kj::mv(putFlush), kj::mv(mutedDeleteFlush),
        countedDeleteFlushes.releaseAsArray(), kj::mv(maybeAlarmChange));
  };

  uint typesOfDataToFlush = 0;
  if (putFlush.batches.size() > 0) { ++typesOfDataToFlush; }
  if (mutedDeleteFlush.batches.size() > 0) { ++typesOfDataToFlush; }
  if (countedDeleteFlushes.size() > 0) { ++typesOfDataToFlush; }
  if (maybeAlarmChange.is<DirtyAlarm>()) { ++typesOfDataToFlush; }

  if (deleteAllUpcoming && KJ_ASSERT_NONNULL(requestedDeleteAll).deletedDirty.empty()) {
    // There were no dirty entries before deleteAll() was called, so we can move on to invoking
    // deleteAll() itself.
    // NOTE: We put this as the first check to maintain legacy behavior even if there isn't a
    // particularly compelling reason to do this before checking whether the alarm is dirty.
    return flushImplDeleteAll();
  } else if (typesOfDataToFlush == 0) {
    // Oh, nothing to do.
    return kj::READY_NOW;
  } else if (typesOfDataToFlush > 1) {
    // We have multiple types of operations, so we have to use a transaction.
    useTransactionToFlush();
  } else if (maybeAlarmChange.is<DirtyAlarm>()) {
    // We only had an alarm, we can skip the transaction.
    flushProm = flushImplAlarmOnly(maybeAlarmChange.get<DirtyAlarm>());
  } else if (putFlush.batches.size() == 1) {
    // As an optimization for the common case where there are only puts and they all fit in a
    // single batch, just send a simple put rather than complicating things with a transaction.
    flushProm = flushImplUsingSinglePut(kj::mv(putFlush));
  } else if (mutedDeleteFlush.batches.size() == 1) {
    // Same as for puts, but for muted deletes.
    flushProm = flushImplUsingSingleMutedDelete(kj::mv(mutedDeleteFlush));
  } else if (countedDeleteFlushes.size() == 1 && countedDeleteFlushes[0].batches.size() == 1) {
    // Same as for puts, but for muted deletes.
    flushProm = flushImplUsingSingleCountedDelete(kj::mv(countedDeleteFlushes[0]));
    countedDeleteFlushes.clear();
  } else {
    // None of the special cases above triggered. Default to using a transaction in all other cases,
    // such as when there are so many keys to be flushed that they don't fit into a single batch.
    useTransactionToFlush();
  }

  return oomCanceler.wrap(kj::mv(flushProm)).then([this, deleteAllUpcoming]() -> kj::Promise<void> {
    // Success!
    KJ_SWITCH_ONEOF(currentAlarmTime) {
      KJ_CASE_ONEOF(knownAlarmTime, ActorCache::KnownAlarmTime) {
        if (knownAlarmTime.status == KnownAlarmTime::Status::FLUSHING) {
          if (knownAlarmTime.noCache) {
            currentAlarmTime = UnknownAlarmTime{};
          } else {
            knownAlarmTime.status = KnownAlarmTime::Status::CLEAN;
          }
        }
      }
      KJ_CASE_ONEOF(deferredDelete, ActorCache::DeferredAlarmDelete) {
        if (deferredDelete.status == DeferredAlarmDelete::Status::FLUSHING) {
          bool wasDeleted = KJ_ASSERT_NONNULL(deferredDelete.wasDeleted);
          if (deferredDelete.noCache || !wasDeleted) {
            currentAlarmTime = UnknownAlarmTime{};
          } else {
            currentAlarmTime = KnownAlarmTime {
              .status = KnownAlarmTime::Status::CLEAN,
              .time = nullptr,
              .noCache = deferredDelete.noCache
            };
          }
        }
      }
      KJ_CASE_ONEOF(_, ActorCache::UnknownAlarmTime) {}
    }
    if (deleteAllUpcoming) {
      // The writes we flushed were writes that had occurred before a deleteAll. Now that they are
      // written, we must perform the deleteAll() itself.
      return flushImplDeleteAll();
    }

    auto lock = lru.cleanList.lockExclusive();

    KJ_IF_MAYBE(r, requestedDeleteAll) {
      // It would appear that all dirty entries were moved into `requestedDeleteAll` during the
      // time that we were waiting for the flushImpl(). We want to remove the `FLUSHING` entries
      // from that vector now.
      // TODO(cleanup): kj::Vector<T>::filter() would be nice to have here.
      auto dst = r->deletedDirty.begin();
      for (auto src = r->deletedDirty.begin(); src != r->deletedDirty.end(); ++src) {
        if (src->get()->state == DIRTY) {
          if (dst != src) *dst = kj::mv(*src);
          ++dst;
        }
      }
      r->deletedDirty.resize(dst - r->deletedDirty.begin());
    } else {
      // Mark all `FLUSHING` entries as `CLEAN`. Note that we know that all `FLUSHING` must
      // form a prefix of `dirtyList` since any new entries would have been added to the end.
      for (auto& entry: dirtyList) {
        if (entry.state == DIRTY) {
          // Completed all FLUSHING entries.
          break;
        }

        KJ_ASSERT(entry.state == FLUSHING);

        // We know all `countedDelete` operations were satisfied so we can remove this if it's
        // present. Note that if, during the flush, the entry was overwritten, then the new entry
        // will have inherited the `countedDelete`, and will still be DIRTY at this point. That is
        // OK, because the `countedDelete`'s fulfiller will have already been fulfilled, and
        // therefore the next flushImpl() will see that it is obsolete and discard it.
        entry.countedDelete = nullptr;

        dirtyList.remove(entry);
        if (entry.noCache) {
          entry.state = NOT_IN_CACHE;
          evictEntry(lock, entry);
        } else {
          entry.state = CLEAN;

          if (entry.gapIsKnownEmpty && entry.value == nullptr) {
            // This is a negative entry, and is followed by a known-empty gap. If the previous entry
            // also has `gapIsKnownEmpty`, then this entry is entirely redundant.
            auto& map = KJ_ASSERT_NONNULL(entry.cache).currentValues.get(lock);
            auto iter = map.seek(entry.key);
            KJ_ASSERT(iter->get() == &entry);

            if (iter != map.ordered().begin()) {
              auto& slot = *iter;
              --iter;
              if (iter->get()->gapIsKnownEmpty) {
                // Yep!
                entry.state = NOT_IN_CACHE;
                map.erase(slot);
                // WARNING: We might have just deleted `entry`.
                continue;
              }
            }
          }

          lock->add(entry);
        }
      }
    }

    evictOrOomIfNeeded(lock);

    return kj::READY_NOW;
  }, [this,retryCount](kj::Exception&& e) -> kj::Promise<void> {
    static const size_t MAX_RETRIES = 4;
    if (e.getType() == kj::Exception::Type::DISCONNECTED && retryCount < MAX_RETRIES) {
      return flushImpl(retryCount + 1);
    } else if (jsg::isTunneledException(e.getDescription()) ||
               jsg::isDoNotLogException(e.getDescription())) {
      // Before passing along the exception, give it the proper brokenness reason.
      // We were overriding any exception that came through here by ioGateBroken (now outputGateBroken).
      // without checking for previous brokeness reasons we would be unable to throw
      // exceededConcurrentStorageOps at all.
      auto msg = jsg::stripRemoteExceptionPrefix(e.getDescription());
      if (!(msg.startsWith("broken."))) {
        e.setDescription(kj::str("broken.outputGateBroken; ", msg));
      }
      return kj::mv(e);
    } else {
      LOG_EXCEPTION("actorCacheFlush", e);
      return KJ_EXCEPTION(FAILED, "broken.outputGateBroken; jsg.Error: Internal error in Durable "
          "Object storage write caused object to be reset.");
    }
  });
}

kj::Promise<void> ActorCache::flushImplUsingSinglePut(PutFlush putFlush) {
  KJ_ASSERT(putFlush.batches.size() == 1);
  auto& batch = putFlush.batches[0];

  KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);
  KJ_ASSERT(batch.pairCount == putFlush.entries.size());

  auto request = storage.putRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
  auto list = request.initEntries(batch.pairCount);
  auto entryIt = putFlush.entries.begin();
  for (auto kv : list) {
    auto& entry = **(entryIt++);
    auto& v = KJ_ASSERT_NONNULL(entry.value);
    kv.setKey(entry.key.asBytes());
    kv.setValue(v);
  }

  // We're done with the batching instructions, free them before we go async.
  putFlush.entries.clear();
  putFlush.batches.clear();

  // See the comment in flushImplUsingTxn for why we need to construct our RPC and then wait on
  // reads before actually sending the write. The same exact logic applies here.
  co_await waitForPastReads();
  co_await request.send().ignoreResult();
}

kj::Promise<void> ActorCache::flushImplUsingSingleMutedDelete(MutedDeleteFlush mutedFlush) {
  KJ_ASSERT(mutedFlush.batches.size() == 1);
  auto& batch = mutedFlush.batches[0];

  KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);
  KJ_ASSERT(batch.pairCount == mutedFlush.entries.size());

  auto request = storage.deleteRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
  auto listBuilder = request.initKeys(batch.pairCount);
  auto entryIt = mutedFlush.entries.begin();
  for (size_t i = 0; i < batch.pairCount; ++i) {
    auto& entry = **(entryIt++);
    listBuilder.set(i, entry.key.asBytes());
  }

  // We're done with the batching instructions, free them before we go async.
  mutedFlush.entries.clear();
  mutedFlush.batches.clear();

  // See the comment in flushImplUsingTxn for why we need to construct our RPC and then wait on
  // reads before actually sending the write. The same exact logic applies here.
  co_await waitForPastReads();
  co_await request.send().ignoreResult();
}

kj::Promise<void> ActorCache::flushImplUsingSingleCountedDelete(CountedDeleteFlush countedFlush) {
  KJ_ASSERT(countedFlush.batches.size() == 1);
  auto& batch = countedFlush.batches[0];

  KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);
  KJ_ASSERT(batch.pairCount == countedFlush.entries.size());

  auto request = storage.deleteRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
  auto listBuilder = request.initKeys(batch.pairCount);
  auto entryIt = countedFlush.entries.begin();
  for (size_t i = 0; i < batch.pairCount; ++i) {
    auto& entry = **(entryIt++);
    listBuilder.set(i, entry.key.asBytes());
  }

  // We're done with the batching instructions, free them before we go async.
  countedFlush.entries.clear();
  countedFlush.batches.clear();

  auto countedDelete = kj::mv(countedFlush.countedDelete);

  // See the comment in flushImplUsingTxn for why we need to construct our RPC and then wait on
  // reads before actually sending the write. The same exact logic applies here.
  co_await waitForPastReads();
  try {
    auto response = co_await request.send();
    countedDelete->resultFulfiller->fulfill(response.getNumDeleted());
  } catch (kj::Exception& e) {
    if (e.getType() == kj::Exception::Type::DISCONNECTED) {
      // This deletion will be retried, so don't touch the fulfiller.
    } else {
      countedDelete->resultFulfiller->reject(kj::cp(e));
    }
    throw kj::mv(e);
  }
}

kj::Promise<void> ActorCache::flushImplAlarmOnly(DirtyAlarm dirty) {
  co_await waitForPastReads();

  // TODO(someday) This could be templated to reuse the same code for this and the transaction case.
  // Handle alarm writes first, since they're simplest.
  KJ_IF_MAYBE(newTime, dirty.newTime) {
    auto req = storage.setAlarmRequest();
    req.setScheduledTimeMs((*newTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
    co_await req.send().ignoreResult();
    co_return;
  } else {
    // Alarm deletes are a bit trickier because we have to take DeferredAlarmDeletes into account.
    auto req = storage.deleteAlarmRequest();
    KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
      if (deferredDelete->status == DeferredAlarmDelete::Status::FLUSHING) {
        req.setTimeToDeleteMs((deferredDelete->timeToDelete - kj::UNIX_EPOCH) / kj::MILLISECONDS);
        auto response = co_await req.send();
        KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
          if (deferredDelete->status == DeferredAlarmDelete::Status::FLUSHING) {
            // We always update wasDeleted regardless of whether or not it is true
            // because this continuation can succeed even if the greater transaction fails,
            // and so we want to make sure we end up with the correct value if the first
            // attempt succeeds to delete, the txn fails, and the retry fails to delete.
            // The early update is OK because we don't actually use the incorrect state until
            // the transaction succeeds in the .then() below.
            deferredDelete->wasDeleted = response.getDeleted();
          }
        }
      } else {
        // Not sending a delete request for WAITING or READY is intentional. The WAITING
        // state refers to when the alarm run has started but has not completed successfully,
        // and READY is set when the run completes -- only FLUSHING indicates we actually
        // need to send a request.
      }
    } else {
      co_await req.send();
    }
  }
}

kj::Promise<void> ActorCache::flushImplUsingTxn(
    PutFlush putFlush, MutedDeleteFlush mutedDeleteFlush,
    CountedDeleteFlushes countedDeleteFlushes, MaybeAlarmChange maybeAlarmChange) {
  auto txnProm = storage.txnRequest(capnp::MessageSize { 4, 0 }).send();
  auto txn = txnProm.getTransaction();

  struct RpcCountedDelete {
    kj::Own<CountedDelete> countedDelete;
    kj::Array<RpcDeleteRequest> rpcDeletes;
  };
  auto rpcCountedDeletes = kj::heapArrayBuilder<RpcCountedDelete>(countedDeleteFlushes.size());
  auto rpcMutedDeletes = kj::heapArrayBuilder<RpcDeleteRequest>(mutedDeleteFlush.batches.size());
  auto rpcPuts = kj::heapArrayBuilder<RpcPutRequest>(putFlush.batches.size());

  for (auto& flush: countedDeleteFlushes) {
    auto entryIt = flush.entries.begin();
    kj::Vector<RpcDeleteRequest> rpcDeletes;
    for (auto& batch: flush.batches) {
      KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);

      auto request = txn.deleteRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
      auto listBuilder = request.initKeys(batch.pairCount);
      for (size_t i = 0; i < batch.pairCount; ++i) {
        KJ_ASSERT(entryIt != flush.entries.end());
        auto& entry = **(entryIt++);
        listBuilder.set(i, entry.key.asBytes());
      }

      rpcDeletes.add(kj::mv(request));
    }
    rpcCountedDeletes.add(RpcCountedDelete{
      .countedDelete = kj::mv(flush.countedDelete),
      .rpcDeletes = rpcDeletes.releaseAsArray(),
    });
    KJ_ASSERT(entryIt == flush.entries.end());
  }

  {
    auto entryIt = mutedDeleteFlush.entries.begin();
    for (auto& batch: mutedDeleteFlush.batches) {
      KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);

      auto request = txn.deleteRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
      auto listBuilder = request.initKeys(batch.pairCount);
      for (size_t i = 0; i < batch.pairCount; ++i) {
        KJ_ASSERT(entryIt != mutedDeleteFlush.entries.end());
        auto& entry = **(entryIt++);
        listBuilder.set(i, entry.key.asBytes());
      }
      rpcMutedDeletes.add(kj::mv(request));
    }
    KJ_ASSERT(entryIt == mutedDeleteFlush.entries.end());
  }

  {
    auto entryIt = putFlush.entries.begin();
    for (auto& batch: putFlush.batches) {
      KJ_ASSERT(batch.wordCount < MAX_ACTOR_STORAGE_RPC_WORDS);

      auto request = txn.putRequest(capnp::MessageSize { 4 + batch.wordCount, 0 });
      auto listBuilder = request.initEntries(batch.pairCount);
      for (auto kv : listBuilder) {
        KJ_ASSERT(entryIt != putFlush.entries.end());
        auto& entry = **(entryIt++);
        auto& v = KJ_ASSERT_NONNULL(entry.value);
        kv.setKey(entry.key.asBytes());
        kv.setValue(v);
      }
      rpcPuts.add(kj::mv(request));
    }
    KJ_ASSERT(entryIt == putFlush.entries.end());
  }

  // We're done with the batching instructions, free them before we go async.
  putFlush.entries.clear();
  putFlush.batches.clear();
  mutedDeleteFlush.entries.clear();
  mutedDeleteFlush.batches.clear();
  countedDeleteFlushes = nullptr;

  // We don't want to write anything until we know that any past reads have completed, because one
  // or more of those reads could have been on the previous value of a key that was then overwritten
  // by a put() that we're about to flush, and we don't want it to be possible for that read to end
  // up receiving a value that was written later (especially if the read retries due to a disconnect).
  //
  // In practice, most code probably will not have any reads in flight when a flush occurs.
  //
  // This used to only block the commit() of the flush transaction, but that allows for a deadlock
  // race condition where a read gets stuck in the DB waiting on the transaction to finish but the
  // commit() can't be sent until the read completes. Instead, we avoid sending any writes to
  // avoid taking any internal DB latches that may block a read (which could in turn block
  // waitForPastReads).
  //
  // But it is important that we created our put/delete batches prior to waiting on past reads,
  // because if we were to wait before doing so then more new writes might sneak into the flush, and
  // if we were to include those new writes we'd potentially have to wait on past reads again.
  // Similarly, we have to copy the data into our RPC structs prior to waiting instead of after to
  // avoid the data changing out from under us while we wait.
  co_await waitForPastReads();

  // Send all the RPCs. It's important that counted deletes are sent first since they can overlap
  // with puts. Specifically this can happen if someone does a delete() immediately followed by a
  // put() on the same key. These two writes may have been coalesced into a single flush.
  // Unfortunately, we can't just skip the delete because we still need to count it. So we issue
  // a delete, followed by a put, in the same transaction.
  // The constant extra 2 promises are those added outside of the rpc batches, currently one
  // to work around a bug in capnp::autoreconnect, and one to actually commit the flush txn
  // A 3rd promise may be added to write the alarm time if necessary.
  auto promises = kj::heapArrayBuilder<kj::Promise<void>>(
      rpcPuts.size() + rpcMutedDeletes.size() + rpcCountedDeletes.size()
      + 2 + !maybeAlarmChange.is<CleanAlarm>());

  auto joinCountedDelete = [](RpcCountedDelete& rpcCountedDelete) -> kj::Promise<void> {
    auto promises = KJ_MAP(request, rpcCountedDelete.rpcDeletes) {
      return request.send().then(
          [](capnp::Response<rpc::ActorStorage::Operations::DeleteResults>&& response) mutable
          -> uint {
        return response.getNumDeleted();
      });
    };

    for (auto& promise : promises) {
      // Reuse `countDeleted` since it's already in a state object anyway.
      rpcCountedDelete.countedDelete->countDeleted += co_await promise;
    }
  };
  for (auto& rpcCountedDelete: rpcCountedDeletes) {
    promises.add(joinCountedDelete(rpcCountedDelete).then(
        [&countedDelete = *rpcCountedDelete.countedDelete]() mutable {
      // Note that it's OK to trust the delete count even if the transaction ultimately gets rolled
      // back, because:
      // - We know that nothing else could be concurrently modifying our storage in a way that
      //   makes the count different on a retry.
      // - If retries fail and the flush never completes at all, the output gate will kick in and
      //   make it impossible for anyone to observe the bogus result.
      // HACK: This uses a `kj::mv()` because promise fulfillers require rvalues even for trivially
      // copyable types.
      countedDelete.resultFulfiller->fulfill(kj::mv(countedDelete.countDeleted));
    }, [&countedDelete = *rpcCountedDelete.countedDelete](kj::Exception&& e) {
      if (e.getType() == kj::Exception::Type::DISCONNECTED) {
        // This deletion will be retried, so don't touch the fulfiller.
      } else {
        countedDelete.resultFulfiller->reject(kj::mv(e));
      }
    }));
  }

  for (auto& request: rpcMutedDeletes) {
    promises.add(request.send().ignoreResult());
  }

  for (auto& request: rpcPuts) {
    promises.add(request.send().ignoreResult());
  }

  KJ_SWITCH_ONEOF(maybeAlarmChange) {
    KJ_CASE_ONEOF(dirty, DirtyAlarm) {
      KJ_IF_MAYBE(newTime, dirty.newTime) {
        auto req = txn.setAlarmRequest();
        req.setScheduledTimeMs((*newTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
        promises.add(req.send().ignoreResult());
      } else {
        auto req = txn.deleteAlarmRequest();
        KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
          if (deferredDelete->status == DeferredAlarmDelete::Status::FLUSHING) {
            req.setTimeToDeleteMs((deferredDelete->timeToDelete - kj::UNIX_EPOCH) / kj::MILLISECONDS);
            auto prom = req.send().then([this](auto response) {
              KJ_IF_MAYBE(deferredDelete, currentAlarmTime.tryGet<DeferredAlarmDelete>()) {
                if (deferredDelete->status == DeferredAlarmDelete::Status::FLUSHING) {
                  // We always update wasDeleted regardless of whether or not it is true
                  // because this continuation can succeed even if the greater transaction fails,
                  // and so we want to make sure we end up with the correct value if the first
                  // attempt succeeds to delete, the txn fails, and the retry fails to delete.
                  // The early update is OK because we don't actually use the incorrect state until
                  // the transaction succeeds in the .then() below.
                  deferredDelete->wasDeleted = response.getDeleted();
                }
              }
            });
            promises.add(kj::mv(prom));
          }
          // Not sending a delete request for WAITING or READY is intentional. The WAITING
          // state refers to when the alarm run has started but has not completed successfully,
          // and READY is set when the run completes -- only FLUSHING indicates we actually
          // need to send a request.
        } else {
          promises.add(req.send().ignoreResult());
        }
      }
    }
    KJ_CASE_ONEOF(_, CleanAlarm) {}
  }

  // We have to wait on the transaction promise so we don't cancel the catch_ branch that triggers
  // our autoReconnect logic on storage failures.
  // TODO(cleanup): We should probably fix ReconnectHook so the catch_ doesn't get canceled
  // if the promise is dropped but the pipeline stays alive.
  promises.add(txnProm.ignoreResult());

  promises.add(txn.commitRequest(capnp::MessageSize { 4, 0 }).send().ignoreResult());

  co_await kj::joinPromises(promises.finish());
}

kj::Promise<void> ActorCache::flushImplDeleteAll(uint retryCount) {
  // By this point, we've completed any writes that had originally been performed before
  // deleteAll() was called, and we're ready to perform the deleteAll() itself.

  KJ_ASSERT(requestedDeleteAll != nullptr);

  return storage.deleteAllRequest(capnp::MessageSize {2, 0}).send()
      .then([this](capnp::Response<rpc::ActorStorage::Operations::DeleteAllResults> results)
          -> kj::Promise<void> {
    KJ_ASSERT_NONNULL(requestedDeleteAll).countFulfiller->fulfill(results.getNumDeleted());

    // Success! We can now null out `requestedDeleteAll`. Note that we don't have to worry about
    // `requestedDeleteAll` having changed since we flushed it earlier, because it can't change
    // until it is first nulled out. If deleteAll() is called multiple times before the first one
    // finishes, subsequent ones see `requestedDeleteAll` is already non-null and they don't change
    // it. Instead, the writes that occurred between the deleteAll()s are simply discarded, as if
    // the two deleteAll()s had been coalesced into a single one.
    requestedDeleteAll = nullptr;

    {
      auto lock = lru.cleanList.lockExclusive();
      evictOrOomIfNeeded(lock);
    }

    // Now we must flush any writes that happened after the deleteAll(). (If there are none, this
    // will complete quickly.)
    // TODO(soon) This will use the write options for the deleteAll() even if the options for future
    // operations differ. This can mean that we will not wait for the output gate when we were asked
    // to do so. We should fix this.
    return flushImpl();
  }, [this, retryCount](kj::Exception&& e) -> kj::Promise<void> {
    static const size_t MAX_RETRIES = 4;
    if (e.getType() == kj::Exception::Type::DISCONNECTED && retryCount < MAX_RETRIES) {
      return flushImplDeleteAll(retryCount + 1);
    } else if (jsg::isTunneledException(e.getDescription()) ||
               jsg::isDoNotLogException(e.getDescription())) {
      // Before passing along the exception, give it the proper brokenness reason.
      auto msg = jsg::stripRemoteExceptionPrefix(e.getDescription());
      e.setDescription(kj::str("broken.outputGateBroken; ", msg));
      return kj::mv(e);
    } else {
      LOG_EXCEPTION("actorCacheDeleteAll", e);
      return KJ_EXCEPTION(FAILED,
          "broken.outputGateBroken; jsg.Error: Internal error in Durable Object storage deleteAll() caused object to be "
          "reset.");
    }
  });
}

// =======================================================================================
// ActorCache::Transaction

ActorCache::Transaction::Transaction(ActorCache& cache)
    : cache(cache) {}
ActorCache::Transaction::~Transaction() noexcept(false) {
  // If not commit()ed... we don't have to do anything in particular here, just drop the changes.
}

kj::Maybe<kj::Promise<void>> ActorCache::Transaction::commit() {
  {
    auto lock = cache.lru.cleanList.lockExclusive();
    for (auto& change: entriesToWrite) {
      cache.putImpl(lock, kj::mv(change.entry), change.options, nullptr);
    }
    entriesToWrite.clear();
    cache.evictOrOomIfNeeded(lock);
  }

  KJ_IF_MAYBE(change, alarmChange) {
    cache.setAlarm(change->newTime, change->options);
  }
  alarmChange = nullptr;

  return cache.getBackpressure();
}

kj::Promise<void> ActorCache::Transaction::rollback() {
  entriesToWrite.clear();
  alarmChange = nullptr;
  return kj::READY_NOW;
}

// -----------------------------------------------------------------------------
// transaction reads

kj::OneOf<kj::Maybe<ActorCache::Value>, kj::Promise<kj::Maybe<ActorCache::Value>>>
    ActorCache::Transaction::get(Key key, ReadOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  KJ_IF_MAYBE(change, entriesToWrite.find(key)) {
    return change->entry->value.map([&](ValuePtr value) {
      return value.attach(kj::atomicAddRef(*change->entry));
    });
  } else {
    return cache.get(kj::mv(key), options);
  }
}

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::Transaction::get(kj::Array<Key> keys, ReadOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;

  kj::Vector<kj::Own<Entry>> changedEntries;
  kj::Vector<Key> keysToFetch;

  for (auto& key: keys) {
    KJ_IF_MAYBE(change, entriesToWrite.find(key)) {
      changedEntries.add(kj::atomicAddRef(*change->entry));
    } else {
      keysToFetch.add(kj::mv(key));
    }
  }

  std::sort(changedEntries.begin(), changedEntries.end(),
      [](auto& a, auto& b) { return a.get()->key < b.get()->key; });

  return merge(kj::mv(changedEntries), cache.get(keysToFetch.releaseAsArray(), options),
                GetResultList::FORWARD);
}

kj::OneOf<kj::Maybe<kj::Date>, kj::Promise<kj::Maybe<kj::Date>>> ActorCache::Transaction::getAlarm(
    ReadOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  KJ_IF_MAYBE(a, alarmChange) {
    return a->newTime;
  } else {
    return cache.getAlarm(options);
  }
}

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::Transaction::list(Key begin, kj::Maybe<Key> end,
                                  kj::Maybe<uint> limit, ReadOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  kj::Vector<kj::Own<Entry>> changedEntries;
  if (limit.orDefault(kj::maxValue) == 0 || begin >= end) {
    // No results in these cases, just return.
    return ActorCache::GetResultList(kj::mv(changedEntries), {}, GetResultList::REVERSE);
  }
  auto beginIter = entriesToWrite.seek(begin);
  auto endIter = seekOrEnd(entriesToWrite, end);
  uint positiveCount = 0;
  // TODO(cleanup): Add `iterRange()` to KJ's public interface.
  for (auto& change: kj::_::iterRange(beginIter, endIter)) {
    changedEntries.add(kj::atomicAddRef(*change.entry));
    if (change.entry->value != nullptr) {
      ++positiveCount;
    }
    if (positiveCount == limit.orDefault(kj::maxValue)) break;
  }

  // Increase limit to make sure it can't be underrun by negative entries negating it.
  limit = limit.map([&](uint n) { return n + (changedEntries.size() - positiveCount); });

  return merge(kj::mv(changedEntries),
      cache.list(kj::mv(begin), kj::mv(end), limit, options),
      GetResultList::FORWARD);
}

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::Transaction::listReverse(Key begin, kj::Maybe<Key> end,
                                         kj::Maybe<uint> limit, ReadOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  kj::Vector<kj::Own<Entry>> changedEntries;
  if (limit.orDefault(kj::maxValue) == 0 || begin >= end) {
    // No results in these cases, just return.
    return ActorCache::GetResultList(kj::mv(changedEntries), {}, GetResultList::REVERSE);
  }
  auto beginIter = entriesToWrite.seek(begin);
  auto endIter = seekOrEnd(entriesToWrite, end);
  uint positiveCount = 0;
  for (auto iter = endIter; iter != beginIter;) {
    --iter;
    changedEntries.add(kj::atomicAddRef(*iter->entry));
    if (iter->entry->value != nullptr) {
      ++positiveCount;
    }
    if (positiveCount == limit.orDefault(kj::maxValue)) break;
  }

  // Increase limit to make sure it can't be underrun by negative entries negating it.
  limit = limit.map([&](uint n) { return n + (changedEntries.size() - positiveCount); });

  return merge(kj::mv(changedEntries),
      cache.listReverse(kj::mv(begin), kj::mv(end), limit, options),
      GetResultList::REVERSE);
}

kj::OneOf<ActorCache::GetResultList, kj::Promise<ActorCache::GetResultList>>
    ActorCache::Transaction::merge(
        kj::Vector<kj::Own<Entry>> changedEntries,
        kj::OneOf<GetResultList, kj::Promise<GetResultList>> cacheRead,
        GetResultList::Order order) {
  KJ_SWITCH_ONEOF(cacheRead) {
    KJ_CASE_ONEOF(results, GetResultList) {
      return GetResultList(kj::mv(changedEntries), kj::mv(results.entries), order);
    }
    KJ_CASE_ONEOF(promise, kj::Promise<GetResultList>) {
      return promise.then([changedEntries = kj::mv(changedEntries), order]
                          (GetResultList results) mutable {
        return GetResultList(kj::mv(changedEntries), kj::mv(results.entries),
                             order);
      });
    }
  }
  KJ_UNREACHABLE;
}

// -----------------------------------------------------------------------------
// transaction writes

kj::Maybe<kj::Promise<void>> ActorCache::Transaction::put(
    Key key, Value value, WriteOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  auto lock = cache.lru.cleanList.lockExclusive();
  putImpl(lock, kj::mv(key), kj::mv(value), options);

  // Don't apply backpressure because transactions can't be flushed anyway.
  return nullptr;
}

kj::Maybe<kj::Promise<void>> ActorCache::Transaction::put(
    kj::Array<KeyValuePair> pairs, WriteOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  auto lock = cache.lru.cleanList.lockExclusive();

  for (auto& pair: pairs) {
    putImpl(lock, kj::mv(pair.key), kj::mv(pair.value), options);
  }

  // Don't apply backpressure because transactions can't be flushed anyway.
  return nullptr;
}

kj::Maybe<kj::Promise<void>> ActorCache::Transaction::setAlarm(
    kj::Maybe<kj::Date> newTime, WriteOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;
  alarmChange = DirtyAlarmWithOptions { DirtyAlarm { newTime }, options };

  return nullptr;
}

kj::OneOf<bool, kj::Promise<bool>> ActorCache::Transaction::delete_(
    Key key, WriteOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;

  uint count = 0;
  kj::Maybe<KeyPtr> keyToCount;

  {
    auto lock = cache.lru.cleanList.lockExclusive();
    keyToCount = putImpl(lock, kj::mv(key), nullptr, options, count);
  }

  KJ_IF_MAYBE(k, keyToCount) {
    // Unfortunately, to find out the count, we have to do a read.
    KJ_SWITCH_ONEOF(cache.get(cloneKey(*k), {})) {
      KJ_CASE_ONEOF(value, kj::Maybe<ActorCache::Value>) {
        return value != nullptr;
      }
      KJ_CASE_ONEOF(promise, kj::Promise<kj::Maybe<ActorCache::Value>>) {
        return promise.then([](kj::Maybe<ActorCache::Value> value) {
          return value != nullptr;
        });
      }
    }
    KJ_UNREACHABLE;
  } else {
    return count > 0;
  }
}

kj::OneOf<uint, kj::Promise<uint>> ActorCache::Transaction::delete_(
    kj::Array<Key> keys, WriteOptions options) {
  options.noCache = options.noCache || cache.lru.options.noCache;

  if (keys.size() == 0) {
    return 0u;
  }

  uint count = 0;
  kj::Vector<kj::Vector<Key>> keysToCount;
  auto startNewBatch = [&]() {
    return &keysToCount.add();
  };
  auto currentBatch = startNewBatch();

  {
    auto lock = cache.lru.cleanList.lockExclusive();
    for (auto& key: keys) {
      KJ_IF_MAYBE(keyToCount, putImpl(lock, kj::mv(key), nullptr, options, count)) {
        if (currentBatch->size() >= cache.lru.options.maxKeysPerRpc) {
          currentBatch = startNewBatch();
        }
        currentBatch->add(cloneKey(*keyToCount));
      }
    }
  }

  if (keysToCount.size() == 0) {
    return count;
  } else {
    // HACK: Since we allow deletes of larger than our maxKeysPerRpc but these deletes can provoke
    // gets, we need to batch said gets. This all would be much simpler if our default get behavior
    // did batching/sync.
    kj::Maybe<kj::Promise<uint>> maybeTotalPromise;
    for(auto& batch : keysToCount) {
      // Unfortunately, to find out the count, we have to do a read. Note that even returning this
      // value separate from a committed transaction means that non-transaction storage ops can make
      // the value incorrect.
      KJ_SWITCH_ONEOF(cache.get(batch.releaseAsArray(), {})) {
        KJ_CASE_ONEOF(results, GetResultList) {
          count = count + results.size();
        }
        KJ_CASE_ONEOF(promise, kj::Promise<GetResultList>) {
          if (maybeTotalPromise == nullptr) {
            // We had to do a remote get, start a promise
            maybeTotalPromise.emplace(0);
          }
          maybeTotalPromise = KJ_ASSERT_NONNULL(maybeTotalPromise).then(
              [promise = kj::mv(promise)](uint previousResult) mutable -> kj::Promise<uint> {
            return promise.then([previousResult](GetResultList results) mutable -> uint {
              return previousResult + kj::implicitCast<uint>(results.size());
            });
          });
        }
      }
    }

    KJ_IF_MAYBE(totalPromise, maybeTotalPromise) {
      return totalPromise->then([count](uint result){
        return count + result;
      });
    } else {
      return count;
    }
  }
}

kj::Maybe<ActorCache::KeyPtr> ActorCache::Transaction::putImpl(
    Lock& lock, Key key, kj::Maybe<Value> value, const WriteOptions& options,
    kj::Maybe<uint&> count) {
  // Use NOT_IN_CACHE state because this entry is not in the cache map yet.
  Change change {
    cache.makeEntry(lock, NOT_IN_CACHE, kj::mv(key), kj::mv(value)),
    options
  };
  bool replaced = false;
  auto& slot = entriesToWrite.upsert(kj::mv(change), [&](auto& existing, auto&& replacement) {
    replaced = true;
    KJ_IF_MAYBE(c, count) {
      *c += existing.entry->value != nullptr;
    }
    existing = kj::mv(replacement);
  });
  if (replaced) {
    // Already counted.
    return nullptr;
  } else {
    return KeyPtr(slot.entry->key);
  }
}

// =======================================================================================

kj::Promise<kj::String> ActorCacheInterface::getCurrentBookmark() {
  JSG_FAIL_REQUIRE(Error,
      "This Durable Object's storage back-end does not implement point-in-time recovery.");
}

kj::Promise<kj::String> ActorCacheInterface::getBookmarkForTime(kj::Date timestamp) {
  JSG_FAIL_REQUIRE(Error,
      "This Durable Object's storage back-end does not implement point-in-time recovery.");
}

kj::Promise<kj::String> ActorCacheInterface::onNextSessionRestoreBookmark(kj::StringPtr bookmark) {
  JSG_FAIL_REQUIRE(Error,
      "This Durable Object's storage back-end does not implement point-in-time recovery.");
}

}  // namespace workerd
