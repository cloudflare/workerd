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
  wgpu::BufferBindingLayout layout;

  // the Dawn default here is Undefined, so we stick with what's in the spec
  layout.type = parseBufferBindingType(buffer.type.orDefault([] { return "uniform"_kj; }));

  KJ_IF_SOME(hasDynamicOffset, buffer.hasDynamicOffset) {
    layout.hasDynamicOffset = hasDynamicOffset;
  }
  KJ_IF_SOME(minBindingSize, buffer.minBindingSize) {
    layout.minBindingSize = minBindingSize;
  }

  return kj::mv(layout);
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
  wgpu::SamplerBindingLayout layout;
  layout.type = parseSamplerBindingType(sampler.type.orDefault([] { return "filtering"_kj; }));
  return kj::mv(layout);
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

wgpu::TextureBindingLayout parseTextureBindingLayout(GPUTextureBindingLayout& texture) {
  wgpu::TextureBindingLayout layout;
  layout.sampleType =
      parseTextureSampleType(texture.sampleType.orDefault([] { return "float"_kj; }));
  layout.viewDimension =
      parseTextureViewDimension(texture.viewDimension.orDefault([] { return "2d"_kj; }));
  layout.multisampled = texture.multisampled.orDefault(false);

  return kj::mv(layout);
}

wgpu::StorageTextureBindingLayout
parseStorageTextureBindingLayout(GPUStorageTextureBindingLayout& storage) {

  wgpu::StorageTextureBindingLayout layout;
  layout.access = parseStorageAccess(storage.access.orDefault([] { return "write-only"_kj; }));
  layout.format = parseTextureFormat(storage.format);
  layout.viewDimension =
      parseTextureViewDimension(storage.viewDimension.orDefault([] { return "2d"_kj; }));

  return kj::mv(layout);
}

wgpu::BindGroupLayoutEntry parseBindGroupLayoutEntry(GPUBindGroupLayoutEntry& entry) {
  wgpu::BindGroupLayoutEntry layoutEntry;
  layoutEntry.binding = entry.binding;
  layoutEntry.visibility = static_cast<wgpu::ShaderStage>(entry.visibility);

  KJ_IF_SOME(buffer, entry.buffer) {
    layoutEntry.buffer = parseBufferBindingLayout(buffer);
  }

  KJ_IF_SOME(sampler, entry.sampler) {
    layoutEntry.sampler = parseSamplerBindingLayout(sampler);
  }

  KJ_IF_SOME(texture, entry.texture) {
    layoutEntry.texture = parseTextureBindingLayout(texture);
  }

  KJ_IF_SOME(storage, entry.storageTexture) {
    layoutEntry.storageTexture = parseStorageTextureBindingLayout(storage);
  }

  return kj::mv(layoutEntry);
};

} // namespace workerd::api::gpu
