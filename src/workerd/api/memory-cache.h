#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/uuid.h>

#include <kj/hash.h>
#include <kj/map.h>
#include <kj/mutex.h>
#include <kj/table.h>
#include <kj/timer.h>

#include <set>

namespace workerd {
class SpanBuilder;
}

namespace workerd::api {

// The MemoryCache mechanism is an in-process, memory-resident data cache that
// can be configured for workers. A single cache instance can be unique to an
// individual worker or shared across multiple workers / isolates.
//
// Instances are configured as bindings on the worker (set up in the workers
// configuration) and accessible via the environment bindings passed into the
// worker handler functions:
//
//  async fetch(req, env) {
//    await env.MY_CACHE.read('key', () => {
//      // Called if the 'key' does not exist in the cache
//      return 'new value';
//    });
//  }
//
// The cache is only capable of storing values that are v8 serializable (so
// JS primitives other than Symbol, ordinary JavaScript objects but not class
// instances, etc). Objects that represent i/o (like streams or promises are
// explicitly not supported.

struct CacheValue: kj::AtomicRefcounted {
  CacheValue(kj::Array<kj::byte>&& bytes): bytes(kj::mv(bytes)) {}

  kj::Array<kj::byte> bytes;
};

struct MemoryCacheEntry {
  // The key that this entry is associated with.
  kj::String key;

  // Whenever an entry is created, updated, or retrieved, its liveliness is
  // set to the value of a monotonically increasing counter.
  uint64_t liveliness;
  // TODO(cleanup): The liveliness index accomplishes the same thing as
  //   kj::InsertionOrderIndex.
  //
  // TODO(perf): Updating a cache entry's liveliness requires a re-insertion,
  //   which means that cache reads require an exclusive lock. This may be
  //   suboptimal for a read-heavy workload. WorkerSet avoids this by atomically
  //   updating a `lastUsed` timestamp. The tradeoff is that LRU-eviction
  //   becomes O(n) instead of O(1), since we can no longer use kj::Table's
  //   index to find the LRU entry.

  // The stored JavaScript value, serialized by V8. It is atomicRefcounted to
  // allow threads to deserialize the value without having to lock the cache,
  // so the value can even be deserialized while the cache entry is being
  // evicted.
  kj::Own<CacheValue> value;

  inline size_t size() const {
    return value->bytes.size();
  }

  // The expiration timestamp of this cache entry, usually the time at which the
  // entry was created plus some TTL. This is measured in milliseconds and
  // stored as a double so that it is compatible with api::dateNow() and
  // EdgeWorkerPlatform::CurrentClockTimeMillis().
  kj::Maybe<double> expiration;
};

struct CacheValueProduceResult {
  jsg::JsRef<jsg::JsValue> value;
  jsg::Optional<double> expiration;
  JSG_STRUCT(value, expiration);
};

class MemoryCacheProvider;

// An in-memory cache that can be accessed by any number of workers/isolates
// within the same process.
// TODO(soon): We plan to explore replacing this implementation with a memcached-based
// implementation in the near future. The memcached-based impl would likely be
// fairly different from this implementation so quite a few of the details here
// are expected to change.
class SharedMemoryCache: public kj::AtomicRefcounted {
 private:
  struct InProgress;

 public:
  struct ThreadUnsafeData;

  struct Limits {
    // The maximum number of keys that may exist within the cache at the same
    // time. The cache size grows at least linearly in the number of entries.
    uint32_t maxKeys;

    // The maximum size of each individual value, when serialized.
    uint32_t maxValueSize;

    // The maximum sum of all stored values. This is essentially the cache size,
    // except that it only includes the sizes of the values and does not account
    // for keys and the overhead of the data structures themselves.
    uint64_t maxTotalValueSize;

    bool operator<(const Limits& b) const {
      if (maxTotalValueSize != b.maxTotalValueSize) {
        return maxTotalValueSize < b.maxTotalValueSize;
      }
      if (maxKeys != b.maxKeys) {
        return maxKeys < b.maxKeys;
      }
      return maxTotalValueSize < b.maxTotalValueSize;
    }

    Limits normalize() const KJ_WARN_UNUSED_RESULT {
      // Avoid surprises due to misconfigured bindings that set one or more limits to 0.
      if (maxKeys == 0 || maxValueSize == 0 || maxTotalValueSize == 0) {
        return min();
      }

      // If a binding specifies a maxValueSize that exceeds the maxTotalValueSize, remedy
      // that by reducing the maxValueSize.
      return Limits{
        .maxKeys = maxKeys,
        .maxValueSize = static_cast<uint32_t>(kj::min(maxValueSize, maxTotalValueSize)),
        .maxTotalValueSize = maxTotalValueSize,
      };
    }

    static constexpr Limits min() {
      return {0, 0, 0};
    }

    static Limits max(const Limits& a, const Limits& b) {
      return Limits{
        std::max(a.maxKeys, b.maxKeys),
        std::max(a.maxValueSize, b.maxValueSize),
        std::max(a.maxTotalValueSize, b.maxTotalValueSize),
      };
    }
  };

  KJ_DISALLOW_COPY_AND_MOVE(SharedMemoryCache);

  using AdditionalResizeMemoryLimitHandler = kj::Function<void(ThreadUnsafeData&)>;

  SharedMemoryCache(kj::Maybe<const MemoryCacheProvider&> provider,
      kj::StringPtr id,
      kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler,
      const kj::MonotonicClock& timer);

  ~SharedMemoryCache() noexcept(false);

  kj::StringPtr getId() const {
    return id;
  }

  static kj::Own<const SharedMemoryCache> create(kj::Maybe<const MemoryCacheProvider&> provider,
      kj::StringPtr id,
      kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler,
      const kj::MonotonicClock& timer);

 public:
  // RAII class that attaches itself to a cache, suggests cache limits to the
  // cache it is attached to, and allows interacting with the cache.
  class Use {
   public:
    KJ_DISALLOW_COPY(Use);

    Use(kj::Own<const SharedMemoryCache> cache, const Limits& limits);
    Use(Use&& other);
    ~Use() noexcept(false);

    // Returns a cached value for the given key if one exists (and has not
    // expired). If no such value exists, nothing is returned, regardless of any
    // in-progress fallbacks trying to produce such a value.
    kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
        const kj::String& key, SpanBuilder& span) const;

    struct FallbackResult {
      kj::Own<CacheValue> value;
      kj::Maybe<double> expiration;
    };
    typedef kj::Function<void(kj::Maybe<FallbackResult>)> FallbackDoneCallback;
    using GetWithFallbackOutcome = kj::OneOf<kj::Own<CacheValue>, FallbackDoneCallback>;

    // Returns either:
    // 1. The immediate value, if already in cache.
    // 2. A Promise that will eventually resolve either to the cached value
    //    or to a FallbackDoneCallback. In the latter case, the caller should
    //    invoke the fallback function.
    kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
        const kj::String& key, SpanBuilder& span) const;

   private:
    // Creates a new FallbackDoneCallback associated with the given
    // InProgress struct. This is called whenever getWithFallback() wants to
    // invoke a fallback but it does not call the fallback directly. The caller
    // is responsible for passing the returned task and fulfiller to the
    // respective I/O context in which the fallback will run.
    FallbackDoneCallback prepareFallback(InProgress& inProgress) const;

    // Called whenever a fallback has failed. The fallback might have thrown an
    // error or it might have returned a Promise that rejected, or the I/O
    // context in which the fallback should have been invoked has already been
    // destroyed. If other concurrent read operations have queued fallbacks,
    // this schedules the next fallback. Otherwise, the InProgress struct is
    // erased.
    void handleFallbackFailure(InProgress& inProgress) const;

    kj::Own<const SharedMemoryCache> cache;
    static constexpr auto memoryCachekLockWaitTimeTag = "memory_cache_lock_wait_time_ns"_kjc;
    Limits limits;
  };

 private:
  struct InProgress {
    const kj::String key;

    struct Waiter {
      kj::Own<kj::CrossThreadPromiseFulfiller<Use::GetWithFallbackOutcome>> fulfiller;
    };
    std::deque<Waiter> waiting;

    InProgress(kj::String&& key): key(kj::mv(key)) {}

    // Callbacks for a HashIndex that allow locating an InProgress struct
    // based on the cache key.
    class KeyCallbacks {
     public:
      inline const kj::String& keyForRow(const kj::Own<InProgress>& entry) const {
        return entry->key;
      }

      template <typename KeyLike>
      inline bool matches(const kj::Own<InProgress>& e, KeyLike&& key) const {
        return e->key == key;
      }

      template <typename KeyLike>
      inline auto hashCode(KeyLike&& key) const {
        return kj::hashCode(key);
      }
    };
  };

  // Called when initializing globals (i.e., bindings) for an isolate. Each
  // cache binding holds one SharedMemoryCache::Use, which automatically calls
  // this function when created. This call will never reduce the effective cache
  // limits, but might increase them.
  void suggest(const Limits& limits) const;

  // Called when a cache global and its associated SharedMemoryCache::Use is
  // destroyed. This call might reduce the effective cache limits. If all uses
  // have been destroyed, the effective limits will be reset to Limits::min(),
  // effectively clearing the cache.
  void unsuggest(const Limits& limits) const;

  // Used internally by suggest() and unsuggest() to dynamically resize the
  // cache as appropriate. This function also recomputed the effective cache
  // limits and thus must be called even when the cache size is increased (which
  // does not change the cache contents).
  void resize(ThreadUnsafeData& data) const;

  // Returns a cached value while the cache's data is already locked by the
  // calling thread. If such a cache entry exists, it will be marked as the
  // most recently used entry.
  kj::Maybe<kj::Own<CacheValue>> getWhileLocked(
      ThreadUnsafeData& data, const kj::String& key) const;

  // Stores a value in the cache, with an optional expiration timestamp. It is
  // marked as the most recently used entry.
  void putWhileLocked(ThreadUnsafeData& data,
      const kj::String& key,
      kj::Own<CacheValue>&& value,
      kj::Maybe<double> expiration) const;

  // Evicts at least one cache entry. The cache's data must already be locked by
  // the calling thread, and the cache must not be empty. Expiration timestamps
  // are only considered if called from within an I/O context or if
  // allowOutsideIoContext is true.
  void evictNextWhileLocked(ThreadUnsafeData& data, bool allowOutsideIoContext = false) const;

  // Removes the cache entry with the given key, if it exists.
  void removeIfExistsWhileLocked(ThreadUnsafeData& data, const kj::String& key) const;

  // Callbacks for a HashIndex that allow locating cache entries based on the
  // cache key, which is a string. This is used for all key-based cache
  // operations.
  class KeyCallbacks {
   public:
    inline const kj::String& keyForRow(const MemoryCacheEntry& entry) const {
      return entry.key;
    }

    template <typename KeyLike>
    inline bool matches(const MemoryCacheEntry& e, KeyLike&& key) const {
      return e.key == key;
    }

    template <typename KeyLike>
    inline auto hashCode(KeyLike&& key) const {
      return kj::hashCode(key);
    }
  };

  // Callbacks for a TreeIndex that allow sorting cache entries by their
  // liveliness. This is used to evict the least recently used entry.
  class LivelinessCallbacks {
   public:
    inline const uint64_t& keyForRow(const MemoryCacheEntry& entry) const {
      return entry.liveliness;
    }

    template <typename KeyLike>
    inline bool matches(const MemoryCacheEntry& e, KeyLike&& key) const {
      return e.liveliness == key;
    }

    template <typename KeyLike>
    inline bool isBefore(const MemoryCacheEntry& e, KeyLike&& key) const {
      return e.liveliness < key;
    }
  };

  // Callbacks for a TreeIndex that allow sorting cache entries by the sizes
  // of the serialized values. The entries are sorted in reverse order, i.e.,
  // the first entry contains the largest value. This is used to quickly evict
  // the largest cache values when the maximum value size is reduced, e.g.,
  // when a new version of a worker is deployed.
  class ValueSizeCallbacks {
   public:
    inline const MemoryCacheEntry& keyForRow(const MemoryCacheEntry& entry) const {
      return entry;
    }

    template <typename KeyLike>
    inline bool matches(const MemoryCacheEntry& e, KeyLike&& key) const {
      return e.size() == key.size() && e.key == key.key;
    }

    template <typename KeyLike>
    inline bool isBefore(const MemoryCacheEntry& e, KeyLike&& key) const {
      size_t szl = e.size(), szr = key.size();
      if (szl != szr) return szl > szr;
      return e.key < key.key;
    }
  };

  // Callbacks for a TreeIndex that allow sorting cache entries by their
  // expiration times. This is used to quickly evict expired entries even when
  // they are not least recently used. Values with no expiration timestamp are
  // at the very end, ordered by their cache keys.
  class ExpirationCallbacks {
   public:
    inline const MemoryCacheEntry& keyForRow(const MemoryCacheEntry& entry) const {
      return entry;
    }

    template <typename KeyLike>
    inline bool matches(const MemoryCacheEntry& e, KeyLike&& key) const {
      return e.expiration == key.expiration && e.key == key.key;
    }

    template <typename KeyLike>
    inline bool isBefore(const MemoryCacheEntry& e, KeyLike&& key) const {
      const kj::Maybe<double>&expl = e.expiration, expr = key.expiration;
      if (expl != expr) return isBefore(expl, expr);
      return e.key < key.key;
    }

   private:
    inline bool isBefore(const kj::Maybe<double>& a, const kj::Maybe<double>& b) const {
      KJ_IF_SOME(da, a) {
        KJ_IF_SOME(db, b) {
          return da < db;
        } else {
          return false;
        }
      } else {
        KJ_ASSERT(b != kj::none);
        return true;
      }
    }
  };

 public:
  struct ThreadUnsafeData {
    KJ_DISALLOW_COPY_AND_MOVE(ThreadUnsafeData);

    ThreadUnsafeData() {}

    // All limits that have been suggested by isolates that are currently using
    // this cache.
    std::multiset<Limits> suggestedLimits;

    // The computed effective limits. These are updated whenever new isolates
    // are attached to this cache.
    Limits effectiveLimits = Limits::min();

    // Returns the next liveliness and increments it so that the next call to
    // this function will return a different value.
    inline uint64_t stepLiveliness() {
      return nextLiveliness++;
    }

    // We do not handle integer overflow, but a 64-bit counter should never wrap
    // around, at least not in the foreseeable future. (Even at a billion cache
    // operations per second, it would take almost 600 years.)
    uint64_t nextLiveliness = 0;

    // The sum of the sizes of all values that are currently stored in the cache.
    // This is technically redundant information, but more efficient than
    // iterating over all cache entries every time we need this information.
    size_t totalValueSize = 0;

    // The actual cache contents.
    kj::Table<MemoryCacheEntry,              // row type
        kj::HashIndex<KeyCallbacks>,         // index over keys
        kj::TreeIndex<LivelinessCallbacks>,  // index over liveliness
        kj::TreeIndex<ValueSizeCallbacks>,   // index over value sizes
        kj::TreeIndex<ExpirationCallbacks>   // index over expiration
        >
        cache;

    // Whenever a fallback is active for a particular key, this table will
    // contain one corresponding row. Other concurrent read operations can add
    // themselves to the InProgress struct to be notified once the fallback
    // completes. When a fallback succeeds, this immediately notifies all
    // waiting read operations, but when it fails, this behaves like a queue and
    // and invokes the next available fallback only.
    kj::Table<kj::Own<InProgress>, kj::HashIndex<InProgress::KeyCallbacks>> inProgress;
  };

 private:
  // To ensure thread-safety, all mutable data is guarded by a mutex. Each cache
  // operation requires an exclusive lock. Even read-only operations need to
  // update the liveliness of cache entries, which currently requires a lock.
  kj::MutexGuarded<ThreadUnsafeData> data;

  // The MemoryCacheProvider instance needs to be guaranteed to outlive the SharedMemoryCache
  // instance. When the SharedMemoryCache is destroyed, it will remove itself from the provider.
  // TODO(cleanup): Eventually, assuming/once the kj::Ptr<T> work progresses, it would be safer
  // to replace this with a kj::Ptr<MemoryCacheProvider>
  kj::Maybe<const MemoryCacheProvider&> provider;

  // It's a bit unfortunate that we need to keep a copy of the id here as well as in the map
  // in the MemoryCacheProvider, however, it's entirely possible (at least theoretically) that
  // the map entry in the MemoryCacheProvider could be removed before the SharedMemoryCache is
  // fully destroyed, leaving a dangling reference. This be safe and keep a copy.
  kj::String id;

  // Same as above, the MemoryCacheProvider owns the actual handler here. Since that is guaranteed
  // to outlive this SharedMemoryCache instance, so is the handler.
  kj::Maybe<AdditionalResizeMemoryLimitHandler&> additionalResizeMemoryLimitHandler;

  const kj::MonotonicClock& timer;
};

// JavaScript class that allows accessing an in-memory cache.
// Each instance of this class holds a SharedMemoryCache::Use object and
// all calls from JavaScript are essentially forwarded to that object, which
// manages interaction with the shared cache in a thread-safe manner.
class MemoryCache: public jsg::Object {
 public:
  MemoryCache(SharedMemoryCache::Use&& use): cacheUse(kj::mv(use)) {}

  using FallbackFunction = jsg::Function<jsg::Promise<CacheValueProduceResult>(kj::String)>;

  // Reads a value from the cache or invokes a fallback function to obtain the
  // value, if a fallback function was given.
  jsg::Promise<jsg::JsRef<jsg::JsValue>> read(jsg::Lock& js,
      jsg::NonCoercible<kj::String> key,
      jsg::Optional<FallbackFunction> optionalFallback);

  JSG_RESOURCE_TYPE(MemoryCache) {
    JSG_METHOD(read);
  }

 private:
  SharedMemoryCache::Use cacheUse;
};

// The MemoryCacheProvider provides the internal implementation of the MemoryCache mechanism.
// It is responsible for owning the SharedMemoryCache instances and providing them to the
// bindings as needed. The default implementation (created and returned by createDefault())
// uses a simple in-memory map to store the SharedMemoryCache instances.
// TODO(later): It may be worth considering some kind of metrics observer for the provider
// that can be passed along to the individual cache instances so we can monitor just how much
// the in memory cache is being used.
class MemoryCacheProvider {
 public:
  MemoryCacheProvider(const kj::MonotonicClock& timer,
      kj::Maybe<SharedMemoryCache::AdditionalResizeMemoryLimitHandler>
          additionalResizeMemoryLimitHandler = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(MemoryCacheProvider);
  ~MemoryCacheProvider() noexcept(false);

  kj::Own<const SharedMemoryCache> getInstance(kj::Maybe<kj::StringPtr> cacheId = kj::none) const;

  void removeInstance(const SharedMemoryCache& instance) const;

 private:
  kj::Maybe<SharedMemoryCache::AdditionalResizeMemoryLimitHandler>
      additionalResizeMemoryLimitHandler;

  // All existing in-memory *shared* caches. This table will not include caches created
  // that do not have an id (and therefore cannot be shared).
  // TODO(cleanup): Later, assuming progress is made on kj::Ptr<T>, it would be nice
  // to avoid the use of the bare pointer to SharedMemoryCache* here. When the SharedMemoryCache
  // is destroyed, it will remove itself from this cache by calling removeInstance.
  kj::MutexGuarded<kj::HashMap<kj::String, const SharedMemoryCache*>> caches;

  const kj::MonotonicClock& timer;
};

// clang-format off
#define EW_MEMORY_CACHE_ISOLATE_TYPES                                                   \
  api::MemoryCache,                                                                     \
  api::CacheValueProduceResult
// clang-format on

}  // namespace workerd::api
