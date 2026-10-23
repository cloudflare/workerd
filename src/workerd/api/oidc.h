// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Resolves identity through the current IoContext at call time, including when a handle is reused.
class OidcContext: public jsg::Object {
 public:
  struct TokenOptions {
    // Prevent arrays and other values from silently becoming an audience or subject string.
    jsg::Optional<jsg::NonCoercible<kj::String>> audience;
    jsg::Optional<jsg::NonCoercible<kj::String>> subject;

    JSG_STRUCT(audience, subject);
    JSG_STRUCT_TS_OVERRIDE(CloudflareOidcTokenOptions {
      audience?: string;
      subject?: "object" | "namespace";
    });
  };

  // The audience is an identifier, never a URL to fetch. Omission selects the account default.
  jsg::Promise<kj::String> getToken(jsg::Lock& js, jsg::Optional<TokenOptions> options);

  JSG_RESOURCE_TYPE(OidcContext) {
    JSG_METHOD(getToken);
    JSG_TS_OVERRIDE(CloudflareOidcContext);
  }
};

}  // namespace workerd::api
