// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-command-encoder.h"

namespace workerd::api::gpu {

jsg::Ref<GPUComputePassEncoder> GPUCommandEncoder::beginComputePass(
    jsg::Optional<GPUComputePassDescriptor> descriptor) {

  wgpu::ComputePassDescriptor desc{};

  kj::Vector<wgpu::ComputePassTimestampWrite> timestamps;
  KJ_IF_MAYBE (d, descriptor) {
    KJ_IF_MAYBE (label, d->label) {
      desc.label = label->cStr();
    }

    for (auto &timestamp : d->timestampWrites) {
      wgpu::ComputePassTimestampWrite t{};
      t.querySet = *timestamp.querySet;
      t.queryIndex = timestamp.queryIndex;
      t.location = parseComputePassTimestampLocation(timestamp.location);
      timestamps.add(kj::mv(t));
    }
  }
  desc.timestampWrites = timestamps.begin();
  desc.timestampWriteCount = timestamps.size();

  auto computePassEncoder = encoder_.BeginComputePass(&desc);
  return jsg::alloc<GPUComputePassEncoder>(kj::mv(computePassEncoder));
}

} // namespace workerd::api::gpu
