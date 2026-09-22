// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

// Selects how string chunks are encoded. NO gives WTF-8, which encodes a lone
// surrogate literally; YES substitutes U+FFFD for it. This is the crypto-free
// spelling of DigestStringEncoding (crypto.h), which createDigestContext()
// translates to; the two exist separately so this header can stay free of
// crypto dependencies.
WD_STRONG_BOOL(ToWellFormed);

// Creates the native digest object that backs the TypeScript DigestStream. The
// result exposes update(chunk) and digest() to JavaScript; see
// DigestContextHandle in crypto.h for their semantics.
//
// Throws a DOMNotSupportedError if the algorithm name is not recognized, which
// is what gives the TypeScript DigestStream constructor the same error type and
// message as the C++ one.
//
// This declaration deliberately mentions no crypto types, so the per-isolate
// bootstrap can reach the factory without depending on the crypto headers.
jsg::JsValue createDigestContext(jsg::Lock& js, kj::StringPtr algorithm, ToWellFormed toWellFormed);

}  // namespace workerd::api
