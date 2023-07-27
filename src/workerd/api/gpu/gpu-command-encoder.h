// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUCommandEncoder : public jsg::Object {
public:
  explicit GPUCommandEncoder(wgpu::CommandEncoder e) : encoder_(kj::mv(e)){};
  JSG_RESOURCE_TYPE(GPUCommandEncoder) {}

private:
  wgpu::CommandEncoder encoder_;
};

struct GPUCommandEncoderDescriptor {
  jsg::Optional<kj::String> label;

  JSG_STRUCT(label);
};

} // namespace workerd::api::gpu
