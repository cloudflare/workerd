// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

struct FeatureFlags {
  FeatureFlags() = delete;

  // Get the feature flags that are relevant for the current jsg::Lock or
  // throw if we are not currently executing JavaScript.
  static CompatibilityFlags::Reader get(jsg::Lock&);

  // Alternative to get() that returns kj::none if the flags are not available.
  // This is typically only the case in certain tests where we may only partially
  // initialize the JS environment.
  static kj::Maybe<CompatibilityFlags::Reader> tryGet(jsg::Lock&);
};

// Returns whether the new module registry implementation should be used for a
// worker with the given flags. Python workers always use the original module
// registry: the new_module_registry flag is ignored for them, whether it was
// listed explicitly or (once the flag gains an enable date) implied by the
// worker's compatibility date. Every decision about which registry to use MUST
// go through this function rather than reading getNewModuleRegistry() directly;
// otherwise a Python worker could end up split across the two registries.
//
// Note that Cloudflare.compatibilityFlags.new_module_registry (as observed by
// user code) still reflects the configured flag value, not this function's
// result; the flag describes the configuration while this function describes
// the behavior.
inline bool isNewModuleRegistryEnabled(const CompatibilityFlags::Reader& flags) {
  return flags.getNewModuleRegistry() && !flags.getPythonWorkers();
}

}  // namespace workerd
