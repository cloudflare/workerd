// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUSupportedFeatures : public jsg::Object {
public:
  explicit GPUSupportedFeatures(std::vector<wgpu::FeatureName> features);
  JSG_RESOURCE_TYPE(GPUSupportedFeatures) {
    JSG_METHOD(has);
    JSG_METHOD(keys);
  }

private:
  kj::HashSet<GPUFeatureName> enabled_;
  bool has(kj::String name);
  kj::Array<kj::StringPtr> keys();
};

} // namespace workerd::api::gpu
