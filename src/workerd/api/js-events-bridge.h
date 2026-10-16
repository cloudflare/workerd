// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/common.h>

namespace workerd::api::events {

// Shared plumbing for the C++ side of the TypeScript events bridge: the dispatch helpers
// C++ code uses to call into the implementation in src/per_isolate/events/. The bootstrap
// module these helpers reach for exists only when the typescript-eventtarget autogate is
// enabled.

// Fetches a named function export of the events/cpp_exports bootstrap module. The module
// is eagerly required by the bootstrap when the typescript-eventtarget autogate is
// enabled, so lookups can only fail on internal errors; the resulting "internal error"
// surfaced to user code is intended.
jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name);

// Calls the named events/cpp_exports function with undefined as the receiver.
//
// The assert turns a dispatch attempt inside a no-JS scope (e.g. V8's deserializer) into a
// named failure rather than an unsymbolizable V8 fatal ("Invoke in
// DisallowJavascriptExecutionScope").
template <jsg::IsJsValue... Args>
jsg::JsValue dispatchCall(jsg::Lock& js, kj::StringPtr name, Args... args) {
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to dispatch into the TypeScript events implementation during a no-JS scope",
      name);
  auto func = getCppExport(js, name);
  return func.call(js, js.undefined(), kj::fwd<Args>(args)...);
}

}  // namespace workerd::api::events
