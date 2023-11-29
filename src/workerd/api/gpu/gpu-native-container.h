// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-container.h"
#include <dawn/native/DawnNative.h>

namespace workerd::api::gpu {

class DawnNativeContainer : public DawnContainer {
public:
  wgpu::Instance getInstance() override;
  void Flush() override;
  DawnNativeContainer();
  ~DawnNativeContainer() noexcept override {}

private:
  dawn::native::Instance instance_;
};

} // namespace workerd::api::gpu
