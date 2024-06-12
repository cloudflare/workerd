// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

struct FeatureFlags {
  FeatureFlags() = delete;

  // Get the feature flags that are relevant for the current jsg::Lock or
  // throw if we are not currently executing JavaScript.
  static CompatibilityFlags::Reader get(jsg::Lock&);
};

}  // namespace workerd
