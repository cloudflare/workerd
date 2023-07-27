// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-utils.h"

namespace workerd::api::gpu {

wgpu::FeatureName parseFeatureName(GPUFeatureName &str) {
  if (str == "depth-clip-control") {
    return wgpu::FeatureName::DepthClipControl;
  }
  if (str == "depth32float-stencil8") {
    return wgpu::FeatureName::Depth32FloatStencil8;
  }
  if (str == "texture-compression-bc") {
    return wgpu::FeatureName::TextureCompressionBC;
  }
  if (str == "texture-compression-etc2") {
    return wgpu::FeatureName::TextureCompressionETC2;
  }
  if (str == "texture-compression-astc") {
    return wgpu::FeatureName::TextureCompressionASTC;
  }
  if (str == "timestamp-query") {
    return wgpu::FeatureName::TimestampQuery;
  }
  if (str == "indirect-first-instance") {
    return wgpu::FeatureName::IndirectFirstInstance;
  }
  if (str == "shader-f16") {
    return wgpu::FeatureName::ShaderF16;
  }
  if (str == "rg11b10ufloat-renderable") {
    return wgpu::FeatureName::RG11B10UfloatRenderable;
  }
  if (str == "bgra8unorm-storage") {
    return wgpu::FeatureName::BGRA8UnormStorage;
  }
  if (str == "float32-filterable") {
    return wgpu::FeatureName::Float32Filterable;
  }

  KJ_FAIL_REQUIRE("unknown GPU feature", str);
}

} // namespace workerd::api::gpu
