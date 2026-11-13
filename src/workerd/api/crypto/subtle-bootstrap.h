// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Creates the native SubtleCryptoBackend (crypto.h) that backs the TypeScript SubtleCrypto,
// installed by the per-isolate bootstrap under the ts-webcrypto autogate.
//
// This declaration deliberately mentions no crypto types, so the per-isolate bootstrap can reach
// the factory without depending on the crypto headers.
jsg::JsValue createSubtleCryptoBackend(jsg::Lock& js);

}  // namespace workerd::api
