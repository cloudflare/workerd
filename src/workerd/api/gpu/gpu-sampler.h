// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUSampler : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::Sampler &() const { return sampler_; }

  explicit GPUSampler(wgpu::Sampler s) : sampler_(s){};
  JSG_RESOURCE_TYPE(GPUSampler) {}

private:
  wgpu::Sampler sampler_;
};

struct GPUSamplerDescriptor {
  jsg::Optional<kj::String> label;
  jsg::Optional<GPUAddressMode> addressModeU;
  jsg::Optional<GPUAddressMode> addressModeV;
  jsg::Optional<GPUAddressMode> addressModeW;
  jsg::Optional<GPUFilterMode> magFilter;
  jsg::Optional<GPUFilterMode> minFilter;
  jsg::Optional<GPUMipmapFilterMode> mipmapFilter;
  jsg::Optional<double> lodMinClamp;
  jsg::Optional<double> lodMaxClamp;
  GPUCompareFunction compare;
  jsg::Optional<uint16_t> maxAnisotropy;

  JSG_STRUCT(label, addressModeU, addressModeV, addressModeW, magFilter,
             minFilter, mipmapFilter, lodMinClamp, lodMaxClamp, compare,
             maxAnisotropy);
};

} // namespace workerd::api::gpu
