// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-bindgroup-layout.h"
#include "gpu-bindgroup.h"
#include "gpu-buffer.h"
#include "gpu-pipeline-layout.h"
#include "gpu-sampler.h"
#include "gpu-shader-module.h"
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUDevice : public jsg::Object {
public:
  explicit GPUDevice(wgpu::Device d) : device_(d){};
  JSG_RESOURCE_TYPE(GPUDevice) {
    JSG_METHOD(createBuffer);
    JSG_METHOD(createBindGroupLayout);
    JSG_METHOD(createBindGroup);
    JSG_METHOD(createSampler);
    JSG_METHOD(createShaderModule);
    JSG_METHOD(createPipelineLayout);
  }

private:
  wgpu::Device device_;
  jsg::Ref<GPUBuffer> createBuffer(jsg::Lock &, GPUBufferDescriptor);
  jsg::Ref<GPUBindGroupLayout>
  createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor);
  jsg::Ref<GPUBindGroup> createBindGroup(GPUBindGroupDescriptor descriptor);
  jsg::Ref<GPUSampler> createSampler(GPUSamplerDescriptor descriptor);
  jsg::Ref<GPUShaderModule>
  createShaderModule(GPUShaderModuleDescriptor descriptor);
  jsg::Ref<GPUPipelineLayout>
  createPipelineLayout(GPUPipelineLayoutDescriptor descriptor);
};

struct GPUQueueDescriptor {
  // TODO(someday)
  jsg::Optional<kj::String> label;
  JSG_STRUCT(label);
};

struct GPUDeviceDescriptor {
  jsg::Optional<kj::String> label;
  jsg::Optional<kj::Array<GPUFeatureName>> requiredFeatures;
  // jsg::Optional<jsg::Dict<GPUSize64>> requiredLimits;
  // jsg::Optional<GPUQueueDescriptor> defaultQueue;

  // JSG_STRUCT(label, requiredFeatures, requiredLimits, defaultQueue);
  JSG_STRUCT(label, requiredFeatures);
};

} // namespace workerd::api::gpu
