// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-command-encoder.h"
#include "gpu-command-buffer.h"

namespace workerd::api::gpu {

jsg::Ref<GPUCommandBuffer> GPUCommandEncoder::finish(
    jsg::Optional<GPUCommandBufferDescriptor> descriptor) {
  wgpu::CommandBufferDescriptor desc{};

  KJ_IF_MAYBE (d, descriptor) {
    KJ_IF_MAYBE (label, d->label) {
      desc.label = label->cStr();
    }
  }

  auto buffer = encoder_.Finish(&desc);
  return jsg::alloc<GPUCommandBuffer>(kj::mv(buffer));
};

void GPUCommandEncoder::copyBufferToBuffer(jsg::Ref<GPUBuffer> source,
                                           GPUSize64 sourceOffset,
                                           jsg::Ref<GPUBuffer> destination,
                                           GPUSize64 destinationOffset,
                                           GPUSize64 size) {

  encoder_.CopyBufferToBuffer(*source, sourceOffset, *destination,
                              destinationOffset, size);
};

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
