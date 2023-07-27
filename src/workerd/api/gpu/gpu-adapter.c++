// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter.h"

namespace workerd::api::gpu {

jsg::Promise<jsg::Ref<GPUDevice>>
GPUAdapter::requestDevice(jsg::Lock &js,
                          jsg::Optional<GPUDeviceDescriptor> descriptor) {
  wgpu::DeviceDescriptor desc{};
  kj::Vector<wgpu::FeatureName> requiredFeatures;
  KJ_IF_MAYBE (d, descriptor) {
    KJ_IF_MAYBE(label, d->label) {
      desc.label = label->cStr();
    }

    KJ_IF_MAYBE (features, d->requiredFeatures) {
      for (auto &required : *features) {
        requiredFeatures.add(parseFeatureName(required));
      }

      desc.requiredFeaturesCount = requiredFeatures.size();
      desc.requiredFeatures = requiredFeatures.begin();
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
         const char *message, void *pUserData) {
        JSG_REQUIRE(status == WGPURequestDeviceStatus_Success, Error, message);

        UserData &userData = *reinterpret_cast<UserData *>(pUserData);
        userData.device = wgpu::Device::Acquire(cDevice);
        userData.requestEnded = true;
      },
      (void *)&userData);

  KJ_ASSERT(userData.requestEnded);

  jsg::Ref<GPUDevice> gpuDevice =
      jsg::alloc<GPUDevice>(kj::mv(userData.device));
  return js.resolvedPromise(kj::mv(gpuDevice));
}

} // namespace workerd::api::gpu
