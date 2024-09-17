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
#include "gpu-render-pipeline.h"
#include "gpu-sampler.h"
#include "gpu-shader-module.h"
#include "gpu-supported-features.h"
#include "gpu-supported-limits.h"
#include "gpu-texture.h"

#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/api/basics.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/promise.h>

namespace workerd::api::gpu {

struct UncapturedErrorContext {
  kj::Maybe<EventTarget*> target;
};

class GPUDevice: public EventTarget {
public:
  explicit GPUDevice(jsg::Lock& js,
      wgpu::Device d,
      kj::Own<AsyncRunner> async,
      kj::Own<AsyncContext<jsg::Ref<GPUDeviceLostInfo>>> deviceLostCtx,
      kj::Own<UncapturedErrorContext> uErrorCtx);
  ~GPUDevice();
  JSG_RESOURCE_TYPE(GPUDevice) {
    JSG_INHERIT(EventTarget);
    JSG_METHOD(createBuffer);
    JSG_METHOD(createBindGroupLayout);
    JSG_METHOD(createBindGroup);
    JSG_METHOD(createSampler);
    JSG_METHOD(createShaderModule);
    JSG_METHOD(createPipelineLayout);
    JSG_METHOD(createComputePipeline);
    JSG_METHOD(createRenderPipeline);
    JSG_METHOD(createCommandEncoder);
    JSG_METHOD(createTexture);
    JSG_METHOD(destroy);
    JSG_METHOD(createQuerySet);
    JSG_METHOD(pushErrorScope);
    JSG_METHOD(popErrorScope);
    JSG_READONLY_PROTOTYPE_PROPERTY(queue, getQueue);
    JSG_READONLY_PROTOTYPE_PROPERTY(lost, getLost);
    JSG_READONLY_PROTOTYPE_PROPERTY(features, getFeatures);
    JSG_READONLY_PROTOTYPE_PROPERTY(limits, getLimits);
  }

  // The GPUDevice explicitly does not expose a constructor(). It is
  // illegal for user code to create an GPUDevice directly.
  static jsg::Ref<GPUDevice> constructor() = delete;

private:
  wgpu::Device device_;
  kj::Own<AsyncContext<jsg::Ref<GPUDeviceLostInfo>>> dlc_;
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<GPUDeviceLostInfo>>> lost_promise_;
  kj::Own<UncapturedErrorContext> uec_;
  kj::Own<AsyncRunner> async_;
  bool destroyed_ = false;
  jsg::Ref<GPUBuffer> createBuffer(jsg::Lock&, GPUBufferDescriptor);
  jsg::Ref<GPUTexture> createTexture(jsg::Lock&, GPUTextureDescriptor);
  jsg::Ref<GPUBindGroupLayout> createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor);
  jsg::Ref<GPUBindGroup> createBindGroup(GPUBindGroupDescriptor descriptor);
  jsg::Ref<GPUSampler> createSampler(GPUSamplerDescriptor descriptor);
  jsg::Ref<GPUShaderModule> createShaderModule(GPUShaderModuleDescriptor descriptor);
  jsg::Ref<GPUPipelineLayout> createPipelineLayout(GPUPipelineLayoutDescriptor descriptor);
  jsg::Ref<GPUComputePipeline> createComputePipeline(GPUComputePipelineDescriptor descriptor);
  jsg::Ref<GPURenderPipeline> createRenderPipeline(GPURenderPipelineDescriptor descriptor);
  jsg::Promise<jsg::Ref<GPUComputePipeline>> createComputePipelineAsync(
      jsg::Lock& js, GPUComputePipelineDescriptor descriptor);
  jsg::Ref<GPUCommandEncoder> createCommandEncoder(
      jsg::Optional<GPUCommandEncoderDescriptor> descriptor);
  jsg::Ref<GPUQueue> getQueue();
  void destroy();
  jsg::Ref<GPUQuerySet> createQuerySet(GPUQuerySetDescriptor descriptor);
  void pushErrorScope(GPUErrorFilter filter);
  jsg::Promise<kj::Maybe<jsg::Ref<GPUError>>> popErrorScope(jsg::Lock& js);
  jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<GPUDeviceLostInfo>>>& getLost();
  jsg::Ref<GPUSupportedFeatures> getFeatures();
  jsg::Ref<GPUSupportedLimits> getLimits();
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

struct GPUUncapturedErrorEventInit {
  jsg::Ref<GPUError> error;
  JSG_STRUCT(error);
};

class GPUUncapturedErrorEvent: public Event {
public:
  GPUUncapturedErrorEvent(
      kj::StringPtr type, GPUUncapturedErrorEventInit gpuUncapturedErrorEventInitDict)
      : Event(kj::mv(type)),
        error_(kj::mv(gpuUncapturedErrorEventInitDict.error)) {};

  static jsg::Ref<GPUUncapturedErrorEvent> constructor() = delete;

  JSG_RESOURCE_TYPE(GPUUncapturedErrorEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_INSTANCE_PROPERTY(error, getError);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("error", error_);
  }

private:
  jsg::Ref<GPUError> error_;

  jsg::Ref<GPUError> getError() {
    return error_.addRef();
  }
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(error_);
  }
};

}  // namespace workerd::api::gpu
