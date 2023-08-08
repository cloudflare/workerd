// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-command-buffer.h"
#include "gpu-compute-pass-encoder.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUCommandEncoder : public jsg::Object {
public:
  explicit GPUCommandEncoder(wgpu::CommandEncoder e) : encoder_(kj::mv(e)){};
  JSG_RESOURCE_TYPE(GPUCommandEncoder) {
    JSG_METHOD(beginComputePass);
    JSG_METHOD(copyBufferToBuffer);
    JSG_METHOD(finish);
  }

private:
  wgpu::CommandEncoder encoder_;
  jsg::Ref<GPUComputePassEncoder>
  beginComputePass(jsg::Optional<GPUComputePassDescriptor> descriptor);
  jsg::Ref<GPUCommandBuffer> finish(jsg::Optional<GPUCommandBufferDescriptor>);
  void copyBufferToBuffer(jsg::Ref<GPUBuffer> source, GPUSize64 sourceOffset,
                          jsg::Ref<GPUBuffer> destination,
                          GPUSize64 destinationOffset, GPUSize64 size);
};

struct GPUCommandEncoderDescriptor {
  jsg::Optional<kj::String> label;

  JSG_STRUCT(label);
};

} // namespace workerd::api::gpu
