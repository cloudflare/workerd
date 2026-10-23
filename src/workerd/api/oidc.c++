// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "oidc.h"

#include <workerd/io/io-context.h>
#include <workerd/util/oidc.h>

namespace workerd::api {

jsg::Promise<kj::String> OidcContext::getToken(jsg::Lock& js, jsg::Optional<TokenOptions> options) {
  auto& context = IoContext::current();
  auto subject = context.getActor() == kj::none ? IoChannelFactory::OidcSubject::WORKER
                                                : IoChannelFactory::OidcSubject::DURABLE_OBJECT;
  kj::Maybe<kj::String> audience;

  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(option, opts.audience) {
      auto string = JSG_REQUIRE_NONNULL(option.getHandle(js).tryCast<jsg::JsString>(), TypeError,
          "OIDC audience must be a string.");
      JSG_REQUIRE(
          string.length(js) > 0 && string.length(js) <= 2048 && string.utf8Length(js) <= 2048,
          TypeError, "OIDC audience must contain 1–2048 UTF-8 bytes.");
      auto value = string.toString(js);
      JSG_REQUIRE(isValidOidcAudience(value), TypeError,
          "OIDC audience must be a non-whitespace StringOrURI of 1–2048 UTF-8 bytes; "
          "values containing ':' must be valid URIs.");
      audience = kj::mv(value);
    }
    KJ_IF_SOME(option, opts.subject) {
      auto string = JSG_REQUIRE_NONNULL(option.getHandle(js).tryCast<jsg::JsString>(), TypeError,
          "OIDC subject must be a string.");
      JSG_REQUIRE(
          string.length(js) <= 9, TypeError, "OIDC subject must be 'object' or 'namespace'.");
      auto value = string.toString(js);
      JSG_REQUIRE(value == "object" || value == "namespace", TypeError,
          "OIDC subject must be 'object' or 'namespace'.");
      JSG_REQUIRE(context.getActor() != kj::none, TypeError,
          "OIDC subject can only be selected inside a Durable Object.");
      if (value == "namespace") {
        subject = IoChannelFactory::OidcSubject::DURABLE_OBJECT_NAMESPACE;
      }
    }
  }

  return context.awaitIo(js,
      context.getIoChannelFactory().getOidcToken(
          {.audience = kj::mv(audience), .subject = subject}));
}

}  // namespace workerd::api
