// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/node/node.h>
#include <workerd/api/rtti.h>
#include <workerd/api/sockets.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules.h>
#include <cloudflare/cloudflare.capnp.h>
#include <kj/vector.h>

namespace workerd::api {

template <typename TypeWrapper>
kj::Array<kj::Own<jsg::modules::ModuleBundle>> registerModules(
    auto featureFlags,
    jsg::CompilationObserver& observer) {
  kj::Vector<kj::Own<jsg::modules::ModuleBundle>> bundles;
  auto nodeModules = node::registerNodeJsCompatModules<TypeWrapper>(featureFlags, observer);
  for (auto& module : nodeModules) {
    bundles.add(kj::mv(module));
  }

  if (featureFlags.getRttiApi()) {
    bundles.add(registerRTTIModule<TypeWrapper>(observer));
  }
  bundles.add(registerSocketsModule<TypeWrapper>(featureFlags, observer));

  jsg::modules::BuiltinModuleBundleBuilder internalCloudflare(jsg::modules::Module::Type::INTERNAL,
                                                              observer);
  jsg::modules::BuiltinModuleBundleBuilder builtinCloudflare(jsg::modules::Module::Type::BUILTIN,
                                                             observer);
  internalCloudflare.add(CLOUDFLARE_BUNDLE);
  builtinCloudflare.add(CLOUDFLARE_BUNDLE);
  bundles.add(internalCloudflare.finish());
  bundles.add(builtinCloudflare.finish());
  return bundles.releaseAsArray();
}

void registerRtti(RttiRegistry& registry, auto featureFlags) {
  node::nodeRegisterRtti(registry, featureFlags);

  if (featureFlags.getRttiApi()) {
    rttiRegisterRtti(registry, featureFlags);
  }

  socketsRegisterRtti(registry, featureFlags);

  registry.add(CLOUDFLARE_BUNDLE);
}

}  // namespace workerd::api
