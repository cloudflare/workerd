// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-streams-bridge.h>
#include <workerd/io/per-isolate-bootstrap.h>

namespace workerd::api::webstreams {

jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name) {
  // Every bridge path that invokes the TypeScript implementation obtains its function here
  // (including constructor invocations that don't go through dispatchCall), so this is the
  // complete chokepoint for the no-JS-scope backstop: callers are about to execute JS, which
  // is forbidden e.g. during RPC deserialization. See the fuller comment on dispatchCall().
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to call into the TypeScript streams implementation during a no-JS scope", name);
  auto cppExports = KJ_REQUIRE_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
  auto cppExportsObj = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(cppExports));
  return KJ_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(cppExportsObj.get(js, name)));
}

kj::Array<kj::byte> stringToBytes(kj::String data) {
  size_t len = data.size();
  auto chars = data.releaseArray();
  return chars.first(len).asBytes().attach(kj::mv(chars));
}

}  // namespace workerd::api::webstreams
