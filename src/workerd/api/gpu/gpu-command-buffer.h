// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUCommandBuffer : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::CommandBuffer &() const { return cmd_buf_; }
  explicit GPUCommandBuffer(wgpu::CommandBuffer b) : cmd_buf_(kj::mv(b)){};
  JSG_RESOURCE_TYPE(GPUCommandBuffer) {}

private:
  wgpu::CommandBuffer cmd_buf_;
};

struct GPUCommandBufferDescriptor {
  jsg::Optional<kj::String> label;

  JSG_STRUCT(label);
};

} // namespace workerd::api::gpu
