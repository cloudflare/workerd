// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-utils.h"

namespace workerd::api::gpu {

wgpu::FeatureName parseFeatureName(GPUFeatureName& str) {
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

  JSG_FAIL_REQUIRE(TypeError, "unknown GPU feature", str);
}

kj::Maybe<GPUFeatureName> getFeatureName(wgpu::FeatureName& feature) {
  switch (feature) {
  case wgpu::FeatureName::DepthClipControl:
    return kj::str("depth-clip-control");
  case wgpu::FeatureName::Depth32FloatStencil8:
    return kj::str("depth32float-stencil8");
  case wgpu::FeatureName::TextureCompressionBC:
    return kj::str("texture-compression-bc");
  case wgpu::FeatureName::TextureCompressionETC2:
    return kj::str("texture-compression-etc2");
  case wgpu::FeatureName::TextureCompressionASTC:
    return kj::str("texture-compression-astc");
  case wgpu::FeatureName::TimestampQuery:
    return kj::str("timestamp-query");
  case wgpu::FeatureName::IndirectFirstInstance:
    return kj::str("indirect-first-instance");
  case wgpu::FeatureName::ShaderF16:
    return kj::str("shader-f16");
  case wgpu::FeatureName::RG11B10UfloatRenderable:
    return kj::str("rg11b10ufloat-renderable");
  case wgpu::FeatureName::BGRA8UnormStorage:
    return kj::str("bgra8unorm-storage");
  case wgpu::FeatureName::Float32Filterable:
    return kj::str("float32-filterable");
  default:
    break;
  }

  return nullptr;
}

} // namespace workerd::api::gpu
