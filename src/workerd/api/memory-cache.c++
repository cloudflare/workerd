#include "memory-cache.h"

#include <workerd/api/util.h>
#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {

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

// ======================================================================================

MemoryCacheProvider::~MemoryCacheProvider() noexcept(false) = default;

}  // namespace workerd::api
