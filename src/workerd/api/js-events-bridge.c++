// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-events-bridge.h>
#include <workerd/api/per-isolate-bootstrap.h>

namespace workerd::api::events {

jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name) {
  // Every bridge path that invokes the TypeScript implementation obtains its function here,
  // so this is the complete chokepoint for the no-JS-scope backstop. See dispatchCall().
  KJ_ASSERT(!js.isJavascriptExecutionDisallowed(),
      "attempted to call into the TypeScript events implementation during a no-JS scope", name);
  auto cppExports = KJ_REQUIRE_NONNULL(tryGetBootstrapExport(js, "events/cpp_exports"));
  auto cppExportsObj = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(cppExports));
  return KJ_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(cppExportsObj.get(js, name)));
}

}  // namespace workerd::api::events
