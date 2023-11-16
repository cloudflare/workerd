// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-render-pass-encoder.h"

namespace workerd::api::gpu {

void GPURenderPassEncoder::setPipeline(jsg::Ref<GPURenderPipeline> pipeline) {
  encoder_.SetPipeline(*pipeline);
}

void GPURenderPassEncoder::draw(GPUSize32 vertexCount, jsg::Optional<GPUSize32> instanceCount,
                                jsg::Optional<GPUSize32> firstVertex,
                                jsg::Optional<GPUSize32> firstInstance) {
  KJ_IF_SOME(iCount, instanceCount) {
    KJ_IF_SOME(fVertex, firstVertex) {
      KJ_IF_SOME(fInstance, firstInstance) {
        return encoder_.Draw(vertexCount, iCount, fVertex, fInstance);
      }
      return encoder_.Draw(vertexCount, iCount, fVertex);
    }
    return encoder_.Draw(vertexCount, iCount);
  }
  return encoder_.Draw(vertexCount);
}

} // namespace workerd::api::gpu
