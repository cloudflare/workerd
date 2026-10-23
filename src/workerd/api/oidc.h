// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The selector identifies one approved assignment. The host checks current execution on each use.
class OidcIdentityBinding: public jsg::Object {
 public:
  explicit OidcIdentityBinding(kj::String assignment): assignment(kj::mv(assignment)) {}

  jsg::Promise<kj::String> getToken(jsg::Lock& js, jsg::Optional<jsg::JsValue> options);

  JSG_RESOURCE_TYPE(OidcIdentityBinding) {
    JSG_METHOD(getToken);
    JSG_TS_ROOT();
    JSG_TS_OVERRIDE(OidcIdentityBinding {
      getToken(options?: { audience?: string; fresh?: boolean }): Promise<string>;
    });
  }

 private:
  kj::String assignment;
};

}  // namespace workerd::api
