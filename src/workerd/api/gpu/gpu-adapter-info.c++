// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-adapter-info.h"
#include "workerd/jsg/exception.h"

namespace workerd::api::gpu {

GPUAdapterInfo::GPUAdapterInfo(WGPUAdapterProperties properties)
    : vendor_(kj::str(properties.vendorName)),
      architecture_(kj::str(properties.architecture)),
      device_(kj::str(properties.name)),
      description_(kj::str(properties.driverDescription)) {}

} // namespace workerd::api::gpu
