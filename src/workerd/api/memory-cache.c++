#include "memory-cache.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/rust/memory-cache/ffi/lib.rs.h>
#include <workerd/util/thread-scopes.h>

#include <kj/time.h>

namespace workerd::api {
namespace {

namespace rustCache = workerd::rust::memory_cache;

using Limits = MemoryCacheLimits;
using Outcome = MemoryCacheUse::GetWithFallbackOutcome;
using FallbackResult = MemoryCacheUse::FallbackResult;
using FallbackDoneCallback = MemoryCacheUse::FallbackDoneCallback;

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

class RustCacheValue final: public CacheValue {
 public:
  explicit RustCacheValue(::rust::Box<rustCache::Value> value): value(kj::mv(value)) {}

  kj::ArrayPtr<const kj::byte> asBytes() const override {
    auto bytes = value->bytes();
    return kj::arrayPtr(reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size());
  }

 private:
  ::rust::Box<rustCache::Value> value;
};

static kj::Own<CacheValue> makeCacheValue(::rust::Box<rustCache::Value> value) {
  return kj::heap<RustCacheValue>(kj::mv(value));
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
      KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
        auto trace = currentPermit->succeed(asRustBytes(value.value), value.expiration, cacheNow());
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

class MemoryCacheUseImpl final: public MemoryCacheUse {
 public:
  explicit MemoryCacheUseImpl(::rust::Box<rustCache::Binding> binding): binding(kj::mv(binding)) {}
  ~MemoryCacheUseImpl() noexcept override {
    binding->release(cacheNow());
  }

  kj::Maybe<kj::Own<CacheValue>> getWithoutFallback(
      const kj::String& key, SpanBuilder& readSpan) const override;
  kj::OneOf<kj::Own<CacheValue>, kj::Promise<GetWithFallbackOutcome>> getWithFallback(
      const kj::String& key, SpanBuilder& readSpan) const override;
  void delete_(const kj::String& key) const override;
  Stats getStatsForTest() const override;

 private:
  ::rust::Box<rustCache::Binding> binding;
};

}  // namespace

struct MemoryCacheProvider::Impl {
  explicit Impl(MemoryCachePolicy policy)
      : cacheNamespace(rustCache::namespace_new(policy.maxTotalValueSize)) {}

  ::rust::Box<rustCache::Namespace> cacheNamespace;
};

MemoryCacheProvider::MemoryCacheProvider(): MemoryCacheProvider(MemoryCachePolicy{}) {}

MemoryCacheProvider::MemoryCacheProvider(MemoryCachePolicy policy): impl(kj::heap<Impl>(policy)) {}

MemoryCacheProvider::~MemoryCacheProvider() noexcept(false) = default;

kj::Own<MemoryCacheUse> MemoryCacheProvider::getUse(
    kj::Maybe<kj::StringPtr> cacheId, MemoryCacheLimits limits) const {
  ::rust::Str name;
  bool isPrivate = cacheId == kj::none;
  KJ_IF_SOME(id, cacheId) {
    name = asRustStr(id);
  }
  auto binding = impl->cacheNamespace->bind(name, isPrivate, toRustLimits(limits));
  return kj::heap<MemoryCacheUseImpl>(kj::mv(binding));
}

kj::Maybe<kj::Own<CacheValue>> MemoryCacheUseImpl::getWithoutFallback(
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

kj::OneOf<kj::Own<CacheValue>, kj::Promise<Outcome>> MemoryCacheUseImpl::getWithFallback(
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

void MemoryCacheUseImpl::delete_(const kj::String& key) const {
  binding->remove(asRustBytes(key));
}

MemoryCacheUse::Stats MemoryCacheUseImpl::getStatsForTest() const {
  auto stats = binding->stats();
  return {
    .bindings = stats.bindings,
    .inFlightFallbacks = stats.in_flight_fallbacks,
    .waiters = stats.waiters,
    .canceledWaiters = stats.canceled_waiters,
  };
}

// ======================================================================================

static constexpr size_t MAX_KEY_SIZE = 2 * 1024;

// Attempts to serialize a JavaScript value. If that fails, this function throws
// a tunneled exception, see jsg::createTunneledException().
static kj::Array<kj::byte> hackySerialize(jsg::Lock& js, jsg::JsRef<jsg::JsValue>& value) {
  JSG_TRY(js) {
    jsg::Serializer serializer(js);
    serializer.write(js, value.getHandle(js));
    return serializer.release().data;
  }
  JSG_CATCH(exception) {
    // We run into big problems with tunneled exceptions here. When
    // the toString() function of the JavaScript error is not marked
    // as side effect free, tunneling the exception fails entirely
    // because kj::str() returns an empty string for the error. As a
    // workaround, we drop the error object in that case and return
    // a generic error that only includes the type of the value.
    // TODO(later): remove this workaround
    if (kj::str(exception.getHandle(js)).size() == 0) {
      throw JSG_KJ_EXCEPTION(
          FAILED, DOMDataCloneError, "failed to serialize ", value.getHandle(js).typeOf(js));
    }

    // This is still pretty bad. We lose the original error stack.
    // TODO(later): remove string-based error tunneling
    throw js.exceptionToKj(kj::mv(exception));
  }
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> MemoryCache::read(jsg::Lock& js,
    jsg::NonCoercible<kj::String> key,
    jsg::Optional<FallbackFunction> optionalFallback) {
  if (key.value.size() > MAX_KEY_SIZE) {
    return js.rejectedPromise<jsg::JsRef<jsg::JsValue>>(js.rangeError("Key too large."_kj));
  }

  auto readSpan = IoContext::current().makeTraceSpan("memory_cache_read"_kjc);
  auto userReadSpan = IoContext::current().makeUserTraceSpan("memory_cache_read"_kjc);

  KJ_IF_SOME(fallback, optionalFallback) {
    KJ_SWITCH_ONEOF(cacheUse->getWithFallback(key.value, readSpan)) {
      KJ_CASE_ONEOF(result, kj::Own<CacheValue>) {
        // Optimization: Don't even release the isolate lock if the value is already in cache.
        jsg::Deserializer deserializer(js, result->asBytes());
        auto value = jsg::JsRef(js, deserializer.readValue(js));

        return js.resolvedPromise(kj::mv(value));
      }
      KJ_CASE_ONEOF(promise, kj::Promise<MemoryCacheUse::GetWithFallbackOutcome>) {
        return IoContext::current().awaitIo(js, kj::mv(promise),
            [fallback = kj::mv(fallback), key = kj::str(key.value), readSpan = kj::mv(readSpan),
                userSpan = kj::mv(userReadSpan), self = JSG_THIS](
                jsg::Lock& js, MemoryCacheUse::GetWithFallbackOutcome cacheResult) mutable
            -> jsg::Promise<jsg::JsRef<jsg::JsValue>> {
          KJ_SWITCH_ONEOF(cacheResult) {
            KJ_CASE_ONEOF(serialized, kj::Own<CacheValue>) {
              readSpan.setTag("fallback_cache_hit"_kjc, true);
              readSpan.setTag("entry_size"_kjc, static_cast<double>(serialized->size()));

              jsg::Deserializer deserializer(js, serialized->asBytes());
              return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
            }
            KJ_CASE_ONEOF(callback, MemoryCacheUse::FallbackDoneCallback) {
              auto& context = IoContext::current();
              auto heapCallback = kj::heap(kj::mv(callback));

              // Create a span for the fallback execution
              auto fallbackSpan = readSpan.newChild("memory_cache_fallback"_kjc);
              fallbackSpan.setTag("key"_kjc, key.asPtr());

              // Refcount the spans so they can be shared between then/catch.
              auto fallbackSpanRc = kj::rc<SpanBuilder>(kj::mv(fallbackSpan));
              auto readSpanRc = kj::rc<SpanBuilder>(kj::mv(readSpan));

              return js.evalNow([&]() { return fallback(js, kj::mv(key)); })
                  .then(js,
                      [callback = context.addObject(*heapCallback),
                          fallbackSpan = fallbackSpanRc.addRef(),
                          readSpan = readSpanRc.addRef()](jsg::Lock& js,
                          CacheValueProduceResult result) mutable -> jsg::JsRef<jsg::JsValue> {
                // NOTE: `callback` is IoPtr, not IoOwn. The catch block gets the IoOwn, which
                //   ensures the object still exists at this point.
                fallbackSpan->setTag("fallback_success"_kjc, true);

                auto serialized = hackySerialize(js, result.value);
                fallbackSpan->setTag(
                    "fallback_result_size"_kjc, static_cast<double>(serialized.size()));

                KJ_IF_SOME(expiration, result.expiration) {
                  JSG_REQUIRE(
                      !kj::isNaN(expiration), TypeError, "Expiration time must not be NaN.");
                  fallbackSpan->setTag("has_expiration"_kjc, true);
                } else {
                  fallbackSpan->setTag("has_expiration"_kjc, false);
                }
                (*callback)(MemoryCacheUse::FallbackResult{kj::mv(serialized), result.expiration},
                    *fallbackSpan);
                return kj::mv(result.value);
              })
                  .catch_(js,
                  JSG_VISITABLE_LAMBDA(
                      (self = kj::mv(self), callback = context.addObject(kj::mv(heapCallback)),
                          fallbackSpan = fallbackSpanRc.addRef(), readSpan = readSpanRc.addRef()),
                      (self),
                      (jsg::Lock & js, jsg::Value&& exception) mutable->jsg::JsRef<jsg::JsValue> {
                        fallbackSpan->setTag("fallback_success"_kjc, false);
                        fallbackSpan->setTag(
                            "fallback_error"_kjc, kj::str(exception.getHandle(js)));
                        (*callback)(kj::none, *fallbackSpan);
                        js.throwException(kj::mv(exception));
                      }));
            }
          }
          KJ_UNREACHABLE;
        });
      }
    }
    KJ_UNREACHABLE;
  } else {
    KJ_IF_SOME(cacheValue, cacheUse->getWithoutFallback(key.value, readSpan)) {
      jsg::Deserializer deserializer(js, cacheValue->asBytes());
      return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
    }
    return js.resolvedPromise(jsg::JsRef(js, js.undefined()));
  }
}

void MemoryCache::delete_(jsg::Lock& js, jsg::NonCoercible<kj::String> key) {
  // Ignore operations on keys exceeding key max size.
  if (key.value.size() > MAX_KEY_SIZE) {
    js.throwException(js.rangeError("Key too large."_kj));
    return;
  }

  auto deleteSpan = IoContext::current().makeTraceSpan("memory_cache_delete"_kjc);
  deleteSpan.setTag("key"_kjc, key.value.asPtr());

  cacheUse->delete_(key.value);

  deleteSpan.setTag("delete_completed"_kjc, true);
}

}  // namespace workerd::api
