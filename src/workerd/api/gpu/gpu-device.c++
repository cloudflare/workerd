// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-device.h"
#include "gpu-bindgroup-layout.h"
#include "gpu-bindgroup.h"
#include "gpu-buffer.h"
#include "gpu-command-encoder.h"
#include "gpu-compute-pipeline.h"
#include "gpu-errors.h"
#include "gpu-query-set.h"
#include "gpu-queue.h"
#include "gpu-sampler.h"
#include "gpu-utils.h"
#include "workerd/jsg/exception.h"
#include "workerd/jsg/jsg.h"

namespace workerd::api::gpu {

jsg::Ref<GPUBuffer> GPUDevice::createBuffer(jsg::Lock& js,
                                            GPUBufferDescriptor descriptor) {
  wgpu::BufferDescriptor desc{};
  desc.label = descriptor.label.cStr();
  desc.mappedAtCreation = descriptor.mappedAtCreation;
  desc.size = descriptor.size;
  desc.usage = static_cast<wgpu::BufferUsage>(descriptor.usage);
  auto buffer = device_.CreateBuffer(&desc);
  return jsg::alloc<GPUBuffer>(js, kj::mv(buffer), kj::mv(desc), device_,
                               kj::addRef(*async_));
}

wgpu::CompareFunction parseCompareFunction(kj::StringPtr compare) {
  if (compare == "never") {
    return wgpu::CompareFunction::Never;
  }

  if (compare == "less") {
    return wgpu::CompareFunction::Less;
  }

  if (compare == "equal") {
    return wgpu::CompareFunction::Equal;
  }

  if (compare == "less-equal") {
    return wgpu::CompareFunction::LessEqual;
  }

  if (compare == "greater") {
    return wgpu::CompareFunction::Greater;
  }

  if (compare == "not-equal") {
    return wgpu::CompareFunction::NotEqual;
  }

  if (compare == "greater-equal") {
    return wgpu::CompareFunction::GreaterEqual;
  }

  if (compare == "always") {
    return wgpu::CompareFunction::Always;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown compare function", compare);
}

wgpu::AddressMode parseAddressMode(kj::StringPtr mode) {

  if (mode == "clamp-to-edge") {
    return wgpu::AddressMode::ClampToEdge;
  }

  if (mode == "repeat") {
    return wgpu::AddressMode::Repeat;
  }

  if (mode == "mirror-repeat") {
    return wgpu::AddressMode::MirrorRepeat;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown address mode", mode);
}

wgpu::FilterMode parseFilterMode(kj::StringPtr mode) {

  if (mode == "nearest") {
    return wgpu::FilterMode::Nearest;
  }

  if (mode == "linear") {
    return wgpu::FilterMode::Linear;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown filter mode", mode);
}

wgpu::MipmapFilterMode parseMipmapFilterMode(kj::StringPtr mode) {

  if (mode == "nearest") {
    return wgpu::MipmapFilterMode::Nearest;
  }

  if (mode == "linear") {
    return wgpu::MipmapFilterMode::Linear;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown mipmap filter mode", mode);
}

jsg::Ref<GPUSampler> GPUDevice::createSampler(GPUSamplerDescriptor descriptor) {
  wgpu::SamplerDescriptor desc{};

  desc.addressModeU = parseAddressMode(
      descriptor.addressModeU.orDefault([] { return "clamp-to-edge"_kj; }));
  desc.addressModeV = parseAddressMode(
      descriptor.addressModeV.orDefault([] { return "clamp-to-edge"_kj; }));
  desc.addressModeW = parseAddressMode(
      descriptor.addressModeW.orDefault([] { return "clamp-to-edge"_kj; }));
  desc.magFilter = parseFilterMode(
      descriptor.magFilter.orDefault([] { return "nearest"_kj; }));
  desc.minFilter = parseFilterMode(
      descriptor.minFilter.orDefault([] { return "nearest"_kj; }));
  desc.mipmapFilter = parseMipmapFilterMode(
      descriptor.mipmapFilter.orDefault([] { return "nearest"_kj; }));
  desc.lodMinClamp = descriptor.lodMinClamp.orDefault(0);
  desc.lodMaxClamp = descriptor.lodMaxClamp.orDefault(32);
  desc.compare = parseCompareFunction(descriptor.compare);
  desc.maxAnisotropy = descriptor.maxAnisotropy.orDefault(1);

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  auto sampler = device_.CreateSampler(&desc);
  return jsg::alloc<GPUSampler>(kj::mv(sampler));
}

jsg::Ref<GPUBindGroupLayout>
GPUDevice::createBindGroupLayout(GPUBindGroupLayoutDescriptor descriptor) {
  wgpu::BindGroupLayoutDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  kj::Vector<wgpu::BindGroupLayoutEntry> layoutEntries;
  for (auto& e : descriptor.entries) {
    layoutEntries.add(parseBindGroupLayoutEntry(e));
  }
  desc.entries = layoutEntries.begin();
  desc.entryCount = layoutEntries.size();

  auto bindGroupLayout = device_.CreateBindGroupLayout(&desc);
  return jsg::alloc<GPUBindGroupLayout>(kj::mv(bindGroupLayout));
}

jsg::Ref<GPUBindGroup>
GPUDevice::createBindGroup(GPUBindGroupDescriptor descriptor) {
  wgpu::BindGroupDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  desc.layout = *descriptor.layout;

  kj::Vector<wgpu::BindGroupEntry> bindGroupEntries;
  for (auto& e : descriptor.entries) {
    bindGroupEntries.add(parseBindGroupEntry(e));
  }

  desc.entries = bindGroupEntries.begin();
  desc.entryCount = bindGroupEntries.size();

  auto bindGroup = device_.CreateBindGroup(&desc);
  return jsg::alloc<GPUBindGroup>(kj::mv(bindGroup));
}

jsg::Ref<GPUShaderModule>
GPUDevice::createShaderModule(GPUShaderModuleDescriptor descriptor) {
  wgpu::ShaderModuleDescriptor desc{};
  wgpu::ShaderModuleWGSLDescriptor wgsl_desc{};
  desc.nextInChain = &wgsl_desc;

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  wgsl_desc.code = descriptor.code.cStr();

  auto shader = device_.CreateShaderModule(&desc);
  return jsg::alloc<GPUShaderModule>(kj::mv(shader), kj::addRef(*async_));
}

jsg::Ref<GPUPipelineLayout>
GPUDevice::createPipelineLayout(GPUPipelineLayoutDescriptor descriptor) {
  wgpu::PipelineLayoutDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  kj::Vector<wgpu::BindGroupLayout> bindGroupLayouts;
  for (auto& l : descriptor.bindGroupLayouts) {
    bindGroupLayouts.add(*l);
  }

  desc.bindGroupLayouts = bindGroupLayouts.begin();
  desc.bindGroupLayoutCount = bindGroupLayouts.size();

  auto layout = device_.CreatePipelineLayout(&desc);
  return jsg::alloc<GPUPipelineLayout>(kj::mv(layout));
}

jsg::Ref<GPUCommandEncoder> GPUDevice::createCommandEncoder(
    jsg::Optional<GPUCommandEncoderDescriptor> descriptor) {
  wgpu::CommandEncoderDescriptor desc{};

  KJ_IF_MAYBE (d, descriptor) {
    KJ_IF_MAYBE (label, d->label) {
      desc.label = label->cStr();
    }
  }

  auto encoder = device_.CreateCommandEncoder(&desc);
  return jsg::alloc<GPUCommandEncoder>(kj::mv(encoder));
}

wgpu::ComputePipelineDescriptor
parseComputePipelineDescriptor(GPUComputePipelineDescriptor& descriptor) {
  wgpu::ComputePipelineDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  desc.compute.module = *descriptor.compute.module;
  desc.compute.entryPoint = descriptor.compute.entryPoint.cStr();

  kj::Vector<wgpu::ConstantEntry> constants;
  KJ_IF_MAYBE (cDict, descriptor.compute.constants) {
    for (auto& f : cDict->fields) {
      wgpu::ConstantEntry e;
      e.key = f.name.cStr();
      e.value = f.value;
      constants.add(kj::mv(e));
    }
  }

  desc.compute.constants = constants.begin();
  desc.compute.constantCount = constants.size();

  KJ_SWITCH_ONEOF(descriptor.layout) {
    KJ_CASE_ONEOF(autoLayoutMode, kj::String) {
      JSG_REQUIRE(autoLayoutMode == "auto", TypeError,
                  "unknown auto layout mode", autoLayoutMode);
      desc.layout = nullptr;
    }
    KJ_CASE_ONEOF(layout, jsg::Ref<GPUPipelineLayout>) {
      desc.layout = *layout;
    }
  }

  return kj::mv(desc);
}

jsg::Ref<GPUComputePipeline>
GPUDevice::createComputePipeline(GPUComputePipelineDescriptor descriptor) {
  wgpu::ComputePipelineDescriptor desc =
      parseComputePipelineDescriptor(descriptor);
  auto pipeline = device_.CreateComputePipeline(&desc);
  return jsg::alloc<GPUComputePipeline>(kj::mv(pipeline));
}

jsg::Promise<kj::Maybe<jsg::Ref<GPUError>>> GPUDevice::popErrorScope() {
  struct Context {
    kj::Own<kj::PromiseFulfiller<kj::Maybe<jsg::Ref<GPUError>>>> fulfiller;
    AsyncTask task;
  };

  auto paf = kj::newPromiseAndFulfiller<kj::Maybe<jsg::Ref<GPUError>>>();
  // This context object will hold information for the callback, including the
  // fullfiller to signal the caller with the result, and an async task that
  // will ensure the device's Tick() function is called periodically. It will be
  // deallocated at the end of the callback function.
  auto ctx = new Context{kj::mv(paf.fulfiller), AsyncTask(kj::addRef(*async_))};

  device_.PopErrorScope(
      [](WGPUErrorType type, char const* message, void* userdata) {
        auto c = std::unique_ptr<Context>(static_cast<Context*>(userdata));
        switch (type) {
        case WGPUErrorType::WGPUErrorType_NoError:
          c->fulfiller->fulfill(nullptr);
          break;
        case WGPUErrorType::WGPUErrorType_OutOfMemory: {
          jsg::Ref<GPUError> err = jsg::alloc<GPUOOMError>(kj::str(message));
          c->fulfiller->fulfill(kj::mv(err));
          break;
        }
        case WGPUErrorType::WGPUErrorType_Validation: {
          jsg::Ref<GPUError> err =
              jsg::alloc<GPUValidationError>(kj::str(message));
          c->fulfiller->fulfill(kj::mv(err));
          break;
        }
        case WGPUErrorType::WGPUErrorType_Unknown:
        case WGPUErrorType::WGPUErrorType_DeviceLost:
          c->fulfiller->reject(JSG_KJ_EXCEPTION(FAILED, TypeError, message));
          break;
        default:
          c->fulfiller->reject(
              JSG_KJ_EXCEPTION(FAILED, TypeError, "unhandled error type"));
          break;
        }
      },
      ctx);

  auto& context = IoContext::current();
  return context.awaitIo(kj::mv(paf.promise));
}

jsg::Promise<jsg::Ref<GPUComputePipeline>>
GPUDevice::createComputePipelineAsync(GPUComputePipelineDescriptor descriptor) {
  wgpu::ComputePipelineDescriptor desc =
      parseComputePipelineDescriptor(descriptor);

  struct Context {
    kj::Own<kj::PromiseFulfiller<jsg::Ref<GPUComputePipeline>>> fulfiller;
    AsyncTask task;
  };
  auto paf = kj::newPromiseAndFulfiller<jsg::Ref<GPUComputePipeline>>();
  // This context object will hold information for the callback, including the
  // fullfiller to signal the caller with the result, and an async task that
  // will ensure the device's Tick() function is called periodically. It will be
  // deallocated at the end of the callback function.
  auto ctx = new Context{kj::mv(paf.fulfiller), AsyncTask(kj::addRef(*async_))};

  device_.CreateComputePipelineAsync(
      &desc,
      [](WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline,
         char const* message, void* userdata) {
        // Note: this is invoked outside the JS isolate lock
        auto c = std::unique_ptr<Context>(static_cast<Context*>(userdata));

        switch (status) {
        case WGPUCreatePipelineAsyncStatus::
            WGPUCreatePipelineAsyncStatus_Success:
          c->fulfiller->fulfill(
              jsg::alloc<GPUComputePipeline>(kj::mv(pipeline)));
          break;
        default:
          c->fulfiller->reject(
              JSG_KJ_EXCEPTION(FAILED, TypeError, "unknown error"));
          break;
        }
      },
      ctx);

  auto& context = IoContext::current();
  return context.awaitIo(kj::mv(paf.promise));
}

jsg::Ref<GPUQueue> GPUDevice::getQueue() {
  auto queue = device_.GetQueue();
  return jsg::alloc<GPUQueue>(kj::mv(queue));
}

GPUDevice::~GPUDevice() {
  if (!destroyed_) {
    device_.Destroy();
    destroyed_ = true;
  }
}

void GPUDevice::destroy() {
  if (lost_promise_fulfiller_->isWaiting()) {
    auto lostInfo = jsg::alloc<GPUDeviceLostInfo>(
        kj::str("destroyed"), kj::str("device was destroyed"));
    lost_promise_fulfiller_->fulfill(kj::mv(lostInfo));
  }

  device_.Destroy();
  destroyed_ = true;
}

jsg::MemoizedIdentity<jsg::Promise<jsg::Ref<GPUDeviceLostInfo>>>&
GPUDevice::getLost() {
  return lost_promise_;
}

kj::String parseDeviceLostReason(WGPUDeviceLostReason reason) {
  switch (reason) {
  case WGPUDeviceLostReason_Force32:
    KJ_UNREACHABLE
  case WGPUDeviceLostReason_Destroyed:
    return kj::str("destroyed");
  case WGPUDeviceLostReason_Undefined:
    return kj::str("undefined");
  }
}

GPUDevice::GPUDevice(jsg::Lock& js, wgpu::Device d)
    : device_(d), lost_promise_(nullptr),
      async_(kj::refcounted<AsyncRunner>(d)) {
  auto& context = IoContext::current();
  auto paf = kj::newPromiseAndFulfiller<jsg::Ref<GPUDeviceLostInfo>>();
  lost_promise_fulfiller_ = kj::mv(paf.fulfiller);
  lost_promise_ = context.awaitIo(js, kj::mv(paf.promise));

  device_.SetLoggingCallback(
      [](WGPULoggingType type, char const* message, void* userdata) {
        KJ_LOG(INFO, "WebGPU logging", kj::str(type), message);
      },
      nullptr);
  device_.SetUncapturedErrorCallback(
      [](WGPUErrorType type, char const* message, void* userdata) {
        KJ_LOG(INFO, "WebGPU uncaptured error", kj::str(type), message);
      },
      nullptr);

  device_.SetDeviceLostCallback(
      [](WGPUDeviceLostReason reason, char const* message, void* userdata) {
        auto r = parseDeviceLostReason(reason);
        auto* self = static_cast<GPUDevice*>(userdata);
        if (self->lost_promise_fulfiller_->isWaiting()) {
          auto lostInfo =
              jsg::alloc<GPUDeviceLostInfo>(kj::mv(r), kj::str(message));
          self->lost_promise_fulfiller_->fulfill(kj::mv(lostInfo));
        }
      },
      this);
};

jsg::Ref<GPUQuerySet>
GPUDevice::createQuerySet(GPUQuerySetDescriptor descriptor) {
  wgpu::QuerySetDescriptor desc{};

  KJ_IF_MAYBE (label, descriptor.label) {
    desc.label = label->cStr();
  }

  desc.count = descriptor.count;
  desc.type = parseQueryType(descriptor.type);

  auto querySet = device_.CreateQuerySet(&desc);
  return jsg::alloc<GPUQuerySet>(kj::mv(querySet));
}

wgpu::ErrorFilter parseErrorFilter(GPUErrorFilter& filter) {

  if (filter == "validation") {
    return wgpu::ErrorFilter::Validation;
  }

  if (filter == "out-of-memory") {
    return wgpu::ErrorFilter::OutOfMemory;
  }

  if (filter == "internal") {
    return wgpu::ErrorFilter::Internal;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown error filter", filter);
}

void GPUDevice::pushErrorScope(GPUErrorFilter filter) {
  wgpu::ErrorFilter f = parseErrorFilter(filter);
  device_.PushErrorScope(f);
}

} // namespace workerd::api::gpu
