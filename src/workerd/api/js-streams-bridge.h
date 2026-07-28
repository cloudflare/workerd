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
// (plus the per-isolate bootstrap autogate) is enabled.
//
// This header is internal to the stream bridge files (and their tests); it is not part of
// the consumer-facing streams surface and is deliberately not re-exported via
// api/streams.h.

// Fetches a named function export of the webstreams/cpp_exports bootstrap module. The
// module is eagerly required by the bootstrap when the typescript_implemented_streams
// compat flag is enabled, so lookups can only fail on internal errors or gross
// misconfiguration (e.g. the flag enabled without the bootstrap autogate); the resulting
// "internal error" surfaced to user code is intended.
jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name);

// Calls the named webstreams/cpp_exports function with undefined as the receiver.
template <jsg::IsJsValue... Args>
jsg::JsValue dispatchCall(jsg::Lock& js, kj::StringPtr name, Args... args) {
  auto func = getCppExport(js, name);
  return func.call(js, js.undefined(), kj::fwd<Args>(args)...);
}

// Calls the named method on the given object, with the object itself as the receiver.
// Used to invoke the TypeScript conduit's controller facade methods (enqueue, close,
// respond, ...). The facade objects are module-owned TypeScript code, not user objects,
// so a missing method indicates an internal error.
template <jsg::IsJsValue... Args>
jsg::JsValue invokeMethod(jsg::Lock& js, jsg::JsObject obj, kj::StringPtr name, Args... args) {
  auto func =
      KJ_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(obj.get(js, name)), "method not found", name);
  return func.call(js, obj, args...);
}

// Convert a kj::String to owned bytes, excluding the trailing NUL terminator. The returned
// array owns the original character buffer but views only the string's bytes.
kj::Array<kj::byte> stringToBytes(kj::String data);

}  // namespace workerd::api::webstreams
