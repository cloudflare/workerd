// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/performance.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// Originally implemented by Node.js contributors.
// Available at https://github.com/nodejs/node/blob/b7b96282b212a2274b9db605ac29d388246754de/src/node_buffer.h#L75
// This is verbose to be explicit with inline commenting
static constexpr bool IsWithinBounds(size_t off, size_t len, size_t max) noexcept {
  // Asking to seek too far into the buffer
  // check to avoid wrapping in subsequent subtraction
  if (off > max) return false;

  // Asking for more than is left over in the buffer
  if (max - off < len) return false;

  // Otherwise we're in bounds
  return true;
}

#define JS_UTIL_IS_TYPES(V)                                                                        \
  V(ArrayBufferView)                                                                               \
  V(ArgumentsObject)                                                                               \
  V(ArrayBuffer)                                                                                   \
  V(AsyncFunction)                                                                                 \
  V(BigInt64Array)                                                                                 \
  V(BigIntObject)                                                                                  \
  V(BigUint64Array)                                                                                \
  V(BooleanObject)                                                                                 \
  V(DataView)                                                                                      \
  V(Date)                                                                                          \
  V(External)                                                                                      \
  V(Float16Array)                                                                                  \
  V(Float32Array)                                                                                  \
  V(Float64Array)                                                                                  \
  V(GeneratorFunction)                                                                             \
  V(GeneratorObject)                                                                               \
  V(Int8Array)                                                                                     \
  V(Int16Array)                                                                                    \
  V(Int32Array)                                                                                    \
  V(Map)                                                                                           \
  V(MapIterator)                                                                                   \
  V(ModuleNamespaceObject)                                                                         \
  V(NativeError)                                                                                   \
  V(NumberObject)                                                                                  \
  V(Promise)                                                                                       \
  V(Proxy)                                                                                         \
  V(RegExp)                                                                                        \
  V(Set)                                                                                           \
  V(SetIterator)                                                                                   \
  V(SharedArrayBuffer)                                                                             \
  V(StringObject)                                                                                  \
  V(SymbolObject)                                                                                  \
  V(TypedArray)                                                                                    \
  V(Uint8Array)                                                                                    \
  V(Uint8ClampedArray)                                                                             \
  V(Uint16Array)                                                                                   \
  V(Uint32Array)                                                                                   \
  V(WeakMap)                                                                                       \
  V(WeakSet)

class UtilModule final: public jsg::Object {
 public:
  UtilModule() = default;
  UtilModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Name getResourceTypeInspect(jsg::Lock& js);

  // `getOwnNonIndexProperties()` `filter`s
  static constexpr int ALL_PROPERTIES = jsg::PropertyFilter::ALL_PROPERTIES;
  static constexpr int ONLY_ENUMERABLE = jsg::PropertyFilter::ONLY_ENUMERABLE;

  jsg::JsArray getOwnNonIndexProperties(jsg::Lock& js, jsg::JsObject value, int filter);

  // `PromiseDetails` `state`s
  static constexpr int kPending = jsg::PromiseState::PENDING;
  static constexpr int kFulfilled = jsg::PromiseState::FULFILLED;
  static constexpr int kRejected = jsg::PromiseState::REJECTED;

  struct PromiseDetails {
    int state;  // TODO: can we make this a `jsg::PromiseState`
    jsg::Optional<jsg::JsRef<jsg::JsValue>> result;

    JSG_STRUCT(state, result);
  };
  jsg::Optional<PromiseDetails> getPromiseDetails(jsg::Lock& js, jsg::JsValue value);

  struct ProxyDetails {
    jsg::JsRef<jsg::JsValue> target;
    jsg::JsRef<jsg::JsValue> handler;

    JSG_STRUCT(target, handler);
  };
  jsg::Optional<ProxyDetails> getProxyDetails(jsg::Lock& js, jsg::JsValue value);

  struct PreviewedEntries {
    jsg::JsRef<jsg::JsArray> entries;
    bool isKeyValue;

    JSG_STRUCT(entries, isKeyValue);
  };
  jsg::Optional<PreviewedEntries> previewEntries(jsg::Lock& js, jsg::JsValue value);

  jsg::JsString getConstructorName(jsg::Lock& js, jsg::JsObject value);

  struct CallSiteEntry {
    kj::String functionName;
    kj::String scriptName;
    int lineNumber;
    // Node.js originally introduced the API with the name `getCallSite()` as an experimental
    // API but then renamed it to `getCallSites()` soon after. We had already implemented the
    // API with the original name in a release. To avoid the possibility of breaking, we export
    // the function using both names.
    int columnNumber;
    int column;

    JSG_STRUCT(functionName, scriptName, lineNumber, columnNumber, column);
  };
  kj::Array<CallSiteEntry> getCallSites(jsg::Lock& js, jsg::Optional<int> frames);

#define V(Type) bool is##Type(jsg::JsValue value);
  JS_UTIL_IS_TYPES(V)
#undef V
  bool isAnyArrayBuffer(jsg::JsValue value);
  bool isBoxedPrimitive(jsg::JsValue value);

  JSG_RESOURCE_TYPE(UtilModule) {
    JSG_READONLY_INSTANCE_PROPERTY(kResourceTypeInspect, getResourceTypeInspect);

    JSG_STATIC_CONSTANT(ALL_PROPERTIES);
    JSG_STATIC_CONSTANT(ONLY_ENUMERABLE);
    JSG_METHOD(getOwnNonIndexProperties);

    JSG_STATIC_CONSTANT(kPending);
    JSG_STATIC_CONSTANT(kFulfilled);
    JSG_STATIC_CONSTANT(kRejected);
    JSG_METHOD(getPromiseDetails);

    JSG_METHOD(getProxyDetails);
    JSG_METHOD(previewEntries);
    JSG_METHOD(getConstructorName);
    JSG_METHOD(getCallSites);

    JSG_NESTED_TYPE(Performance);
    JSG_NESTED_TYPE(PerformanceEntry);
    JSG_NESTED_TYPE(PerformanceMeasure);
    JSG_NESTED_TYPE(PerformanceMark);
    JSG_NESTED_TYPE(PerformanceObserver);
    JSG_NESTED_TYPE(PerformanceObserverEntryList);
    JSG_NESTED_TYPE(PerformanceResourceTiming);

#define V(Type) JSG_METHOD(is##Type);
    JS_UTIL_IS_TYPES(V)
#undef V
    JSG_METHOD(isAnyArrayBuffer);
    JSG_METHOD(isBoxedPrimitive);
  }
};

#define EW_NODE_UTIL_ISOLATE_TYPES                                                                 \
  api::node::UtilModule, api::node::UtilModule::PromiseDetails,                                    \
      api::node::UtilModule::ProxyDetails, api::node::UtilModule::PreviewedEntries,                \
      api::node::UtilModule::CallSiteEntry

}  // namespace workerd::api::node
