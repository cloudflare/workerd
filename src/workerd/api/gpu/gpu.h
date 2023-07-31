// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-adapter.h"
#include "gpu-bindgroup-layout.h"
#include "gpu-bindgroup.h"
#include "gpu-command-encoder.h"
#include "gpu-compute-pass-encoder.h"
#include "gpu-compute-pipeline.h"
#include "gpu-device.h"
#include "gpu-pipeline-layout.h"
#include "gpu-query-set.h"
#include "gpu-sampler.h"
#include "gpu-shader-module.h"
#include "gpu-utils.h"
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

// Very experimental initial webgpu support based on the Dawn library.
namespace workerd::api::gpu {
void initialize();

class GPU : public jsg::Object {
public:
  explicit GPU();
  JSG_RESOURCE_TYPE(GPU) { JSG_METHOD(requestAdapter); }

private:
  jsg::Promise<kj::Maybe<jsg::Ref<GPUAdapter>>> requestAdapter(jsg::Lock &);
  dawn::native::Instance instance_;
};

#define EW_WEBGPU_ISOLATE_TYPES                                                \
  api::gpu::GPU, api::gpu::GPUAdapter, api::gpu::GPUDevice,                    \
      api::gpu::GPUDeviceDescriptor, api::gpu::GPUBufferDescriptor,            \
      api::gpu::GPUQueueDescriptor, api::gpu::GPUBufferUsage,                  \
      api::gpu::GPUBuffer, api::gpu::GPUShaderStage,                           \
      api::gpu::GPUBindGroupLayoutDescriptor,                                  \
      api::gpu::GPUBindGroupLayoutEntry,                                       \
      api::gpu::GPUStorageTextureBindingLayout,                                \
      api::gpu::GPUTextureBindingLayout, api::gpu::GPUSamplerBindingLayout,    \
      api::gpu::GPUBufferBindingLayout, api::gpu::GPUBindGroupLayout,          \
      api::gpu::GPUBindGroup, api::gpu::GPUBindGroupDescriptor,                \
      api::gpu::GPUBindGroupEntry, api::gpu::GPUBufferBinding,                 \
      api::gpu::GPUSampler, api::gpu::GPUSamplerDescriptor,                    \
      api::gpu::GPUShaderModule, api::gpu::GPUShaderModuleDescriptor,          \
      api::gpu::GPUPipelineLayout, api::gpu::GPUPipelineLayoutDescriptor,      \
      api::gpu::GPUComputePipeline, api::gpu::GPUComputePipelineDescriptor,    \
      api::gpu::GPUProgrammableStage, api::gpu::GPUCommandEncoder,             \
      api::gpu::GPUCommandEncoderDescriptor, api::gpu::GPUComputePassEncoder,  \
      api::gpu::GPUComputePassDescriptor, api::gpu::GPUQuerySet,               \
      api::gpu::GPUQuerySetDescriptor, api::gpu::GPUComputePassTimestampWrite

}; // namespace workerd::api::gpu
