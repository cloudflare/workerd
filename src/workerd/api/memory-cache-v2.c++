#include "memory-cache-v2-test.h"
#include "memory-cache.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/rust/memory-cache/ffi/lib.rs.h>
#include <workerd/util/autogate.h>
#include <workerd/util/thread-scopes.h>

#include <kj/time.h>

namespace workerd::api {
namespace {

namespace rustCache = workerd::rust::memory_cache;

using Limits = SharedMemoryCache::Limits;
using Outcome = MemoryCacheUse::GetWithFallbackOutcome;
using FallbackResult = MemoryCacheUse::FallbackResult;
using FallbackDoneCallback = MemoryCacheUse::FallbackDoneCallback;

class MemoryCacheUseV1 final: public MemoryCacheUse {
 public:
  MemoryCacheUseV1(kj::Own<const SharedMemoryCache> cache, Limits limits)
      : use(kj::mv(cache), limits) {}

  kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
      const kj::String& key, SpanBuilder& readSpan) const override {
    return use.getWithoutFallback(key, readSpan);
  }

  kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
      const kj::String& key, SpanBuilder& readSpan) const override {
    return use.getWithFallback(key, readSpan);
  }

  void delete_(const kj::String& key) const override {
    use.delete_(key);
  }

 private:
  SharedMemoryCache::Use use;
};

static ::rust::Str asRustStr(kj::StringPtr value) {
  return ::rust::Str(value.begin(), value.size());
}

static ::rust::Slice<const uint8_t> asRustBytes(kj::ArrayPtr<const kj::byte> value) {
  return ::rust::Slice<const uint8_t>(
      reinterpret_cast<const uint8_t*>(value.begin()), value.size());
}

static ::rust::Slice<const uint8_t> asRustBytes(kj::StringPtr value) {
  return ::rust::Slice<const uint8_t>(
      reinterpret_cast<const uint8_t*>(value.begin()), value.size());
}

static rustCache::Limits toRustLimits(Limits limits) {
  return {
    .max_keys = limits.maxKeys,
    .max_value_size = limits.maxValueSize,
    .max_total_value_size = limits.maxTotalValueSize,
  };
}

static double cacheNow() {
  if (IoContext::tryCurrent() != kj::none) {
    return dateNow();
  }
  return (kj::systemPreciseCalendarClock().now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

class RustCacheValueBacking final: public CacheValueBacking {
 public:
  explicit RustCacheValueBacking(::rust::Box<rustCache::Value> value): value(kj::mv(value)) {}

  kj::ArrayPtr<const kj::byte> asBytes() const override {
    auto bytes = value->bytes();
    return kj::arrayPtr(reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size());
  }

 private:
  ::rust::Box<rustCache::Value> value;
};

static kj::Own<CacheValue> makeCacheValue(::rust::Box<rustCache::Value> value) {
  return kj::atomicRefcounted<CacheValue>(kj::heap<RustCacheValueBacking>(kj::mv(value)));
}

class FallbackPermitOwner final {
 public:
  explicit FallbackPermitOwner(::rust::Box<rustCache::FallbackPermit> permit)
      : permit(kj::mv(permit)) {}

  ::rust::Box<rustCache::FallbackPermit> take() {
    KJ_IF_SOME(current, permit) {
      auto result = kj::mv(current);
      permit = kj::none;
      return result;
    }
    KJ_FAIL_REQUIRE("memory cache fallback callback invoked more than once");
  }

 private:
  kj::Maybe<::rust::Box<rustCache::FallbackPermit>> permit;
};

static int64_t lockWaitNsForTrace(uint64_t lockWaitNs) {
  if (isPredictableModeForTest()) {
    return 0;
  }
  return static_cast<int64_t>(kj::min(lockWaitNs, static_cast<uint64_t>(INT64_MAX)));
}

static void emitReadTrace(SpanBuilder& span, const rustCache::ReadTrace& trace) {
  span.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(trace.lock_wait_ns));
  span.setTag("cache_hit"_kjc, trace.cache_hit);
  if (trace.cache_hit) {
    span.setTag("entry_size"_kjc, static_cast<double>(trace.entry_size));
  }
  span.setTag("cache_total_size"_kjc, static_cast<double>(trace.total_value_size));
  span.setTag("cache_entry_count"_kjc, static_cast<double>(trace.entry_count));
}

static void emitWriteTrace(kj::StringPtr key, const rustCache::WriteTrace& trace) {
  auto writeSpan = IoContext::current().makeTraceSpan("memory_cache_write"_kjc);
  writeSpan.setTag("key"_kjc, key);
  writeSpan.setTag("value_size"_kjc, static_cast<double>(trace.value_size));
  writeSpan.setTag("has_expiration"_kjc, trace.has_expiration);
  switch (trace.outcome) {
    case rustCache::WriteOutcome::Success:
      writeSpan.setTag("write_success"_kjc, true);
      writeSpan.setTag("is_update"_kjc, trace.is_update);
      writeSpan.setTag("evictions_triggered"_kjc, static_cast<double>(trace.evictions.size()));
      writeSpan.setTag("cache_total_size_after"_kjc, static_cast<double>(trace.total_after));
      writeSpan.setTag("cache_entry_count_after"_kjc, static_cast<double>(trace.entries_after));
      break;
    case rustCache::WriteOutcome::ValueTooLarge:
      writeSpan.setTag("write_rejected"_kjc, true);
      writeSpan.setTag("rejection_reason"_kjc, "value_too_large"_kjc);
      writeSpan.setTag("max_value_size"_kjc, static_cast<double>(trace.max_value_size));
      break;
    case rustCache::WriteOutcome::AlreadyExpired:
      writeSpan.setTag("write_rejected"_kjc, true);
      writeSpan.setTag("rejection_reason"_kjc, "already_expired"_kjc);
      break;
  }

  for (const auto& eviction: trace.evictions) {
    auto span = IoContext::current().makeTraceSpan("memory_cache_eviction"_kjc);
    switch (eviction.reason) {
      case rustCache::EvictionReason::Expiration:
        span.setTag("eviction_reason"_kjc, "expiration"_kjc);
        break;
      case rustCache::EvictionReason::Lru:
        span.setTag("eviction_reason"_kjc, "lru"_kjc);
        break;
    }
    span.setTag("evicted_key"_kjc,
        kj::str(
            kj::arrayPtr(reinterpret_cast<const char*>(eviction.key.data()), eviction.key.size())));
    span.setTag("evicted_size"_kjc, static_cast<double>(eviction.value_size));
    span.setTag("cache_size_before"_kjc, static_cast<double>(eviction.total_before));
    span.setTag("cache_entries_before"_kjc, static_cast<double>(eviction.entries_before));
  }
}

static FallbackDoneCallback makeFallback(
    ::rust::Box<rustCache::FallbackPermit> permit, kj::String key) {
  return [permit = kj::heap<FallbackPermitOwner>(kj::mv(permit)), key = kj::mv(key)](
             kj::Maybe<FallbackResult> result, SpanBuilder& fallbackSpan) mutable {
    auto currentPermit = permit->take();
    KJ_IF_SOME(value, result) {
      auto source = value.value->asBytes();
      KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
        auto trace = currentPermit->succeed(asRustBytes(source), value.expiration, cacheNow());
        emitWriteTrace(key, trace);
        fallbackSpan.setTag("waiters_notified"_kjc, static_cast<double>(trace.waiters_notified));
      })) {
        KJ_LOG(ERROR, "memory cache fallback completion failed", exception);
      }
    }
  };
}

static Outcome convertWaitOutcome(::rust::Box<rustCache::WaitOutcome> outcome, kj::String key) {
  switch (outcome->kind()) {
    case rustCache::WaitKind::Value:
      return makeCacheValue(outcome->take_value());
    case rustCache::WaitKind::Leader:
      return makeFallback(outcome->take_permit(), kj::mv(key));
    default:
      KJ_UNREACHABLE;
  }
}

class MemoryCacheUseV2 final: public MemoryCacheUse {
 public:
  explicit MemoryCacheUseV2(::rust::Box<rustCache::Binding> binding): binding(kj::mv(binding)) {}
  ~MemoryCacheUseV2() noexcept override {
    binding->release(cacheNow());
  }

  kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
      const kj::String& key, SpanBuilder& readSpan) const override;
  kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
      const kj::String& key, SpanBuilder& readSpan) const override;
  void delete_(const kj::String& key) const override;
  MemoryCacheV2TestStats getStatsForTest() const;

 private:
  ::rust::Box<rustCache::Binding> binding;
};

}  // namespace

MemoryCacheProvider::MemoryCacheProvider(const kj::MonotonicClock& timer)
    : MemoryCacheProvider(timer, MemoryCachePolicy{}) {}

MemoryCacheProvider::MemoryCacheProvider(const kj::MonotonicClock& timer, MemoryCachePolicy policy)
    : additionalResizeMemoryLimitHandler([policy](SharedMemoryCache::ThreadUnsafeData& data) {
        KJ_IF_SOME(cap, policy.maxTotalValueSize) {
          data.effectiveLimits.maxTotalValueSize =
              kj::min(data.effectiveLimits.maxTotalValueSize, cap);
        }
        data.effectiveLimits = data.effectiveLimits.normalize();
      }),
      timer(timer) {
  if (util::Autogate::isEnabled(util::AutogateKey::MEMORY_CACHE_V2)) {
    namespaceV2 = MemoryCacheNamespace::create(policy);
  }
}

kj::Own<MemoryCacheUse> MemoryCacheProvider::getUse(
    kj::Maybe<kj::StringPtr> cacheId, SharedMemoryCache::Limits limits) const {
  KJ_IF_SOME(cacheNamespace, namespaceV2) {
    return cacheNamespace->getBinding(cacheId, limits);
  }
  return kj::heap<MemoryCacheUseV1>(getInstance(cacheId), limits);
}

kj::Own<MemoryCacheNamespace> MemoryCacheNamespace::create(MemoryCachePolicy policy) {
  class V2 final: public MemoryCacheNamespace {
   public:
    explicit V2(MemoryCachePolicy policy)
        : cacheNamespace(rustCache::namespace_new(policy.maxTotalValueSize)) {}

    kj::Own<MemoryCacheUse> getBinding(kj::Maybe<kj::StringPtr> id, Limits limits) const override {
      ::rust::Str name;
      bool isPrivate = id == kj::none;
      KJ_IF_SOME(value, id) {
        name = asRustStr(value);
      }
      auto binding = cacheNamespace->bind(name, isPrivate, toRustLimits(limits));
      return kj::heap<MemoryCacheUseV2>(kj::mv(binding));
    }

   private:
    ::rust::Box<rustCache::Namespace> cacheNamespace;
  };

  return kj::heap<V2>(policy);
}

kj::Maybe<kj::Own<CacheValue>> MemoryCacheUseV2::getWithoutFallback(
    const kj::String& key, SpanBuilder& readSpan) const {
  auto decision = binding->read(asRustBytes(key), dateNow(), rustCache::ReadMode::CacheOnly);
  auto trace = decision->trace();
  emitReadTrace(readSpan, trace);
  switch (decision->kind()) {
    case rustCache::ReadKind::Miss:
      return kj::none;
    case rustCache::ReadKind::Value:
      return makeCacheValue(decision->take_value());
    default:
      KJ_FAIL_ASSERT("unexpected Rust memory cache decision without fallback");
  }
}

kj::OneOf<kj::Own<CacheValue>, kj::Promise<Outcome>> MemoryCacheUseV2::getWithFallback(
    const kj::String& key, SpanBuilder& readSpan) const {
  auto decision = binding->read(asRustBytes(key), dateNow(), rustCache::ReadMode::WithFallback);
  auto trace = decision->trace();
  switch (decision->kind()) {
    case rustCache::ReadKind::Value:
      emitReadTrace(readSpan, trace);
      return makeCacheValue(decision->take_value());
    case rustCache::ReadKind::Leader:
      readSpan.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(trace.lock_wait_ns));
      readSpan.setTag("cache_hit"_kjc, false);
      readSpan.setTag("coalesced_request"_kjc, false);
      readSpan.setTag("initiating_fallback"_kjc, true);
      readSpan.setTag("cache_total_size"_kjc, static_cast<double>(trace.total_value_size));
      readSpan.setTag("cache_entry_count"_kjc, static_cast<double>(trace.entry_count));
      return kj::Promise<Outcome>(makeFallback(decision->take_permit(), kj::str(key)));
    case rustCache::ReadKind::Waiter: {
      readSpan.setTag("memory_cache_lock_wait_time_ns"_kjc, lockWaitNsForTrace(trace.lock_wait_ns));
      readSpan.setTag("cache_hit"_kjc, false);
      readSpan.setTag("coalesced_request"_kjc, true);
      readSpan.setTag("waiting_on_inflight"_kjc, true);
      readSpan.setTag("inflight_waiters_count"_kjc, static_cast<double>(trace.waiters_ahead + 1));
      auto waitSpan = kj::rc<SpanBuilder>(readSpan.newChild("memory_cache_coalesce_wait"_kjc));
      waitSpan->setTag("key"_kjc, key.asPtr());
      waitSpan->setTag("waiters_ahead"_kjc, static_cast<double>(trace.waiters_ahead));
      return rustCache::waiter_wait(decision->take_waiter())
          .then([key = kj::str(key)](::rust::Box<rustCache::WaitOutcome> outcome) mutable {
        return convertWaitOutcome(kj::mv(outcome), kj::mv(key));
      }).attach(IoContext::current().registerPendingEvent(), waitSpan.addRef());
    }
    case rustCache::ReadKind::Miss:
      KJ_FAIL_ASSERT("unexpected Rust memory cache miss with fallback");
    default:
      KJ_UNREACHABLE;
  }
}

void MemoryCacheUseV2::delete_(const kj::String& key) const {
  binding->remove(asRustBytes(key));
}

MemoryCacheV2TestStats MemoryCacheUseV2::getStatsForTest() const {
  auto stats = binding->stats();
  return {stats.bindings, stats.in_flight_fallbacks, stats.waiters, stats.canceled_waiters};
}

MemoryCacheV2TestStats getMemoryCacheV2StatsForTest(const MemoryCacheUse& use) {
  return static_cast<const MemoryCacheUseV2&>(use).getStatsForTest();
}

bool isMemoryCacheV2ForTest(const MemoryCacheProvider& provider) {
  return provider.namespaceV2 != kj::none;
}

}  // namespace workerd::api
