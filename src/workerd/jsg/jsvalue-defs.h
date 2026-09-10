#pragma once

#include <kj/common.h>

namespace workerd::jsg {

// Forward declarations of all the Js* types. Separated out for clarity since
// the definitions are used in several places and we want to avoid circular
// deps and defining them in weird places. The actual definitions are in
// jsvalue.h

template <typename T>
class JsRef;
class JsValue;
class JsMessage;
#define JS_TYPE_CLASSES(V)                                                                         \
  V(Object)                                                                                        \
  V(Boolean)                                                                                       \
  V(Array)                                                                                         \
  V(String)                                                                                        \
  V(Symbol)                                                                                        \
  V(BigInt)                                                                                        \
  V(Number)                                                                                        \
  V(Int32)                                                                                         \
  V(Uint32)                                                                                        \
  V(Date)                                                                                          \
  V(RegExp)                                                                                        \
  V(Map)                                                                                           \
  V(Set)                                                                                           \
  V(Promise)                                                                                       \
  V(Proxy)                                                                                         \
  V(Function)                                                                                      \
  V(Uint8Array)                                                                                    \
  V(ArrayBuffer)                                                                                   \
  V(ArrayBufferView)                                                                               \
  V(SharedArrayBuffer)

#define V(Name) class Js##Name;
JS_TYPE_CLASSES(V)
#undef V

// JsBufferSource is not in JS_TYPE_CLASSES because there is no v8::BufferSource
// type (and hence no v8::Value::IsBufferSource() check). It is instead handled
// with special-case logic in JsValue::tryCast and JsValueWrapper.
class JsBufferSource;

#define V(Name) || kj::isSameType<T, Js##Name>()
template <typename T>
concept IsJsValue = kj::isSameType<T, JsValue>() ||
    kj::isSameType<T, JsMessage>() JS_TYPE_CLASSES(V) || kj::isSameType<T, JsBufferSource>();
#undef V

}  // namespace workerd::jsg
