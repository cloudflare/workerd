// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-bindgroup-layout.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUPipelineLayout: public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::PipelineLayout&() const {
    return layout_;
  }
  explicit GPUPipelineLayout(wgpu::PipelineLayout l): layout_(kj::mv(l)) {};
  JSG_RESOURCE_TYPE(GPUPipelineLayout) {}

private:
  wgpu::PipelineLayout layout_;
};

struct GPUPipelineLayoutDescriptor {
  jsg::Optional<kj::String> label;
  kj::Array<jsg::Ref<GPUBindGroupLayout>> bindGroupLayouts;

  JSG_STRUCT(label, bindGroupLayouts);
};

using GPUPipelineLayoutBase = kj::OneOf<jsg::NonCoercible<kj::String>, jsg::Ref<GPUPipelineLayout>>;

}  // namespace workerd::api::gpu
