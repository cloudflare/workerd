// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/mimetype.h>

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

class MIMEType;

class MIMEParams final: public jsg::Object {
private:
  template <typename T>
  struct IteratorState final {
    kj::Array<T> values;
    uint index = 0;
  };

public:
  MIMEParams(kj::Maybe<MimeType&> mimeType = kj::none);

  static jsg::Ref<MIMEParams> constructor();

  void delete_(kj::String name);
  kj::Maybe<kj::StringPtr> get(kj::String name);
  bool has(kj::String name);
  void set(kj::String name, kj::String value);
  kj::String toString();

  JSG_ITERATOR(EntryIterator,
      entries,
      kj::Array<kj::String>,
      IteratorState<kj::Array<kj::String>>,
      iteratorNext<kj::Array<kj::String>>);
  JSG_ITERATOR(KeyIterator, keys, kj::String, IteratorState<kj::String>, iteratorNext<kj::String>);
  JSG_ITERATOR(
      ValueIterator, values, kj::String, IteratorState<kj::String>, iteratorNext<kj::String>);

  JSG_RESOURCE_TYPE(MIMEParams) {
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(get);
    JSG_METHOD(has);
    JSG_METHOD(set);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_METHOD(toString);
    JSG_METHOD_NAMED(toJSON, toString);
    JSG_ITERABLE(entries);
  }

private:
  template <typename T>
  static kj::Maybe<T> iteratorNext(jsg::Lock& js, IteratorState<T>& state) {
    if (state.index >= state.values.size()) {
      return kj::none;
    }
    auto& item = state.values[state.index++];
    if constexpr (kj::isSameType<T, kj::Array<kj::String>>()) {
      return KJ_MAP(i, item) { return kj::str(i); };
    } else {
      static_assert(kj::isSameType<T, kj::String>());
      return kj::str(item);
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<MimeType&> mimeType;
  friend class MIMEType;
};

class MIMEType final: public jsg::Object {
public:
  explicit MIMEType(MimeType inner);
  ~MIMEType() noexcept(false);
  static jsg::Ref<MIMEType> constructor(kj::String input);

  kj::StringPtr getType();
  void setType(kj::String type);
  kj::StringPtr getSubtype();
  void setSubtype(kj::String subtype);
  kj::String getEssence();
  jsg::Ref<MIMEParams> getParams();
  kj::String toString();

  JSG_RESOURCE_TYPE(MIMEType) {
    JSG_PROTOTYPE_PROPERTY(type, getType, setType);
    JSG_PROTOTYPE_PROPERTY(subtype, getSubtype, setSubtype);
    JSG_READONLY_PROTOTYPE_PROPERTY(essence, getEssence);
    JSG_READONLY_PROTOTYPE_PROPERTY(params, getParams);
    JSG_METHOD(toString);
    JSG_METHOD_NAMED(toJSON, toString);
  }

private:
  workerd::MimeType inner;
  jsg::Ref<MIMEParams> params;
};

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

  struct CallSiteEntry {
    kj::String functionName;
    kj::String scriptName;
    int lineNumber;
    int column;

    JSG_STRUCT(functionName, scriptName, lineNumber, column);
  };
  kj::Array<CallSiteEntry> getCallSite(jsg::Lock& js, int frames);

#define V(Type) bool is##Type(jsg::JsValue value);
  JS_UTIL_IS_TYPES(V)
#undef V
  bool isAnyArrayBuffer(jsg::JsValue value);
  bool isBoxedPrimitive(jsg::JsValue value);

  jsg::JsValue getBuiltinModule(jsg::Lock& js, kj::String specifier);

  // This is used in the implementation of process.exit(...). Contrary
  // to what the name suggests, it does not actually exit the process.
  // Instead, it will cause the IoContext, if any, and will stop javascript
  // from further executing in that request. If there is no active IoContext,
  // then it becomes a non-op.
  void processExitImpl(jsg::Lock& js, int code);

  JSG_RESOURCE_TYPE(UtilModule) {
    JSG_NESTED_TYPE(MIMEType);
    JSG_NESTED_TYPE(MIMEParams);

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
    JSG_METHOD(getCallSite);

#define V(Type) JSG_METHOD(is##Type);
    JS_UTIL_IS_TYPES(V)
#undef V
    JSG_METHOD(isAnyArrayBuffer);
    JSG_METHOD(isBoxedPrimitive);

    JSG_METHOD(getBuiltinModule);
    JSG_METHOD(processExitImpl);
  }
};

#define EW_NODE_UTIL_ISOLATE_TYPES                                                                 \
  api::node::UtilModule, api::node::UtilModule::PromiseDetails,                                    \
      api::node::UtilModule::ProxyDetails, api::node::UtilModule::PreviewedEntries,                \
      api::node::MIMEType, api::node::MIMEParams, api::node::MIMEParams::EntryIterator,            \
      api::node::MIMEParams::ValueIterator, api::node::MIMEParams::KeyIterator,                    \
      api::node::MIMEParams::EntryIterator::Next, api::node::MIMEParams::ValueIterator::Next,      \
      api::node::MIMEParams::KeyIterator::Next, api::node::UtilModule::CallSiteEntry

}  // namespace workerd::api::node
