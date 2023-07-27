// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-pipeline-layout.h"
#include "gpu-shader-module.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUComputePipeline : public jsg::Object {
public:
  explicit GPUComputePipeline(wgpu::ComputePipeline p) : pipeline_(kj::mv(p)){};
  JSG_RESOURCE_TYPE(GPUComputePipeline) {}

private:
  wgpu::ComputePipeline pipeline_;
};

using GPUPipelineConstantValue = double;

struct GPUProgrammableStage {
  jsg::Ref<GPUShaderModule> module;
  kj::String entryPoint;
  jsg::Optional<jsg::Dict<GPUPipelineConstantValue>> constants;

  JSG_STRUCT(module, entryPoint, constants);
};

using GPUComputePipelineLayout = kj::OneOf<kj::String, jsg::Ref<GPUPipelineLayout>>;

struct GPUComputePipelineDescriptor {
  jsg::Optional<kj::String> label;
  GPUProgrammableStage compute;
  GPUComputePipelineLayout layout;

  JSG_STRUCT(label, compute, layout);
};

} // namespace workerd::api::gpu
