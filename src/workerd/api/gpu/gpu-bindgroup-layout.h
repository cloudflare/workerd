// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUBindGroupLayout : public jsg::Object {
public:
  explicit GPUBindGroupLayout(wgpu::BindGroupLayout l) : layout_(kj::mv(l)){};

  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::BindGroupLayout &() const { return layout_; }

  JSG_RESOURCE_TYPE(GPUBindGroupLayout) {}

private:
  wgpu::BindGroupLayout layout_;
};

struct GPUBufferBindingLayout {
  jsg::Optional<kj::String> type;
  jsg::Optional<bool> hasDynamicOffset;
  jsg::Optional<GPUSize64> minBindingSize;

  JSG_STRUCT(type, hasDynamicOffset, minBindingSize);
};

struct GPUSamplerBindingLayout {
  jsg::Optional<GPUSamplerBindingType> type;

  JSG_STRUCT(type);
};

struct GPUTextureBindingLayout {
  jsg::Optional<GPUTextureSampleType> sampleType;
  jsg::Optional<GPUTextureViewDimension> viewDimension;
  jsg::Optional<bool> multisampled;

  JSG_STRUCT(sampleType, viewDimension, multisampled);
};

struct GPUStorageTextureBindingLayout {
  jsg::Optional<GPUStorageTextureAccess> access;
  GPUTextureFormat format;
  jsg::Optional<GPUTextureViewDimension> viewDimension;

  JSG_STRUCT(access, format, viewDimension);
};

struct GPUBindGroupLayoutEntry {
  GPUIndex32 binding;
  GPUShaderStageFlags visibility;
  jsg::Optional<GPUBufferBindingLayout> buffer;
  jsg::Optional<GPUSamplerBindingLayout> sampler;
  jsg::Optional<GPUTextureBindingLayout> texture;
  jsg::Optional<GPUStorageTextureBindingLayout> storageTexture;
  // empty dict
  // jsg::Optional<GPUExternalTextureBindingLayout> externalTexture;

  JSG_STRUCT(binding, visibility, buffer, sampler, texture, storageTexture);
};

struct GPUBindGroupLayoutDescriptor {
  jsg::Optional<kj::String> label;
  kj::Array<GPUBindGroupLayoutEntry> entries;

  JSG_STRUCT(label, entries);
};

wgpu::BindGroupLayoutEntry parseBindGroupLayoutEntry(GPUBindGroupLayoutEntry &);

} // namespace workerd::api::gpu
