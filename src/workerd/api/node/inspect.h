// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

#define JS_INSPECT_IS_TYPES(V) \
  V(ArrayBufferView) \
  V(ArgumentsObject) \
  V(ArrayBuffer) \
  V(AsyncFunction) \
  V(BigInt64Array) \
  V(BigIntObject) \
  V(BigUint64Array) \
  V(BooleanObject) \
  V(DataView) \
  V(Date) \
  V(Float32Array) \
  V(Float64Array) \
  V(GeneratorFunction) \
  V(GeneratorObject) \
  V(Int8Array) \
  V(Int16Array) \
  V(Int32Array) \
  V(Map) \
  V(MapIterator) \
  V(ModuleNamespaceObject) \
  V(NativeError) \
  V(NumberObject) \
  V(Promise) \
  V(Proxy) \
  V(RegExp) \
  V(Set) \
  V(SetIterator) \
  V(SharedArrayBuffer) \
  V(StringObject) \
  V(SymbolObject) \
  V(TypedArray) \
  V(Uint8Array) \
  V(Uint8ClampedArray) \
  V(Uint16Array) \
  V(Uint32Array) \
  V(WeakMap) \
  V(WeakSet)

// Implements supporting utilities for Node's `util.inspect()` function
class InspectModule final: public jsg::Object {
public:
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
    int state; // TODO: can we make this a `jsg::PromiseState`
    jsg::Optional<jsg::JsValue> result;

    JSG_STRUCT(state, result);
  };
  jsg::Optional<PromiseDetails> getPromiseDetails(jsg::JsValue value);

  struct ProxyDetails {
    jsg::JsValue target;
    jsg::JsValue handler;

    JSG_STRUCT(target, handler);
  };
  jsg::Optional<ProxyDetails> getProxyDetails(jsg::JsValue value);

  struct PreviewedEntries {
    jsg::JsArray entries;
    bool isKeyValue;

    JSG_STRUCT(entries, isKeyValue);
  };
  jsg::Optional<PreviewedEntries> previewEntries(jsg::JsValue value);

  jsg::JsString getConstructorName(jsg::Lock& js, jsg::JsObject value);

#define V(Type) bool is##Type(jsg::JsValue value);
  JS_INSPECT_IS_TYPES(V)
#undef V
  bool isAnyArrayBuffer(jsg::JsValue value);
  bool isBoxedPrimitive(jsg::JsValue value);

  JSG_RESOURCE_TYPE(InspectModule) {
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

  #define V(Type) JSG_METHOD(is##Type);
    JS_INSPECT_IS_TYPES(V)
  #undef V
    JSG_METHOD(isAnyArrayBuffer);
    JSG_METHOD(isBoxedPrimitive);
  }
};

#define EW_NODE_INSPECT_ISOLATE_TYPES \
    api::InspectModule,                 \
    api::InspectModule::PromiseDetails, \
    api::InspectModule::ProxyDetails,   \
    api::InspectModule::PreviewedEntries

}
