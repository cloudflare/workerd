// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter.h"
#include "gpu-adapter-info.h"
#include "gpu-device.h"
#include "gpu-supported-features.h"
#include "gpu-supported-limits.h"
#include "workerd/jsg/exception.h"

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

jsg::Promise<jsg::Ref<GPUAdapterInfo>>
GPUAdapter::requestAdapterInfo(jsg::Lock& js, jsg::Optional<kj::Array<kj::String>> unmaskHints) {

  wgpu::AdapterInfo info = {};
  adapter_.GetInfo(&info);
  auto gpuInfo = jsg::alloc<GPUAdapterInfo>(kj::mv(info));
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

kj::Promise<void> dispatchEventTask(IoContext& ctx, jsg::Ref<GPUUncapturedErrorEvent> event, EventTarget& target) {
    co_await ctx.run([&](jsg::Lock& js) -> kj::Promise<void> {
      target.dispatchEventImpl(js, kj::mv(event));
      return kj::READY_NOW;
    }, kj::Maybe<InputGate::Lock>(kj::none));
}

jsg::Promise<jsg::Ref<GPUDevice>>
GPUAdapter::requestDevice(jsg::Lock& js, jsg::Optional<GPUDeviceDescriptor> descriptor) {
  wgpu::DeviceDescriptor desc{};
  kj::Vector<wgpu::FeatureName> requiredFeatures;
  wgpu::RequiredLimits limits;
  KJ_IF_SOME(d, descriptor) {
    KJ_IF_SOME(label, d.label) {
      desc.label = label.cStr();
    }

    KJ_IF_SOME(features, d.requiredFeatures) {
      for (auto& required : features) {
        requiredFeatures.add(parseFeatureName(required));
      }

      desc.requiredFeatureCount = requiredFeatures.size();
      desc.requiredFeatures = requiredFeatures.begin();
    }

    KJ_IF_SOME(requiredLimits, d.requiredLimits) {
      for (auto& f : requiredLimits.fields) {
        setLimit(limits, f.name, f.value);
      }
      desc.requiredLimits = &limits;
    }
  }

  using DeviceLostContext = AsyncContext<jsg::Ref<GPUDeviceLostInfo>>;
  auto deviceLostCtx = kj::refcounted<DeviceLostContext>(js, kj::addRef(*async_));
  desc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [ctx = kj::addRef(*deviceLostCtx)](const wgpu::Device&, wgpu::DeviceLostReason reason,
                                         const char* message) mutable {
        auto r = parseDeviceLostReason(reason);
        if (ctx->fulfiller_->isWaiting()) {
          auto lostInfo = jsg::alloc<GPUDeviceLostInfo>(kj::mv(r), kj::str(message));
          ctx->fulfiller_->fulfill(kj::mv(lostInfo));
        }
      });

  auto uErrorCtx = kj::heap<UncapturedErrorContext>(IoContext::current());
  desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, const char* message, void* userdata) {
        auto uec = static_cast<UncapturedErrorContext*>(userdata);

        KJ_IF_SOME(target, uec->target) {
          if (target->getHandlerCount("uncapturederror") > 0) {
            jsg::Ref<GPUError> error = nullptr;
            switch (type) {
            case wgpu::ErrorType::Validation:
              error = jsg::alloc<GPUValidationError>(kj::str(message));
              break;
            case wgpu::ErrorType::NoError:
              KJ_UNREACHABLE;
            case wgpu::ErrorType::OutOfMemory:
              error = jsg::alloc<GPUOutOfMemoryError>(kj::str(message));
              break;
            case wgpu::ErrorType::Internal:
            case wgpu::ErrorType::DeviceLost:
            case wgpu::ErrorType::Unknown:
              error = jsg::alloc<GPUInternalError>(kj::str(message));
              break;
            }

            auto init = GPUUncapturedErrorEventInit{kj::mv(error)};
            auto ev = jsg::alloc<GPUUncapturedErrorEvent>("uncapturederror"_kj, kj::mv(init));
            uec->context.addTask(dispatchEventTask(uec->context, kj::mv(ev), *target));
            return;
          }
        }

        // no "uncapturederror" handler
        KJ_LOG(INFO, "WebGPU uncaptured error", kj::str((uint32_t)type), message);
      },
      (void*)uErrorCtx);

  using RequestDeviceContext = AsyncContext<jsg::Ref<GPUDevice>>;
  auto ctx = kj::heap<RequestDeviceContext>(js, kj::addRef(*async_));
  auto promise = kj::mv(ctx->promise_);
  adapter_.RequestDevice(
      &desc, wgpu::CallbackMode::AllowProcessEvents,
      [ctx = kj::mv(ctx), uErrorCtx = kj::mv(uErrorCtx), deviceLostCtx = kj::mv(deviceLostCtx),
       async_ = kj::addRef(*async_)](wgpu::RequestDeviceStatus status, wgpu::Device device,
                                     const char* message) mutable {
        JSG_REQUIRE(status == wgpu::RequestDeviceStatus::Success, Error, message);

        jsg::Ref<GPUDevice> gpuDevice = jsg::alloc<GPUDevice>(
            kj::mv(device), kj::mv(async_), kj::mv(deviceLostCtx), kj::mv(uErrorCtx));

        ctx->fulfiller_->fulfill(kj::mv(gpuDevice));
      });
  async_->MaybeFlush();
  return promise;
}

jsg::Ref<GPUSupportedFeatures> GPUAdapter::getFeatures() {
  wgpu::Adapter adapter(adapter_.Get());
  size_t count = adapter.EnumerateFeatures(nullptr);
  kj::Array<wgpu::FeatureName> features = kj::heapArray<wgpu::FeatureName>(count);
  if (count > 0) {
    adapter.EnumerateFeatures(&features[0]);
  }
  return jsg::alloc<GPUSupportedFeatures>(kj::mv(features));
}

jsg::Ref<GPUSupportedLimits> GPUAdapter::getLimits() {
  wgpu::SupportedLimits limits{};
  JSG_REQUIRE(adapter_.GetLimits(&limits), TypeError, "failed to get adapter limits");

  // need to copy to the C++ version of the object
  wgpu::SupportedLimits wgpuLimits{};

#define COPY_LIMIT(LIMIT) wgpuLimits.limits.LIMIT = limits.limits.LIMIT;
  WGPU_FOR_EACH_LIMIT(COPY_LIMIT)
#undef COPY_LIMIT

  return jsg::alloc<GPUSupportedLimits>(kj::mv(wgpuLimits));
}

} // namespace workerd::api::gpu
