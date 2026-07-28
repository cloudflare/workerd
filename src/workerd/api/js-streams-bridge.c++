// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-streams-bridge.h>
#include <workerd/io/per-isolate-bootstrap.h>

namespace workerd::api::webstreams {

jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name) {
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
