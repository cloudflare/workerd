// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUShaderModule : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::ShaderModule &() const { return shader_; }
  explicit GPUShaderModule(wgpu::ShaderModule s) : shader_(kj::mv(s)){};
  JSG_RESOURCE_TYPE(GPUShaderModule) {}

private:
  wgpu::ShaderModule shader_;
};

struct GPUShaderModuleDescriptor {
  jsg::Optional<kj::String> label;
  kj::String code;

  JSG_STRUCT(label, code);
};

} // namespace workerd::api::gpu
