// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-device.h"
#include "gpu-utils.h"
#include <dawn/native/DawnNative.h>
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUAdapter : public jsg::Object {
public:
  explicit GPUAdapter(dawn::native::Adapter a) : adapter_(a){};
  JSG_RESOURCE_TYPE(GPUAdapter) { JSG_METHOD(requestDevice); }

private:
  jsg::Promise<jsg::Ref<GPUDevice>>
  requestDevice(jsg::Lock &, jsg::Optional<GPUDeviceDescriptor>);
  dawn::native::Adapter adapter_;
};

} // namespace workerd::api::gpu
