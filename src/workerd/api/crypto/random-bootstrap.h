// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The native halves of a `crypto` global implemented in the per-isolate bootstrap. Each reads the
// current request's entropy source, and throws outside a request, as the C++ Crypto's methods
// do.
//
// Like subtle-bootstrap.h, these declarations mention no crypto types, so the per-isolate
// bootstrap can reach them without depending on the crypto headers.

// Fills `view`, as crypto.getRandomValues() does once it has checked its argument. The caller
// makes those checks itself.
void fillRandomValues(jsg::Lock& js, jsg::JsArrayBufferView view);

// A new random UUID, as crypto.randomUUID() returns.
jsg::JsString newRandomUUID(jsg::Lock& js);

}  // namespace workerd::api
