// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-compute-pass-encoder.h"

namespace workerd::api::gpu {

void GPUComputePassEncoder::setPipeline(jsg::Ref<GPUComputePipeline> pipeline) {
  encoder_.SetPipeline(*pipeline);
}

void GPUComputePassEncoder::dispatchWorkgroups(GPUSize32 workgroupCountX,
    jsg::Optional<GPUSize32> workgroupCountY,
    jsg::Optional<GPUSize32> workgroupCountZ) {

  GPUSize32 countY = workgroupCountY.orDefault(1);
  GPUSize32 countZ = workgroupCountZ.orDefault(1);

  encoder_.DispatchWorkgroups(workgroupCountX, countY, countZ);
}

void GPUComputePassEncoder::end() {
  encoder_.End();
}

void GPUComputePassEncoder::setBindGroup(GPUIndex32 index,
    kj::Maybe<jsg::Ref<GPUBindGroup>> bindGroup,
    jsg::Optional<jsg::Sequence<GPUBufferDynamicOffset>> dynamicOffsets) {
  wgpu::BindGroup bg = nullptr;

  KJ_IF_SOME(bgroup, bindGroup) {
    bg = *bgroup;
  }

  uint32_t* offsets = nullptr;
  uint32_t num_offsets = 0;

  KJ_IF_SOME(dos, dynamicOffsets) {
    offsets = dos.begin();
    num_offsets = dos.size();
  }

  encoder_.SetBindGroup(index, bg, num_offsets, offsets);
}

}  // namespace workerd::api::gpu
