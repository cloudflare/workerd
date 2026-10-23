// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "oidc.h"

#include <workerd/io/io-context.h>
#include <workerd/util/oidc.h>

namespace workerd::api {

jsg::Promise<kj::String> OidcIdentityBinding::getToken(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> options) {
  auto& context = IoContext::current();
  kj::Maybe<kj::String> audience;
  bool fresh = false;
  KJ_IF_SOME(value, options) {
    auto opts = JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsObject>(), TypeError,
        "OIDC options must be an object containing only audience and fresh.");
    auto keys = opts.getPropertyNames(js, jsg::KeyCollectionFilter::OWN_ONLY,
        jsg::PropertyFilter::ALL_PROPERTIES, jsg::IndexFilter::INCLUDE_INDICES);
    JSG_REQUIRE(keys.size() <= 2, TypeError, "Unknown OIDC option. Use only audience and fresh.");
    for (uint32_t i = 0; i < keys.size(); ++i) {
      auto key = JSG_REQUIRE_NONNULL(keys.get(js, i).tryCast<jsg::JsString>(), TypeError,
          "Unknown OIDC option. Use only audience and fresh.");
      JSG_REQUIRE(
          key.length(js) <= 8, TypeError, "Unknown OIDC option. Use only audience and fresh.");
      auto name = key.toString(js);
      JSG_REQUIRE(name == "audience" || name == "fresh", TypeError,
          "Unknown OIDC option. Use only audience and fresh.");
      auto option = opts.get(js, name);
      if (name == "audience") {
        if (option.isUndefined()) continue;
        auto string = JSG_REQUIRE_NONNULL(
            option.tryCast<jsg::JsString>(), TypeError, "OIDC audience must be a string.");
        JSG_REQUIRE(
            string.length(js) > 0 && string.length(js) <= 2048 && string.utf8Length(js) <= 2048,
            TypeError, "OIDC audience must contain 1–2048 UTF-8 bytes.");
        auto text = string.toString(js);
        JSG_REQUIRE(isValidOidcAudience(text), TypeError,
            "OIDC audience must be a non-whitespace StringOrURI of 1–2048 UTF-8 bytes; "
            "values containing ':' must be valid URIs.");
        audience = kj::mv(text);
      } else {
        auto boolean = JSG_REQUIRE_NONNULL(option.tryCast<jsg::JsBoolean>(), TypeError,
            "OIDC fresh must be a boolean. Use true to request a new assertion.");
        fresh = boolean.value(js);
      }
    }
  }
  return context.awaitIo(js,
      context.getIoChannelFactory().getOidcToken(
          {.assignment = kj::str(assignment), .audience = kj::mv(audience), .fresh = fresh}));
}

}  // namespace workerd::api
