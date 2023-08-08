// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter.h"
#include "gpu-adapter-info.h"
#include "gpu-supported-features.h"
#include "gpu-supported-limits.h"
#include "workerd/jsg/exception.h"

#define WGPU_FOR_EACH_LIMIT(X)                                                 \
  X(maxTextureDimension1D)                                                     \
  X(maxTextureDimension2D)                                                     \
  X(maxTextureDimension3D)                                                     \
  X(maxTextureArrayLayers)                                                     \
  X(maxBindGroups)                                                             \
  X(maxBindingsPerBindGroup)                                                   \
  X(maxDynamicUniformBuffersPerPipelineLayout)                                 \
  X(maxDynamicStorageBuffersPerPipelineLayout)                                 \
  X(maxSampledTexturesPerShaderStage)                                          \
  X(maxSamplersPerShaderStage)                                                 \
  X(maxStorageBuffersPerShaderStage)                                           \
  X(maxStorageTexturesPerShaderStage)                                          \
  X(maxUniformBuffersPerShaderStage)                                           \
  X(maxUniformBufferBindingSize)                                               \
  X(maxStorageBufferBindingSize)                                               \
  X(minUniformBufferOffsetAlignment)                                           \
  X(minStorageBufferOffsetAlignment)                                           \
  X(maxVertexBuffers)                                                          \
  X(maxBufferSize)                                                             \
  X(maxVertexAttributes)                                                       \
  X(maxVertexBufferArrayStride)                                                \
  X(maxInterStageShaderComponents)                                             \
  X(maxColorAttachments)                                                       \
  X(maxColorAttachmentBytesPerSample)                                          \
  X(maxComputeWorkgroupStorageSize)                                            \
  X(maxComputeInvocationsPerWorkgroup)                                         \
  X(maxComputeWorkgroupSizeX)                                                  \
  X(maxComputeWorkgroupSizeY)                                                  \
  X(maxComputeWorkgroupSizeZ)                                                  \
  X(maxComputeWorkgroupsPerDimension)

namespace workerd::api::gpu {

void setLimit(wgpu::RequiredLimits& limits, kj::StringPtr name,
              unsigned long long value) {

#define COPY_LIMIT(LIMIT)                                                      \
  if (name == "#LIMIT") {                                                      \
    limits.limits.LIMIT = value;                                               \
    return;                                                                    \
  }
  WGPU_FOR_EACH_LIMIT(COPY_LIMIT)
#undef COPY_LIMIT

  JSG_FAIL_REQUIRE(TypeError, "unknown limit", name);
}

jsg::Promise<jsg::Ref<GPUAdapterInfo>> GPUAdapter::requestAdapterInfo(
    jsg::Lock& js, jsg::Optional<kj::Array<kj::String>> unmaskHints) {

  WGPUAdapterProperties adapterProperties = {};
  adapter_.GetProperties(&adapterProperties);
  auto info = jsg::alloc<GPUAdapterInfo>(adapterProperties);
  return js.resolvedPromise(kj::mv(info));
}

jsg::Promise<jsg::Ref<GPUDevice>>
GPUAdapter::requestDevice(jsg::Lock& js,
                          jsg::Optional<GPUDeviceDescriptor> descriptor) {
  wgpu::DeviceDescriptor desc{};
  kj::Vector<wgpu::FeatureName> requiredFeatures;
  wgpu::RequiredLimits limits;
  KJ_IF_MAYBE (d, descriptor) {
    KJ_IF_MAYBE (label, d->label) {
      desc.label = label->cStr();
    }

    KJ_IF_MAYBE (features, d->requiredFeatures) {
      for (auto& required : *features) {
        requiredFeatures.add(parseFeatureName(required));
      }

      desc.requiredFeaturesCount = requiredFeatures.size();
      desc.requiredFeatures = requiredFeatures.begin();
    }

    KJ_IF_MAYBE (requiredLimits, d->requiredLimits) {
      for (auto& f : requiredLimits->fields) {
        setLimit(limits, f.name, f.value);
      }
      desc.requiredLimits = &limits;
    }
  }

  struct UserData {
    wgpu::Device device = nullptr;
    bool requestEnded = false;
  };
  UserData userData;

  adapter_.RequestDevice(
      &desc,
      [](WGPURequestDeviceStatus status, WGPUDevice cDevice,
         const char* message, void* pUserData) {
        JSG_REQUIRE(status == WGPURequestDeviceStatus_Success, Error, message);

        UserData& userData = *reinterpret_cast<UserData*>(pUserData);
        userData.device = wgpu::Device::Acquire(cDevice);
        userData.requestEnded = true;
      },
      (void*)&userData);

  KJ_ASSERT(userData.requestEnded);

  jsg::Ref<GPUDevice> gpuDevice =
      jsg::alloc<GPUDevice>(js, kj::mv(userData.device));
  return js.resolvedPromise(kj::mv(gpuDevice));
}

jsg::Ref<GPUSupportedFeatures> GPUAdapter::getFeatures() {
  wgpu::Adapter adapter(adapter_.Get());
  size_t count = adapter.EnumerateFeatures(nullptr);
  kj::Array<wgpu::FeatureName> features =
      kj::heapArray<wgpu::FeatureName>(count);
  adapter.EnumerateFeatures(&features[0]);
  return jsg::alloc<GPUSupportedFeatures>(kj::mv(features));
}

jsg::Ref<GPUSupportedLimits> GPUAdapter::getLimits() {
  WGPUSupportedLimits limits{};
  JSG_REQUIRE(adapter_.GetLimits(&limits), TypeError,
              "failed to get adapter limits");

  // need to copy to the C++ version of the object
  wgpu::SupportedLimits wgpuLimits{};

#define COPY_LIMIT(LIMIT) wgpuLimits.limits.LIMIT = limits.limits.LIMIT;
  WGPU_FOR_EACH_LIMIT(COPY_LIMIT)
#undef COPY_LIMIT

  return jsg::alloc<GPUSupportedLimits>(kj::mv(wgpuLimits));
}

} // namespace workerd::api::gpu
