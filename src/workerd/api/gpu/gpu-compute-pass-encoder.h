// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-bindgroup.h"
#include "gpu-compute-pipeline.h"
#include "gpu-query-set.h"
#include "gpu-utils.h"

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUComputePassEncoder: public jsg::Object {
public:
  explicit GPUComputePassEncoder(wgpu::ComputePassEncoder e): encoder_(kj::mv(e)) {};
  JSG_RESOURCE_TYPE(GPUComputePassEncoder) {
    JSG_METHOD(setPipeline);
    JSG_METHOD(setBindGroup);
    JSG_METHOD(dispatchWorkgroups);
    JSG_METHOD(end);
  }

private:
  wgpu::ComputePassEncoder encoder_;
  void setPipeline(jsg::Ref<GPUComputePipeline> pipeline);
  void dispatchWorkgroups(GPUSize32 workgroupCountX,
      jsg::Optional<GPUSize32> workgroupCountY,
      jsg::Optional<GPUSize32> workgroupCountZ);
  void end();
  void setBindGroup(GPUIndex32 index,
      kj::Maybe<jsg::Ref<GPUBindGroup>> bindGroup,
      jsg::Optional<jsg::Sequence<GPUBufferDynamicOffset>> dynamicOffsets);
  // TODO(soon): overloads don't seem to be supported
  // void setBindGroup(GPUIndex32 index,
  //                  kj::Maybe<jsg::Ref<GPUBindGroup>> bindGroup,
  //                  kj::Array<uint32_t> dynamicOffsetsData,
  //                  GPUSize64 dynamicOffsetsDataStart,
  //                  GPUSize32 dynamicOffsetsDataLength);
};

struct GPUComputePassTimestampWrites {
  jsg::Ref<GPUQuerySet> querySet;
  jsg::Optional<GPUSize32> beginningOfPassWriteIndex;
  jsg::Optional<GPUSize32> endOfPassWriteIndex;

  JSG_STRUCT(querySet, beginningOfPassWriteIndex, endOfPassWriteIndex);
};

struct GPUComputePassDescriptor {
  jsg::Optional<kj::String> label;

  jsg::Optional<GPUComputePassTimestampWrites> timestampWrites;

  JSG_STRUCT(label, timestampWrites);
};

}  // namespace workerd::api::gpu
