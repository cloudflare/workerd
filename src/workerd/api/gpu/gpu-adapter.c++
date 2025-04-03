// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter.h"

#include "gpu-adapter-info.h"
#include "gpu-device.h"
#include "gpu-supported-features.h"
#include "gpu-supported-limits.h"

#include <workerd/jsg/exception.h>

#define WGPU_FOR_EACH_LIMIT(X)                                                                     \
  X(maxTextureDimension1D)                                                                         \
  X(maxTextureDimension2D)                                                                         \
  X(maxTextureDimension3D)                                                                         \
  X(maxTextureArrayLayers)                                                                         \
  X(maxBindGroups)                                                                                 \
  X(maxBindingsPerBindGroup)                                                                       \
  X(maxDynamicUniformBuffersPerPipelineLayout)                                                     \
  X(maxDynamicStorageBuffersPerPipelineLayout)                                                     \
  X(maxSampledTexturesPerShaderStage)                                                              \
  X(maxSamplersPerShaderStage)                                                                     \
  X(maxStorageBuffersPerShaderStage)                                                               \
  X(maxStorageTexturesPerShaderStage)                                                              \
  X(maxUniformBuffersPerShaderStage)                                                               \
  X(maxUniformBufferBindingSize)                                                                   \
  X(maxStorageBufferBindingSize)                                                                   \
  X(minUniformBufferOffsetAlignment)                                                               \
  X(minStorageBufferOffsetAlignment)                                                               \
  X(maxVertexBuffers)                                                                              \
  X(maxBufferSize)                                                                                 \
  X(maxVertexAttributes)                                                                           \
  X(maxVertexBufferArrayStride)                                                                    \
  X(maxInterStageShaderComponents)                                                                 \
  X(maxColorAttachments)                                                                           \
  X(maxColorAttachmentBytesPerSample)                                                              \
  X(maxComputeWorkgroupStorageSize)                                                                \
  X(maxComputeInvocationsPerWorkgroup)                                                             \
  X(maxComputeWorkgroupSizeX)                                                                      \
  X(maxComputeWorkgroupSizeY)                                                                      \
  X(maxComputeWorkgroupSizeZ)                                                                      \
  X(maxComputeWorkgroupsPerDimension)

namespace workerd::api::gpu {

void setLimit(wgpu::RequiredLimits& limits, kj::StringPtr name, unsigned long long value) {

#define COPY_LIMIT(LIMIT)                                                                          \
  if (name == "#LIMIT") {                                                                          \
    limits.limits.LIMIT = value;                                                                   \
    return;                                                                                        \
  }
  WGPU_FOR_EACH_LIMIT(COPY_LIMIT)
#undef COPY_LIMIT

  JSG_FAIL_REQUIRE(TypeError, "unknown limit", name);
}

jsg::Promise<jsg::Ref<GPUAdapterInfo>> GPUAdapter::requestAdapterInfo(
    jsg::Lock& js, jsg::Optional<kj::Array<kj::String>> unmaskHints) {

  wgpu::AdapterInfo info = {};
  adapter_.GetInfo(&info);
  auto gpuInfo = js.alloc<GPUAdapterInfo>(kj::mv(info));
  return js.resolvedPromise(kj::mv(gpuInfo));
}

kj::String parseDeviceLostReason(wgpu::DeviceLostReason reason) {
  switch (reason) {
    case wgpu::DeviceLostReason::Destroyed:
      return kj::str("destroyed");
    case wgpu::DeviceLostReason::Unknown:
      return kj::str("unknown");
    case wgpu::DeviceLostReason::InstanceDropped:
      return kj::str("dropped");
    case wgpu::DeviceLostReason::FailedCreation:
      return kj::str("failed_creation");
  }
}

jsg::Promise<jsg::Ref<GPUDevice>> GPUAdapter::requestDevice(
    jsg::Lock& js, jsg::Optional<GPUDeviceDescriptor> descriptor) {
  wgpu::DeviceDescriptor desc{};
  kj::Vector<wgpu::FeatureName> requiredFeatures;
  wgpu::RequiredLimits limits;
  KJ_IF_SOME(d, descriptor) {
    KJ_IF_SOME(label, d.label) {
      desc.label = label.cStr();
    }

    KJ_IF_SOME(features, d.requiredFeatures) {
      for (auto& required: features) {
        requiredFeatures.add(parseFeatureName(required));
      }

      desc.requiredFeatureCount = requiredFeatures.size();
      desc.requiredFeatures = requiredFeatures.begin();
    }

    KJ_IF_SOME(requiredLimits, d.requiredLimits) {
      for (auto& f: requiredLimits.fields) {
        setLimit(limits, f.name, f.value);
      }
      desc.requiredLimits = &limits;
    }
  }

  using DeviceLostContext = AsyncContext<jsg::Ref<GPUDeviceLostInfo>>;
  auto deviceLostCtx = kj::refcounted<DeviceLostContext>(js, kj::addRef(*async_));
  desc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
      [ctx = kj::addRef(*deviceLostCtx)](
          const wgpu::Device&, wgpu::DeviceLostReason reason, const char* message) mutable {
    jsg::Lock& js = IoContext::current().getCurrentLock();
    auto r = parseDeviceLostReason(reason);
    if (ctx->fulfiller_->isWaiting()) {
      auto lostInfo = js.alloc<GPUDeviceLostInfo>(kj::mv(r), kj::str(message));
      ctx->fulfiller_->fulfill(kj::mv(lostInfo));
    }
  });

  auto uErrorCtx = kj::heap<UncapturedErrorContext>();
  desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, const char* message, void* userdata) {
    jsg::Lock& js = IoContext::current().getCurrentLock();
    auto maybeTarget = static_cast<kj::Maybe<EventTarget*>*>(userdata);

    KJ_IF_SOME(target, *maybeTarget) {
      if (target->getHandlerCount("uncapturederror") > 0) {
        jsg::Ref<GPUError> error = nullptr;
        switch (type) {
          case wgpu::ErrorType::Validation:
            error = js.alloc<GPUValidationError>(kj::str(message));
            break;
          case wgpu::ErrorType::NoError:
            KJ_UNREACHABLE;
          case wgpu::ErrorType::OutOfMemory:
            error = js.alloc<GPUOutOfMemoryError>(kj::str(message));
            break;
          case wgpu::ErrorType::Internal:
          case wgpu::ErrorType::DeviceLost:
          case wgpu::ErrorType::Unknown:
            error = js.alloc<GPUInternalError>(kj::str(message));
            break;
        }

        auto init = GPUUncapturedErrorEventInit{kj::mv(error)};
        auto ev = js.alloc<GPUUncapturedErrorEvent>("uncapturederror"_kj, kj::mv(init));
        target->dispatchEventImpl(js, kj::mv(ev));
        return;
      }
    }

    // no "uncapturederror" handler
    KJ_LOG(INFO, "WebGPU uncaptured error", kj::str((uint32_t)type), message);
  }, (void*)&uErrorCtx->target);

  struct UserData {
    wgpu::Device device = nullptr;
    bool requestEnded = false;
  };
  UserData userData;

  adapter_.RequestDevice(&desc,
      [](WGPURequestDeviceStatus status, WGPUDevice cDevice, const char* message, void* pUserData) {
    JSG_REQUIRE(status == WGPURequestDeviceStatus_Success, Error, message);

    UserData& userData = *reinterpret_cast<UserData*>(pUserData);
    userData.device = wgpu::Device::Acquire(cDevice);
    userData.requestEnded = true;
  }, (void*)&userData);

  KJ_ASSERT(userData.requestEnded);

  jsg::Ref<GPUDevice> gpuDevice = js.alloc<GPUDevice>(
      js, kj::mv(userData.device), kj::addRef(*async_), kj::mv(deviceLostCtx), kj::mv(uErrorCtx));
  return js.resolvedPromise(kj::mv(gpuDevice));
}

jsg::Ref<GPUSupportedFeatures> GPUAdapter::getFeatures(jsg::Lock& js) {
  wgpu::Adapter adapter(adapter_.Get());
  size_t count = adapter.EnumerateFeatures(nullptr);
  kj::Array<wgpu::FeatureName> features = kj::heapArray<wgpu::FeatureName>(count);
  if (count > 0) {
    adapter.EnumerateFeatures(&features[0]);
  }
  return js.alloc<GPUSupportedFeatures>(kj::mv(features));
}

jsg::Ref<GPUSupportedLimits> GPUAdapter::getLimits(jsg::Lock& js) {
  WGPUSupportedLimits limits{};
  JSG_REQUIRE(adapter_.GetLimits(&limits), TypeError, "failed to get adapter limits");

  // need to copy to the C++ version of the object
  wgpu::SupportedLimits wgpuLimits{};

#define COPY_LIMIT(LIMIT) wgpuLimits.limits.LIMIT = limits.limits.LIMIT;
  WGPU_FOR_EACH_LIMIT(COPY_LIMIT)
#undef COPY_LIMIT

  return js.alloc<GPUSupportedLimits>(kj::mv(wgpuLimits));
}

}  // namespace workerd::api::gpu
