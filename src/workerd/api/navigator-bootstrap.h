// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Creates the native half of the TypeScript Navigator (src/per_isolate/navigator.ts): a
// NavigatorNative, see global-scope.h for its methods.
//
// This declaration deliberately mentions no global scope types, so the per-isolate bootstrap can
// reach the factory without depending on global-scope.h.
jsg::JsValue createNavigatorNative(jsg::Lock& js);

}  // namespace workerd::api
