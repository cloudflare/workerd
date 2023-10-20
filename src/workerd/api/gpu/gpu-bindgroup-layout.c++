// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-bindgroup-layout.h"

namespace workerd::api::gpu {

wgpu::BufferBindingType parseBufferBindingType(kj::StringPtr bType) {
  if (bType == "uniform") {
    return wgpu::BufferBindingType::Uniform;
  }

  if (bType == "storage") {
    return wgpu::BufferBindingType::Storage;
  }

  if (bType == "read-only-storage") {
    return wgpu::BufferBindingType::ReadOnlyStorage;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown buffer binding type", bType);
}

wgpu::BufferBindingLayout parseBufferBindingLayout(GPUBufferBindingLayout& buffer) {
  wgpu::BufferBindingLayout l;

  l.type = parseBufferBindingType(buffer.type.orDefault([] { return "uniform"_kj; }));
  l.hasDynamicOffset = buffer.hasDynamicOffset.orDefault(false);
  l.minBindingSize = buffer.minBindingSize.orDefault(0);

  return kj::mv(l);
}

wgpu::SamplerBindingType parseSamplerBindingType(kj::StringPtr bType) {
  if (bType == "filtering") {
    return wgpu::SamplerBindingType::Filtering;
  }

  if (bType == "non-filtering") {
    return wgpu::SamplerBindingType::NonFiltering;
  }

  if (bType == "comparison") {
    return wgpu::SamplerBindingType::Comparison;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown sampler binding type", bType);
}

wgpu::SamplerBindingLayout parseSamplerBindingLayout(GPUSamplerBindingLayout& sampler) {
  wgpu::SamplerBindingLayout s;
  s.type = parseSamplerBindingType(sampler.type.orDefault([] { return "filtering"_kj; }));

  return kj::mv(s);
}

wgpu::TextureSampleType parseTextureSampleType(kj::StringPtr sType) {
  if (sType == "float") {
    return wgpu::TextureSampleType::Float;
  }

  if (sType == "unfilterable-float") {
    return wgpu::TextureSampleType::UnfilterableFloat;
  }

  if (sType == "depth") {
    return wgpu::TextureSampleType::Depth;
  }

  if (sType == "sint") {
    return wgpu::TextureSampleType::Sint;
  }

  if (sType == "uint") {
    return wgpu::TextureSampleType::Uint;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown texture sample type", sType);
}

wgpu::TextureViewDimension parseTextureViewDimension(kj::StringPtr dim) {

  if (dim == "1d") {
    return wgpu::TextureViewDimension::e1D;
  }

  if (dim == "2d") {
    return wgpu::TextureViewDimension::e2D;
  }

  if (dim == "2d-array") {
    return wgpu::TextureViewDimension::e2DArray;
  }

  if (dim == "cube") {
    return wgpu::TextureViewDimension::Cube;
  }

  if (dim == "cube-array") {
    return wgpu::TextureViewDimension::CubeArray;
  }

  if (dim == "3d") {
    return wgpu::TextureViewDimension::e3D;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown texture view dimension", dim);
}

wgpu::TextureBindingLayout parseTextureBindingLayout(GPUTextureBindingLayout& texture) {
  wgpu::TextureBindingLayout t;
  t.sampleType = parseTextureSampleType(texture.sampleType.orDefault([] { return "float"_kj; }));
  t.viewDimension =
      parseTextureViewDimension(texture.viewDimension.orDefault([] { return "2d"_kj; }));
  t.multisampled = texture.multisampled.orDefault(false);

  return kj::mv(t);
}

wgpu::StorageTextureAccess parseStorageAccess(kj::StringPtr access) {
  if (access == "write-only") {
    return wgpu::StorageTextureAccess::WriteOnly;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown storage access", access);
}

wgpu::StorageTextureBindingLayout
parseStorageTextureBindingLayout(GPUStorageTextureBindingLayout& storage) {

  wgpu::StorageTextureBindingLayout s;
  s.access = parseStorageAccess(storage.access.orDefault([] { return "write-only"_kj; }));
  s.format = parseTextureFormat(storage.format);
  s.viewDimension =
      parseTextureViewDimension(storage.viewDimension.orDefault([] { return "2d"_kj; }));

  return kj::mv(s);
}

wgpu::BindGroupLayoutEntry parseBindGroupLayoutEntry(GPUBindGroupLayoutEntry& entry) {
  wgpu::BindGroupLayoutEntry e;
  e.binding = entry.binding;
  e.visibility = static_cast<wgpu::ShaderStage>(entry.visibility);

  KJ_IF_SOME(buffer, entry.buffer) {
    e.buffer = parseBufferBindingLayout(buffer);
  }

  KJ_IF_SOME(sampler, entry.sampler) {
    e.sampler = parseSamplerBindingLayout(sampler);
  }

  KJ_IF_SOME(texture, entry.texture) {
    e.texture = parseTextureBindingLayout(texture);
  }

  KJ_IF_SOME(storage, entry.storageTexture) {
    e.storageTexture = parseStorageTextureBindingLayout(storage);
  }

  return kj::mv(e);
};

} // namespace workerd::api::gpu
