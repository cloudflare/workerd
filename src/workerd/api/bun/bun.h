// Copyright (c) 2024 Jeju Network
// Bun Runtime Compatibility Layer for Workerd
// Licensed under the Apache 2.0 license

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/url.h>

#include <bun/bun.capnp.h>

#include <capnp/dynamic.h>

namespace workerd::api::bun {

// Helper to check if bun compat is enabled via feature flags
// Bun compatibility requires nodejs_compat since it builds on Node.js foundations
template <typename FeatureFlags>
constexpr bool isBunCompatEnabled(FeatureFlags featureFlags) {
  return featureFlags.getNodeJsCompat() || featureFlags.getNodeJsCompatV2();
}

// =============================================================================
// Module Registration for Original ModuleRegistry
// =============================================================================

template <class Registry>
void registerBunCompatModules(Registry& registry, auto featureFlags) {
  if (!isBunCompatEnabled(featureFlags)) {
    return;
  }

  // Register the Bun bundle containing JavaScript modules
  // BUN_BUNDLE is defined in the generated bun/bun.capnp.h header
  registry.addBuiltinBundle(BUN_BUNDLE);
}

// =============================================================================
// Module Registration for New ModuleRegistry
// =============================================================================

inline kj::Own<jsg::modules::ModuleBundle> getInternalBunCompatModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

  if (isBunCompatEnabled(featureFlags)) {
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, BUN_BUNDLE);
  }

  return builder.finish();
}

inline kj::Own<jsg::modules::ModuleBundle> getExternalBunCompatModuleBundle(auto featureFlags) {
  jsg::modules::ModuleBundle::BuiltinBuilder builder(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);

  if (isBunCompatEnabled(featureFlags)) {
    jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(builder, BUN_BUNDLE);
  }

  return builder.finish();
}

}  // namespace workerd::api::bun
