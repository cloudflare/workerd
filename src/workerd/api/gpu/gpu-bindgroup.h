// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-bindgroup-layout.h"
#include "gpu-buffer.h"
#include "gpu-sampler.h"
#include "gpu-utils.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUBindGroup : public jsg::Object {
public:
  explicit GPUBindGroup(wgpu::BindGroup g) : group_(kj::mv(g)){};
  JSG_RESOURCE_TYPE(GPUBindGroup) {}

private:
  wgpu::BindGroup group_;
};

struct GPUBufferBinding {
  jsg::Ref<GPUBuffer> buffer;
  jsg::Optional<GPUSize64> offset;
  jsg::Optional<GPUSize64> size;

  JSG_STRUCT(buffer, offset, size);
};

// TODO add GPUTextureView and GPUExternalTexture
using GPUBindingResource = kj::OneOf<GPUBufferBinding, jsg::Ref<GPUSampler>>;

struct GPUBindGroupEntry {
  GPUIndex32 binding;
  GPUBindingResource resource;

  JSG_STRUCT(binding, resource);
};

struct GPUBindGroupDescriptor {
  jsg::Optional<kj::String> label;
  jsg::Ref<GPUBindGroupLayout> layout;
  kj::Array<GPUBindGroupEntry> entries;

  JSG_STRUCT(label, layout, entries);
};

wgpu::BindGroupEntry parseBindGroupEntry(GPUBindGroupEntry &);

} // namespace workerd::api::gpu
