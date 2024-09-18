// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUTextureView: public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::TextureView&() const {
    return textureView_;
  }
  explicit GPUTextureView(wgpu::TextureView t): textureView_(kj::mv(t)) {};
  JSG_RESOURCE_TYPE(GPUTextureView) {}

private:
  wgpu::TextureView textureView_;
};

struct GPUTextureViewDescriptor {
  kj::String label;
  GPUTextureFormat format;
  GPUTextureViewDimension dimension;
  jsg::Optional<GPUTextureAspect> aspect;
  jsg::Optional<GPUIntegerCoordinate> baseMipLevel;
  GPUIntegerCoordinate mipLevelCount;
  jsg::Optional<GPUIntegerCoordinate> baseArrayLayer;
  GPUIntegerCoordinate arrayLayerCount;
  JSG_STRUCT(label,
      format,
      dimension,
      aspect,
      baseMipLevel,
      mipLevelCount,
      baseArrayLayer,
      arrayLayerCount);
};

}  // namespace workerd::api::gpu
