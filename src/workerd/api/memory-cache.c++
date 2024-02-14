#include "memory-cache.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/io/io-context.h>
#include <workerd/api/util.h>

namespace workerd::api {

static constexpr size_t MAX_KEY_SIZE = 2 * 1024;

// Returns the current calendar time as a double, just like Date.now() would,
// except without the safeguards that exist within an I/O context. This
// function is used only when a worker is being created or destroyed.
static double getCurrentTimeOutsideIoContext() {
  KJ_ASSERT(!IoContext::hasCurrent());
  auto now = kj::systemCoarseCalendarClock().now();
  return (now - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

// Returns true if the given expiration time exists and has passed. If this is
// called in an I/O context, the I/O context's timer is used. Otherwise,
// if allowOutsideIoContext is true, the system clock is used (see above).
// Lastly, if this function is called from outside of an I/O context and if
// allowOutsideIoContext is false, this function returns false regardless
// of whether the expiration time has passed.
static bool hasExpired(const kj::Maybe<double>& expiration, bool allowOutsideIoContext = false) {
  KJ_IF_SOME(e, expiration) {
    double now = (allowOutsideIoContext && !IoContext::hasCurrent())
        ? getCurrentTimeOutsideIoContext()
        : api::dateNow();
    return e < now;
  }
  return false;
}

void SharedMemoryCache::suggest(const Limits& limits) {
  auto data = this->data.lockExclusive();
  bool isKnownLimit = data->suggestedLimits.find(limits) != data->suggestedLimits.end();
  data->suggestedLimits.insert(limits);
  if (!isKnownLimit) {
    resize(*data);
  }
}

void SharedMemoryCache::unsuggest(const Limits& limits) {
  auto data = this->data.lockExclusive();
  auto loc = data->suggestedLimits.find(limits);
  KJ_ASSERT(loc != data->suggestedLimits.end());
  data->suggestedLimits.erase(loc);
  resize(*data);
}

void SharedMemoryCache::resize(ThreadUnsafeData& data) {
  data.effectiveLimits = Limits::min();
  for (const auto& limits: data.suggestedLimits) {
    data.effectiveLimits = Limits::max(data.effectiveLimits, limits.normalize());
  }

  KJ_IF_SOME(handler, additionalResizeMemoryLimitHandler) {
    // Allow the embedder to adjust the effective limits.
    handler(data);
  }

  // Fast path for clearing the cache.
  if (data.effectiveLimits.maxKeys == 0) {
    data.totalValueSize = 0;
    data.cache.clear();
    return;
  }

  // First, remove any values that might be too large.
  while (data.cache.size() != 0) {
    MemoryCacheEntry& largestEntry = *data.cache.ordered<2>().begin();
    if (largestEntry.size() <= data.effectiveLimits.maxValueSize) {
      break;
    }
    data.totalValueSize -= largestEntry.size();
    data.cache.erase(largestEntry);
  }

  // Now just keep keep evicting until we are within limits.
  while (data.totalValueSize > data.effectiveLimits.maxTotalValueSize ||
      data.cache.size() > data.effectiveLimits.maxKeys) {
    evictNextWhileLocked(data, true);
  }
}

kj::Maybe<kj::Own<CacheValue>> SharedMemoryCache::getWhileLocked(
    ThreadUnsafeData& data, const kj::String& key) {
  KJ_IF_SOME(existingCacheEntry, data.cache.find(key)) {
    if (hasExpired(existingCacheEntry.expiration)) {
      // The cache entry has an associated expiration time and that time has
      // passed (according to the calling IoContext's timer).
      data.totalValueSize -= existingCacheEntry.size();
      data.cache.erase(existingCacheEntry);
      return kj::none;
    }

    // Obtain a reference to the cache value before we kj::mv the cache entry.
    auto cacheValue = kj::atomicAddRef(*existingCacheEntry.value);

    // Update the liveliness.
    MemoryCacheEntry entry = data.cache.release(existingCacheEntry);
    entry.liveliness = data.stepLiveliness();
    data.cache.insert(kj::mv(entry));

    return kj::mv(cacheValue);
  } else {
    return kj::none;
  }
}

void SharedMemoryCache::putWhileLocked(ThreadUnsafeData& data,
    const kj::String& key,
    kj::Own<CacheValue>&& value,
    kj::Maybe<double> expiration) {
  size_t valueSize = value->bytes.size();
  if (valueSize > data.effectiveLimits.maxValueSize) {
    // Silently drop the value. For consistency, also drop the previous value,
    // if one exists, such that a subsequent read() will not return an outdated
    // value. Note that removeIfExistsWhileLocked(key) will update the
    // totalValueSize if necessary, so we don't need to do that here.
    removeIfExistsWhileLocked(data, key);
    return;
  }

  if (hasExpired(expiration)) {
    removeIfExistsWhileLocked(data, key);
    return;
  }

  kj::Maybe<MemoryCacheEntry&> existingEntry = data.cache.find(key.asPtr());
  KJ_IF_SOME(entry, existingEntry) {
    size_t oldValueSize = entry.size();
    KJ_ASSERT(data.totalValueSize >= oldValueSize);
    MemoryCacheEntry updatedEntry = data.cache.release(entry);
    data.totalValueSize -= oldValueSize;
    while (data.totalValueSize + valueSize > data.effectiveLimits.maxTotalValueSize) {
      // We have already released the existing entry for our key, so there is no
      // risk of evicting it.
      evictNextWhileLocked(data);
    }
    updatedEntry.liveliness = data.stepLiveliness();
    updatedEntry.value = kj::mv(value);
    updatedEntry.expiration = expiration;
    data.cache.insert(kj::mv(updatedEntry));
    data.totalValueSize += valueSize;
  } else {
    // Ensure that adding a new key won't push us over the limit.
    if (data.cache.size() >= data.effectiveLimits.maxKeys) {
      evictNextWhileLocked(data);
    }
    // Ensure that the size of the new value won't push us over the limit.
    while (data.totalValueSize + valueSize > data.effectiveLimits.maxTotalValueSize) {
      evictNextWhileLocked(data);
    }
    MemoryCacheEntry newEntry = {
      kj::str(key),
      data.stepLiveliness(),
      kj::mv(value),
      expiration,
    };
    data.cache.insert(kj::mv(newEntry));
    data.totalValueSize += valueSize;
  }
}

void SharedMemoryCache::evictNextWhileLocked(ThreadUnsafeData& data, bool allowOutsideIoContext) {
  // The caller is responsible for ensuring that the cache is not empty already.
  KJ_REQUIRE(data.cache.size() > 0);

  // If there is an entry that has expired already, evict that one.
  MemoryCacheEntry& maybeExpired = *data.cache.ordered<3>().begin();
  KJ_ASSERT(data.totalValueSize >= maybeExpired.size());
  if (hasExpired(maybeExpired.expiration, allowOutsideIoContext)) {
    data.totalValueSize -= maybeExpired.size();
    data.cache.erase(maybeExpired);
    return;
  }

  // Otherwise, if no entry has expired, evict the least recently used entry.
  MemoryCacheEntry& leastRecentlyUsed = *data.cache.ordered<1>().begin();
  KJ_ASSERT(data.totalValueSize >= leastRecentlyUsed.size());
  data.totalValueSize -= leastRecentlyUsed.size();
  data.cache.erase(leastRecentlyUsed);
}

void SharedMemoryCache::removeIfExistsWhileLocked(ThreadUnsafeData& data, const kj::String& key) {
  KJ_IF_SOME(entry, data.cache.find(key)) {
    // This DOES NOT count as an eviction because it might happen while
    // replacing the existing cache entry with a new one, when the new one is
    // being evicted immediately. It is up to the caller to count that.
    size_t valueSize = entry.size();
    KJ_ASSERT(valueSize <= data.totalValueSize);
    data.totalValueSize -= valueSize;
    data.cache.erase(entry);
  }
}

kj::Maybe<kj::Own<CacheValue>> SharedMemoryCache::Use::getWithoutFallback(const kj::String& key) {
  auto data = cache.data.lockExclusive();
  return cache.getWhileLocked(*data, key);
}

kj::OneOf<kj::Own<CacheValue>, kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>>
SharedMemoryCache::Use::getWithFallback(const kj::String& key) {
  auto data = cache.data.lockExclusive();
  KJ_IF_SOME(existingValue, cache.getWhileLocked(*data, key)) {
    return kj::mv(existingValue);
  } else KJ_IF_SOME(existingInProgress, data->inProgress.find(key)) {
    // We return a Promise, but we keep the fulfiller. We might fulfill it
    // from a different thread, so we need a cross-thread fulfiller here.
    auto pair = kj::newPromiseAndCrossThreadFulfiller<GetWithFallbackOutcome>();
    existingInProgress->waiting.push_back(InProgress::Waiter{kj::mv(pair.fulfiller)});
    // We have to register a pending event with the I/O context so that the
    // runtime does not detect a hanging promise. Another fallback is in
    // progress and once it settles, we will fulfill the promise that we return
    // here, either with the produced value or with another fallback task.
    return pair.promise.attach(IoContext::current().registerPendingEvent());
  } else {
    auto& newEntry = data->inProgress.insert(kj::heap<InProgress>(kj::str(key)));
    auto inProgress = newEntry.get();
    return kj::Promise<GetWithFallbackOutcome>(prepareFallback(*inProgress));
  }
}

SharedMemoryCache::Use::FallbackDoneCallback SharedMemoryCache::Use::prepareFallback(
    InProgress& inProgress) {
  // We need to detect if the Promise that we are about to create ever settles,
  // as opposed to being destroyed without either being resolved or rejecting.
  struct FallbackStatus {
    bool hasSettled = false;
  };
  auto status = kj::heap<FallbackStatus>();
  auto& statusRef = *status;

  auto deferredCancel = kj::defer([this, status = kj::mv(status), &inProgress]() {
    // If the callback was destroyed without having run (for example, because
    // it was added to an I/O context that has since been canceled), we treat
    // it as if the promise had failed.
    if (!status->hasSettled) {
      handleFallbackFailure(inProgress);
    }
  });

  return [this, &inProgress, &status = statusRef, deferredCancel = kj::mv(deferredCancel)](
             kj::Maybe<FallbackResult> maybeResult) mutable {
    KJ_IF_SOME(result, maybeResult) {
      // The fallback succeeded. Store the value in the cache and propagate it to
      // all waiting requests, even if it has expired already.
      status.hasSettled = true;
      auto data = cache.data.lockExclusive();
      cache.putWhileLocked(
          *data, kj::str(inProgress.key), kj::atomicAddRef(*result.value), result.expiration);
      for (auto& waiter: inProgress.waiting) {
        waiter.fulfiller->fulfill(kj::atomicAddRef(*result.value));
      }
      data->inProgress.eraseMatch(inProgress.key);
    } else {
      // The fallback failed for some reason. We do not care much about why it
      // failed. If there are other queued fallbacks, handelFallbackFailure will
      // schedule the next one.
      status.hasSettled = true;
      handleFallbackFailure(inProgress);
    }
  };
}

void SharedMemoryCache::Use::handleFallbackFailure(InProgress& inProgress) {
  kj::Own<kj::CrossThreadPromiseFulfiller<GetWithFallbackOutcome>> nextFulfiller;

  // If there is another queued fallback, retrieve it and remove it from the
  // queue. Otherwise, just delete the queue entirely.
  {
    auto data = cache.data.lockExclusive();
    auto next = inProgress.waiting.begin();
    if (next != inProgress.waiting.end()) {
      nextFulfiller = kj::mv(next->fulfiller);
      inProgress.waiting.erase(next);
    } else {
      // Queue is empty, erase it.
      data->inProgress.eraseMatch(inProgress.key);
    }
  }

  // fulfill() might destroy the Promise returned by prepareFallback(). In
  // particular, that will happen if the I/O context that the fulfiller was
  // created for has been canceled or destroyed, in which case the promise
  // associated with the fulfiller has been destroyed. When the promise returned
  // by prepareFallback() is destroyed without having settled, it will recover
  // from that, but it will lock the cache while doing so. That is why it is
  // important that the cache is not already locked when we call fulfill().
  if (nextFulfiller) {
    nextFulfiller->fulfill(prepareFallback(inProgress));
  }
}

// Attempts to serialize a JavaScript value. If that fails, this function throws
// a tunneled exception, see jsg::createTunneledException().
static kj::Own<CacheValue> hackySerialize(jsg::Lock& js, jsg::JsRef<jsg::JsValue>& value) {
  return js.tryCatch([&]() -> kj::Own<CacheValue> {
    jsg::Serializer serializer(js, kj::none);
    serializer.write(js, value.getHandle(js));
    return kj::atomicRefcounted<CacheValue>(serializer.release().data);
  }, [&](jsg::Value&& exception) -> kj::Own<CacheValue> {
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
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> MemoryCache::read(jsg::Lock& js,
    jsg::NonCoercible<kj::String> key,
    jsg::Optional<FallbackFunction> optionalFallback) {
  if (key.value.size() > MAX_KEY_SIZE) {
    return js.rejectedPromise<jsg::JsRef<jsg::JsValue>>(js.rangeError("Key too large."_kj));
  }

  KJ_IF_SOME(fallback, optionalFallback) {
    KJ_SWITCH_ONEOF(cacheUse.getWithFallback(key.value)) {
      KJ_CASE_ONEOF(result, kj::Own<CacheValue>) {
        // Optimization: Don't even release the isolate lock if the value is aleady in cache.
        jsg::Deserializer deserializer(js, result->bytes.asPtr());
        return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
      }
      KJ_CASE_ONEOF(promise, kj::Promise<SharedMemoryCache::Use::GetWithFallbackOutcome>) {
        return IoContext::current().awaitIo(js, kj::mv(promise),
            [fallback = kj::mv(fallback), key = kj::str(key.value)](
                jsg::Lock& js, SharedMemoryCache::Use::GetWithFallbackOutcome cacheResult) mutable
            -> jsg::Promise<jsg::JsRef<jsg::JsValue>> {
          KJ_SWITCH_ONEOF(cacheResult) {
            KJ_CASE_ONEOF(serialized, kj::Own<CacheValue>) {
              jsg::Deserializer deserializer(js, serialized->bytes.asPtr());
              return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
            }
            KJ_CASE_ONEOF(callback, SharedMemoryCache::Use::FallbackDoneCallback) {
              auto& context = IoContext::current();
              auto heapCallback = kj::heap(kj::mv(callback));

              return js.evalNow([&]() { return fallback(js, kj::mv(key)); })
                  .then(js,
                      [callback = context.addObject(*heapCallback)](jsg::Lock& js,
                          CacheValueProduceResult result) mutable -> jsg::JsRef<jsg::JsValue> {
                // NOTE: `callback` is IoPtr, not IoOwn. The catch block gets the IoOwn, which
                //   ensures the object still exists at this point.
                auto serialized = hackySerialize(js, result.value);
                KJ_IF_SOME(expiration, result.expiration) {
                  JSG_REQUIRE(
                      !kj::isNaN(expiration), TypeError, "Expiration time must not be NaN.");
                }
                (*callback)(SharedMemoryCache::Use::FallbackResult{
                  kj::mv(serialized), result.expiration});
                return kj::mv(result.value);
              })
                  .catch_(js,
                      [callback = context.addObject(kj::mv(heapCallback))](jsg::Lock& js,
                          jsg::Value&& exception) mutable -> jsg::JsRef<jsg::JsValue> {
                (*callback)(kj::none);
                js.throwException(kj::mv(exception));
              });
            }
          }
          KJ_UNREACHABLE;
        });
      }
    }
    KJ_UNREACHABLE;
  } else {
    KJ_IF_SOME(cacheValue, cacheUse.getWithoutFallback(key.value)) {
      jsg::Deserializer deserializer(js, cacheValue->bytes.asPtr());
      return js.resolvedPromise(jsg::JsRef(js, deserializer.readValue(js)));
    }
    return js.resolvedPromise(jsg::JsRef(js, js.undefined()));
  }
}

SharedMemoryCache& MemoryCacheMap::getInstance(kj::StringPtr cacheId) const {
  auto lock = caches.lockExclusive();
  return *lock->findOrCreate(cacheId, [this, &cacheId]() {
    auto handler = additionalResizeMemoryLimitHandler.map([](
            const SharedMemoryCache::AdditionalResizeMemoryLimitHandler& handler)
                -> SharedMemoryCache::AdditionalResizeMemoryLimitHandler& {
      return const_cast<SharedMemoryCache::AdditionalResizeMemoryLimitHandler&>(handler);
    });
    return HashMap::Entry{
      kj::str(cacheId),
      kj::heap<SharedMemoryCache>(cacheId, handler)
    };
  });
}

}  // namespace workerd::api
