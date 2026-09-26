// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// The DOM's "add an algorithm to signal's abort algorithms" for the per-isolate
// bootstrap (utils.addAbortAlgorithm): `algorithm` is called with no arguments
// when `signal` aborts, before the 'abort' event is dispatched, and never for a
// synthetic 'abort' event. Returns an AbortAlgorithmHandle whose remove()
// unregisters it. See AbortSignal::addAbortAlgorithm() in basics.h.
//
// Throws a TypeError if `signal` is not an AbortSignal or `algorithm` is not a
// function. The caller is expected to have checked that the signal has not
// aborted: algorithms are never invoked retroactively.
jsg::JsValue addAbortAlgorithmForBootstrap(
    jsg::Lock& js, jsg::JsValue signal, jsg::JsValue algorithm);

}  // namespace workerd::api
