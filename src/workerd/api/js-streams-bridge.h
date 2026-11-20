// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/common.h>

namespace workerd::api::webstreams {

// Shared plumbing for the C++ side of the TypeScript webstreams bridge: the dispatch
// helpers JsReadableStream / JsWritableStream (and their native source/sink classes) use
// to call into the implementation in src/per_isolate/webstreams/. The bootstrap module
// these helpers reach for exists only when the typescript_implemented_streams compat flag
// is enabled.
//
// This header is internal to the stream bridge files (and their tests); it is not part of
// the consumer-facing streams surface and is deliberately not re-exported via
// api/streams.h.

// Fetches a named function export of the webstreams/cpp_exports bootstrap module. The
// module is eagerly required by the bootstrap when the typescript_implemented_streams
// compat flag is enabled, so lookups can only fail on internal errors; the resulting
// "internal error" surfaced to user code is intended.
jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name);

// Calls the named webstreams/cpp_exports function with undefined as the receiver.
//
// The assert catches dispatch attempts inside a no-JS scope (most importantly V8's
// deserializer, which forbids JS execution for the whole value-graph read) and turns what
// would be an unsymbolizable V8 fatal ("Invoke in DisallowJavascriptExecutionScope") into a
// named failure. Callers that can legitimately be reached inside such a scope must answer
// without dispatching -- see e.g. the state-probe suppression in JsReadableStream::
// isDisturbed()/isLocked(), which rely on streams in such scopes being hydration-fresh.
template <jsg::IsJsValue... Args>
jsg::JsValue dispatchCall(jsg::Lock& js, kj::StringPtr name, Args... args) {
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to dispatch into the TypeScript streams implementation during a no-JS scope",
      name);
  auto func = getCppExport(js, name);
  return func.call(js, js.undefined(), kj::fwd<Args>(args)...);
}

// Calls the named method on the given object, with the object itself as the receiver.
// Used to invoke the TypeScript conduit's controller facade methods (enqueue, close,
// respond, ...). The facade objects are module-owned TypeScript code, not user objects,
// so a missing method indicates an internal error. Carries the same no-JS-scope assert as
// dispatchCall(), for the same reason.
template <jsg::IsJsValue... Args>
jsg::JsValue invokeMethod(jsg::Lock& js, jsg::JsObject obj, kj::StringPtr name, Args... args) {
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to invoke a TypeScript streams method during a no-JS scope", name);
  auto func =
      KJ_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(obj.get(js, name)), "method not found", name);
  return func.call(js, obj, args...);
}

// A TypeScript stream built over a C++ source or sink (JsReadableStream::from() and fromPull(),
// JsWritableStream::fromWrite()) is constructed over a JS underlying source or sink object that
// carries the C++ object under a private name and whose hooks are functions over static
// callbacks. The stream calls a hook with the object as `this`, through which the callback
// reaches the C++ object. The functions carry no data: a startup snapshot serializes a
// function's data at isolate level, where context objects are not allowed.

// The private name under which such an object carries its C++ object.
constexpr auto NATIVE_BACKING_KEY = "workerd::api::webstreams::nativeBacking"_kj;

// Makes `backing` the C++ object of `object`.
template <typename T>
void setNativeBacking(jsg::Lock& js, jsg::JsObject object, T backing) {
  object.setPrivate(js, NATIVE_BACKING_KEY, jsg::JsValue(js.opaque(kj::mv(backing))));
}

// The C++ object of the object the hook `info` describes was called on.
template <typename T>
T& getNativeBacking(jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto self = JSG_REQUIRE_NONNULL(
      jsg::JsValue(info.This()).tryCast<jsg::JsObject>(), TypeError, "Illegal invocation");
  auto backing = self.getPrivate(js, NATIVE_BACKING_KEY);
  JSG_REQUIRE(!backing.isUndefined(), TypeError, "Illegal invocation");
  return jsg::unwrapOpaqueRef<T>(js.v8Isolate, backing);
}

// A hook of such an object: a function over `callback`, which is registered as a snapshot
// external reference, since a stream made at a Worker's top level is alive when its startup
// snapshot is taken.
jsg::JsValue newNativeHook(jsg::Lock& js, v8::FunctionCallback callback);

// Convert a kj::String to owned bytes, excluding the trailing NUL terminator. The returned
// array owns the original character buffer but views only the string's bytes.
kj::Array<kj::byte> stringToBytes(kj::String data);

}  // namespace workerd::api::webstreams
