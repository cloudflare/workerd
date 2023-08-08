// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-supported-features.h"

namespace workerd::api::gpu {

GPUSupportedFeatures::GPUSupportedFeatures(
    kj::Array<wgpu::FeatureName> features) {
  for (wgpu::FeatureName feature : features) {
    // add only known features to the feature list
    KJ_IF_MAYBE (knownF, getFeatureName(feature)) {
      enabled_.insert(kj::mv(*knownF));
    }
  }
}

bool GPUSupportedFeatures::has(kj::String name) {
  return enabled_.contains(name);
}

kj::Array<kj::StringPtr> GPUSupportedFeatures::keys() {
  kj::Vector<kj::StringPtr> res(enabled_.size());
  res.addAll(enabled_);
  return res.releaseAsArray();
}

} // namespace workerd::api::gpu
