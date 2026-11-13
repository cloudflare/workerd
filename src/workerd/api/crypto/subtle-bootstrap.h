// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Creates the C++ SubtleCrypto (crypto.h) whose backend members back the TypeScript SubtleCrypto.
// The per-isolate bootstrap creates one per context under the ts-webcrypto autogate and passes it
// to its scripts as the `cryptoBackend` pseudo-global.
//
// This declaration deliberately mentions no crypto types, so the per-isolate bootstrap can reach
// the factory without depending on the crypto headers.
jsg::JsValue createSubtleCryptoBackend(jsg::Lock& js);

}  // namespace workerd::api
