// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter-info.h"

namespace workerd::api::gpu {

GPUAdapterInfo::GPUAdapterInfo(wgpu::AdapterInfo info)
    : vendor_(kj::str(info.vendor)),
      architecture_(kj::str(info.architecture)),
      device_(kj::str(info.device)),
      description_(kj::str(info.description)) {}

}  // namespace workerd::api::gpu
