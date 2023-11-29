// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-native-container.h"

namespace workerd::api::gpu {

DawnNativeContainer::DawnNativeContainer() {}

wgpu::Instance DawnNativeContainer::getInstance() {
  return instance_.Get();
}

void DawnNativeContainer::Flush() {}

} // namespace workerd::api::gpu
