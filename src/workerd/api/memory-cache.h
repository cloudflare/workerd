#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

#include <kj/time.h>

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

class CacheValueBacking {
 public:
  virtual kj::ArrayPtr<const kj::byte> asBytes() const = 0;
  virtual ~CacheValueBacking() noexcept(false) = default;
};

struct CacheValue: kj::AtomicRefcounted {
  CacheValue(kj::Array<kj::byte>&& bytes): data(kj::mv(bytes)) {}
  CacheValue(kj::Own<CacheValueBacking> backing): data(kj::mv(backing)) {}

  kj::ArrayPtr<const kj::byte> asBytes() const {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(d, kj::Array<kj::byte>) {
        return d.asPtr();
      }
      KJ_CASE_ONEOF(b, kj::Own<CacheValueBacking>) {
        return b->asBytes();
      }
    }
    KJ_UNREACHABLE;
  }

  size_t size() const {
    return asBytes().size();
  }

 private:
  kj::OneOf<kj::Array<kj::byte>, kj::Own<CacheValueBacking>> data;
};

struct CacheValueProduceResult {
  jsg::JsRef<jsg::JsValue> value;
  jsg::Optional<double> expiration;
  JSG_STRUCT(value, expiration);
};

// Limits suggested by a single binding. Bindings that share a cache each
// contribute their limits; the effective limits are the component-wise maximum
// over all live bindings.
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

struct MemoryCachePolicy {
  kj::Maybe<uint64_t> maxTotalValueSize;
};

class MemoryCacheUse {
 public:
  struct FallbackResult {
    kj::Own<CacheValue> value;
    kj::Maybe<double> expiration;
  };
  using FallbackDoneCallback = kj::Function<void(kj::Maybe<FallbackResult>, SpanBuilder&)>;
  using GetWithFallbackOutcome = kj::OneOf<kj::Own<CacheValue>, FallbackDoneCallback>;

  virtual ~MemoryCacheUse() noexcept(false) = default;

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
};

class MemoryCacheNamespace {
 public:
  static kj::Own<MemoryCacheNamespace> create(MemoryCachePolicy policy);
  virtual ~MemoryCacheNamespace() noexcept(false) = default;
  virtual kj::Own<MemoryCacheUse> getBinding(
      kj::Maybe<kj::StringPtr> id, MemoryCacheLimits limits) const = 0;
};

// JavaScript class that allows accessing an in-memory cache.
// Each instance forwards JavaScript calls to the selected backend lease.
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

// The MemoryCacheProvider provides the internal implementation of the MemoryCache mechanism.
// It owns the namespace of caches and hands out bindings to them as needed.
// TODO(later): It may be worth considering some kind of metrics observer for the provider
// that can be passed along to the individual cache instances so we can monitor just how much
// the in memory cache is being used.
class MemoryCacheProvider {
 public:
  explicit MemoryCacheProvider(const kj::MonotonicClock& timer);
  MemoryCacheProvider(const kj::MonotonicClock& timer, MemoryCachePolicy policy);
  KJ_DISALLOW_COPY_AND_MOVE(MemoryCacheProvider);
  ~MemoryCacheProvider() noexcept(false);

  kj::Own<MemoryCacheUse> getUse(kj::Maybe<kj::StringPtr> cacheId, MemoryCacheLimits limits) const;

 private:
  kj::Own<MemoryCacheNamespace> cacheNamespace;

  const kj::MonotonicClock& timer;
};

// clang-format off
#define EW_MEMORY_CACHE_ISOLATE_TYPES                                                   \
  api::MemoryCache,                                                                     \
  api::CacheValueProduceResult
// clang-format on

}  // namespace workerd::api
