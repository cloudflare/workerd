// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/modules.h>
#include <cloudflare/cloudflare.capnp.h>
#include <node/node.capnp.h>
#include <pyodide/pyodide.capnp.h>
#include <workerd/api/rtti.h>

namespace workerd::api {

template <class Registry>
void registerModulesBundles(Registry& registry, auto featureFlags) {
  // Module bundles are registered separately from C++ modules so we can type check bundles against
  // automatically generated "internal" types. See the `//types:types_internal` target for details.

  if (featureFlags.getRttiApi()) {
    registerRTTIModule(registry);
  }

  registry.addBuiltinBundle(CLOUDFLARE_BUNDLE);

  // If the `nodejs_compat` flag isn't enabled, only register internal modules.
  // We need these for `console.log()`ing when running `workerd` locally.
  kj::Maybe<jsg::ModuleType> maybeFilter;
  if (!featureFlags.getNodeJsCompat()) maybeFilter = jsg::ModuleType::INTERNAL;

  registry.addBuiltinBundle(NODE_BUNDLE, maybeFilter);

  // If the `nodejs_compat` flag is off, but the `nodejs_als` flag is on, we
  // need to register the `node:async_hooks` module from the bundle.
  if (!featureFlags.getNodeJsCompat() && featureFlags.getNodeJsAls()) {
    jsg::Bundle::Reader reader = NODE_BUNDLE;
    for (auto module : reader.getModules()) {
      auto specifier = module.getName();
      if (specifier == "node:async_hooks") {
        KJ_DASSERT(module.getType() == jsg::ModuleType::BUILTIN);
        registry.addBuiltinModule(module);
      }
    }
  }

  if (featureFlags.getPythonWorkers()) {
    // We add `pyodide:` packages here including python-entrypoint-helper.js.
    registry.addBuiltinBundle(PYODIDE_BUNDLE, kj::none);
  }
}

}  // namespace workerd::api
