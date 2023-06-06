#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

struct FeatureFlags {
  FeatureFlags() = delete;

  static CompatibilityFlags::Reader get(jsg::Lock&);
  // Get the feature flags that are relevant for the current jsg::Lock or
  // throw if we are not currently executing JavaScript.
};

}  // namespace workerd
