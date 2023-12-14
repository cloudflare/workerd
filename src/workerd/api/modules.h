// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/node/node.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <workerd/api/unsafe.h>
#include <workerd/io/worker.h>
#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

template <class Registry>
void registerModules(Registry& registry, auto featureFlags) {
  node::registerNodeJsCompatModules(registry, featureFlags);
  pyodide::registerPyodideModules(registry, featureFlags);
  registerUnsafeModules(registry, featureFlags);
  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }
  if (featureFlags.getUnsafeModule()) {
    registerUnsafeModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);
}

}  // namespace workerd::api
