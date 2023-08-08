// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-async-runner.h"
#include "gpu-bindgroup-layout.h"
#include "gpu-bindgroup.h"
#include "gpu-buffer.h"
#include "gpu-command-encoder.h"
#include "gpu-compute-pipeline.h"
#include "gpu-errors.h"
#include "gpu-pipeline-layout.h"
#include "gpu-query-set.h"
#include "gpu-queue.h"
#include "gpu-sampler.h"
#include "gpu-shader-module.h"
#include "workerd/jsg/promise.h"
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUDevice : public jsg::Object {
public:
  explicit GPUDevice(jsg::Lock& js, wgpu::Device d);
  ~GPUDevice();
  JSG_RESOURCE_TYPE(GPUDevice) {
    JSG_METHOD(createBuffer);
    JSG_METHOD(createBindGroupLayout);
    JSG_METHOD(createBindGroup);
    JSG_METHOD(createSampler);
    JSG_METHOD(createShaderModule);
    JSG_METHOD(createPipelineLayout);
    JSG_METHOD(createComputePipeline);
    JSG_METHOD(createCommandEncoder);
    JSG_METHOD(destroy);
    JSG_METHOD(createQuerySet);
    JSG_METHOD(pushErrorScope);
    JSG_METHOD(popErrorScope);
    JSG_READONLY_PROTOTYPE_PROPERTY(queue, getQueue);
    JSG_READONLY_PROTOTYPE_PROPERTY(lost, getLost);
  }

private:
  wgpu::Device device_;
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<GPUDeviceLostInfo>>>
      lost_promise_;
  kj::Own<kj::PromiseFulfiller<jsg::Ref<GPUDeviceLostInfo>>>
      lost_promise_fulfiller_;
  kj::Own<AsyncRunner> async_;
  bool destroyed_ = false;
  jsg::Ref<GPUBuffer> createBuffer(jsg::Lock&, GPUBufferDescriptor);
  jsg::Ref<GPUBindGroupLayout>
  createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor);
  jsg::Ref<GPUBindGroup> createBindGroup(GPUBindGroupDescriptor descriptor);
  jsg::Ref<GPUSampler> createSampler(GPUSamplerDescriptor descriptor);
  jsg::Ref<GPUShaderModule>
  createShaderModule(GPUShaderModuleDescriptor descriptor);
  jsg::Ref<GPUPipelineLayout>
  createPipelineLayout(GPUPipelineLayoutDescriptor descriptor);
  jsg::Ref<GPUComputePipeline>
  createComputePipeline(GPUComputePipelineDescriptor descriptor);
  jsg::Promise<jsg::Ref<GPUComputePipeline>>
  createComputePipelineAsync(GPUComputePipelineDescriptor descriptor);
  jsg::Ref<GPUCommandEncoder>
  createCommandEncoder(jsg::Optional<GPUCommandEncoderDescriptor> descriptor);
  jsg::Ref<GPUQueue> getQueue();
  void destroy();
  jsg::Ref<GPUQuerySet> createQuerySet(GPUQuerySetDescriptor descriptor);
  void pushErrorScope(GPUErrorFilter filter);
  jsg::Promise<kj::Maybe<jsg::Ref<GPUError>>> popErrorScope();
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<GPUDeviceLostInfo>>>& getLost();
};

struct GPUQueueDescriptor {
  // TODO(someday)
  jsg::Optional<kj::String> label;
  JSG_STRUCT(label);
};

struct GPUDeviceDescriptor {
  jsg::Optional<kj::String> label;
  jsg::Optional<kj::Array<GPUFeatureName>> requiredFeatures;
  jsg::Optional<jsg::Dict<GPUSize64>> requiredLimits;
  jsg::Optional<GPUQueueDescriptor> defaultQueue;

  JSG_STRUCT(label, requiredFeatures, requiredLimits, defaultQueue);
};

} // namespace workerd::api::gpu
