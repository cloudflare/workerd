// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/node/node.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <cloudflare/cloudflare.capnp.h>

namespace workerd::api {

template <class Registry>
void registerModules(Registry& registry, auto featureFlags) {
  if (featureFlags.getNodeJsCompat()) {
    node::registerNodeJsCompatModules(registry, featureFlags);
  }
  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }
  registerSocketsModule(registry, featureFlags);
  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);
}

}  // namespace workerd::api
