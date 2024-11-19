// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-adapter-info.h"
#include "gpu-device.h"
#include "gpu-supported-features.h"
#include "gpu-supported-limits.h"
#include "gpu-utils.h"

#include <workerd/jsg/jsg.h>

#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUAdapter: public jsg::Object {
 public:
  explicit GPUAdapter(dawn::native::Adapter a, kj::Own<AsyncRunner> async)
      : adapter_(a),
        async_(kj::mv(async)) {};
  JSG_RESOURCE_TYPE(GPUAdapter) {
    JSG_METHOD(requestDevice);
    JSG_METHOD(requestAdapterInfo);
    JSG_READONLY_PROTOTYPE_PROPERTY(features, getFeatures);
    JSG_READONLY_PROTOTYPE_PROPERTY(limits, getLimits);
  }

 private:
  jsg::Promise<jsg::Ref<GPUDevice>> requestDevice(jsg::Lock&, jsg::Optional<GPUDeviceDescriptor>);
  dawn::native::Adapter adapter_;
  kj::Own<AsyncRunner> async_;
  jsg::Promise<jsg::Ref<GPUAdapterInfo>> requestAdapterInfo(
      jsg::Lock& js, jsg::Optional<kj::Array<kj::String>> unmaskHints);
  jsg::Ref<GPUSupportedFeatures> getFeatures();
  jsg::Ref<GPUSupportedLimits> getLimits();
};

}  // namespace workerd::api::gpu
