// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-pipeline-layout.h"
#include "gpu-shader-module.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUComputePipeline: public jsg::Object {
 public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::ComputePipeline&() const {
    return pipeline_;
  }
  explicit GPUComputePipeline(wgpu::ComputePipeline p): pipeline_(kj::mv(p)) {};
  JSG_RESOURCE_TYPE(GPUComputePipeline) {
    JSG_METHOD(getBindGroupLayout);
  }

 private:
  wgpu::ComputePipeline pipeline_;
  jsg::Ref<GPUBindGroupLayout> getBindGroupLayout(jsg::Lock& js, uint32_t index);
};

struct GPUComputePipelineDescriptor {
  jsg::Optional<kj::String> label;
  GPUProgrammableStage compute;
  GPUPipelineLayoutBase layout;

  JSG_STRUCT(label, compute, layout);
};

}  // namespace workerd::api::gpu
