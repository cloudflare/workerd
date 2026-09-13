#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

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
//
// The cache storage and singleflight coordination live in the Rust crate at
// src/rust/memory-cache. The classes here adapt that crate to JSG and hide the
// generated CXX bridge types from the rest of the code base.

// A serialized JavaScript value read from the cache. The backing bytes are
// owned by the cache implementation and stay valid for the lifetime of this
// object, even if the entry is evicted in the meantime.
class CacheValue {
 public:
  virtual ~CacheValue() noexcept(false) = default;
  virtual kj::ArrayPtr<const kj::byte> asBytes() const = 0;

  size_t size() const {
    return asBytes().size();
  }
};

struct CacheValueProduceResult {
  jsg::JsRef<jsg::JsValue> value;
  jsg::Optional<double> expiration;
  JSG_STRUCT(value, expiration);
};

// Limits suggested by a single binding. Bindings that share a cache each
// contribute their limits; the effective limits are the component-wise maximum
// over all live bindings, capped by the provider's MemoryCachePolicy.
struct MemoryCacheLimits {
  // The maximum number of keys that may exist within the cache at the same
  // time. The cache size grows at least linearly in the number of entries.
  uint32_t maxKeys;

  // The maximum size of each individual value, when serialized.
  uint32_t maxValueSize;

  // The maximum sum of all stored values. This is essentially the cache size,
  // except that it only includes the sizes of the values and does not account
  // for keys and the overhead of the data structures themselves.
  uint64_t maxTotalValueSize;
};

// Process-wide policy applied on top of the limits suggested by bindings.
struct MemoryCachePolicy {
  // Upper bound for the effective maxTotalValueSize of every cache owned by the
  // provider, regardless of what the bindings request.
  kj::Maybe<uint64_t> maxTotalValueSize;
};

// A binding's handle to a cache. Each MemoryCache JS object owns one of these.
// Destroying it withdraws the binding's suggested limits from the cache; once
// the last binding for a shared cache is gone, the cache itself is dropped.
class MemoryCacheUse {
 public:
  virtual ~MemoryCacheUse() noexcept(false) = default;

  struct FallbackResult {
    kj::Array<kj::byte> value;
    kj::Maybe<double> expiration;
  };
  // Invoked exactly once by the caller that was elected to run the fallback.
  // Passing a result stores it and publishes it to all coalesced readers.
  // Passing kj::none (or dropping the callback without invoking it) releases
  // the fallback so that the next coalesced reader is promoted to run it.
  using FallbackDoneCallback = kj::Function<void(kj::Maybe<FallbackResult>, SpanBuilder&)>;
  using GetWithFallbackOutcome = kj::OneOf<kj::Own<CacheValue>, FallbackDoneCallback>;

  // Returns a cached value for the given key if one exists (and has not
  // expired). If no such value exists, nothing is returned, regardless of any
  // in-progress fallbacks trying to produce such a value.
  virtual kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
      const kj::String& key, SpanBuilder& readSpan) const = 0;

  // Returns either:
  // 1. The immediate value, if already in cache.
  // 2. A Promise that will eventually resolve either to the cached value
  //    or to a FallbackDoneCallback. In the latter case, the caller should
  //    invoke the fallback function.
  virtual kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
      const kj::String& key, SpanBuilder& readSpan) const = 0;

  virtual void delete_(const kj::String& key) const = 0;

  struct Stats {
    size_t bindings;
    size_t inFlightFallbacks;
    size_t waiters;
    size_t canceledWaiters;
  };
  // Best-effort counters for the cache this binding is attached to. The
  // waiter counts are maintained without holding the cache lock and are only
  // meant for assertions in tests.
  virtual Stats getStatsForTest() const = 0;
};

// JavaScript class that allows accessing an in-memory cache.
class MemoryCache: public jsg::Object {
 public:
  MemoryCache(kj::Own<MemoryCacheUse> use): cacheUse(kj::mv(use)) {}

  using FallbackFunction = jsg::Function<jsg::Promise<CacheValueProduceResult>(kj::String)>;

  // Reads a value from the cache or invokes a fallback function to obtain the
  // value, if a fallback function was given.
  jsg::Promise<jsg::JsRef<jsg::JsValue>> read(jsg::Lock& js,
      jsg::NonCoercible<kj::String> key,
      jsg::Optional<FallbackFunction> optionalFallback);

  // Delete a value from the cache.
  void delete_(jsg::Lock& js, jsg::NonCoercible<kj::String> key);

  JSG_RESOURCE_TYPE(MemoryCache, CompatibilityFlags::Reader flags) {
    JSG_METHOD(read);
    if (flags.getMemoryCacheDelete()) {
      JSG_METHOD_NAMED(delete, delete_);
    }
  }

 private:
  kj::Own<MemoryCacheUse> cacheUse;
};

// Owns the namespace of caches for one process (or one sandbox). Caches with
// the same id are shared between all bindings that request them; bindings
// without an id get a private cache. The provider only has to outlive the
// calls to getUse(): the returned MemoryCacheUse keeps its cache alive by
// itself, so it is safe to destroy the provider while bindings still exist.
// TODO(later): It may be worth considering some kind of metrics observer for the provider
// that can be passed along to the individual cache instances so we can monitor just how much
// the in memory cache is being used.
class MemoryCacheProvider {
 public:
  MemoryCacheProvider();
  explicit MemoryCacheProvider(MemoryCachePolicy policy);
  KJ_DISALLOW_COPY_AND_MOVE(MemoryCacheProvider);
  ~MemoryCacheProvider() noexcept(false);

  kj::Own<MemoryCacheUse> getUse(kj::Maybe<kj::StringPtr> cacheId, MemoryCacheLimits limits) const;

 private:
  struct Impl;
  kj::Own<Impl> impl;
};

// clang-format off
#define EW_MEMORY_CACHE_ISOLATE_TYPES                                                   \
  api::MemoryCache,                                                                     \
  api::CacheValueProduceResult
// clang-format on

}  // namespace workerd::api
