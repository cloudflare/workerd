// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-compute-pipeline.h"

namespace workerd::api::gpu {
jsg::Ref<GPUBindGroupLayout> GPUComputePipeline::getBindGroupLayout(uint32_t index) {
  auto layout = pipeline_.GetBindGroupLayout(index);
  return jsg::alloc<GPUBindGroupLayout>(kj::mv(layout));
}
}  // namespace workerd::api::gpu
