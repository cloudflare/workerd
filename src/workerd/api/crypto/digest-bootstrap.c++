// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "digest-bootstrap.h"

#include "crypto.h"

namespace workerd::api {

jsg::JsValue createDigestContext(jsg::Lock& js, kj::StringPtr algorithm) {
  // newDigestContext() validates the algorithm name and throws for unrecognized
  // ones, so this must run before the handle is allocated.
  auto context = newDigestContext(algorithm);

  // DigestContextHandle is registered in EW_CRYPTO_ISOLATE_TYPES, so the handler
  // is always present. Asserting rather than propagating keeps the caller (a raw
  // bootstrap callback) free of a failure mode it could not act on.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<DigestContextHandle>>(),
      "DigestContextHandle is missing from the isolate type list");

  return jsg::JsValue(handler.wrap(js, js.alloc<DigestContextHandle>(kj::mv(context))));
}

}  // namespace workerd::api
