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

  KJ_FAIL_REQUIRE("unknown buffer binding type", bType);
}

wgpu::BufferBindingLayout
parseBufferBindingLayout(GPUBufferBindingLayout &buffer) {
  wgpu::BufferBindingLayout l;
  l.type = wgpu::BufferBindingType::Uniform;
  l.hasDynamicOffset = false;
  l.minBindingSize = 0;

  KJ_IF_MAYBE (type, buffer.type) {
    l.type = parseBufferBindingType(*type);
  }

  KJ_IF_MAYBE (offset, buffer.hasDynamicOffset) {
    l.hasDynamicOffset = *offset;
  }

  KJ_IF_MAYBE (minSize, buffer.minBindingSize) {
    l.minBindingSize = *minSize;
  }

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

  KJ_FAIL_REQUIRE("unknown sampler binding type", bType);
}

wgpu::SamplerBindingLayout
parseSamplerBindingLayout(GPUSamplerBindingLayout &sampler) {
  wgpu::SamplerBindingLayout s;
  s.type = wgpu::SamplerBindingType::Filtering;

  KJ_IF_MAYBE (sType, sampler.type) {
    s.type = parseSamplerBindingType(*sType);
  }

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

  KJ_FAIL_REQUIRE("unknown texture sample type", sType);
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

  KJ_FAIL_REQUIRE("unknown texture view dimension", dim);
}

wgpu::TextureBindingLayout
parseTextureBindingLayout(GPUTextureBindingLayout &texture) {
  wgpu::TextureBindingLayout t;
  t.sampleType = wgpu::TextureSampleType::Float;
  t.viewDimension = wgpu::TextureViewDimension::e2D;
  t.multisampled = false;

  KJ_IF_MAYBE (sType, texture.sampleType) {
    t.sampleType = parseTextureSampleType(*sType);
  }

  KJ_IF_MAYBE (dim, texture.viewDimension) {
    t.viewDimension = parseTextureViewDimension(*dim);
  }

  KJ_IF_MAYBE (ms, texture.multisampled) {
    t.multisampled = *ms;
  }

  return kj::mv(t);
}

wgpu::TextureFormat parseTextureFormat(kj::StringPtr format) {

  if (format == "r8unorm") {
    return wgpu::TextureFormat::R8Unorm;
  }

  if (format == "r8snorm") {
    return wgpu::TextureFormat::R8Snorm;
  }

  if (format == "r8uint") {
    return wgpu::TextureFormat::R8Uint;
  }

  if (format == "r8sint") {
    return wgpu::TextureFormat::R8Sint;
  }

  if (format == "r16uint") {
    return wgpu::TextureFormat::R16Uint;
  }

  if (format == "r16sint") {
    return wgpu::TextureFormat::R16Sint;
  }

  if (format == "r16float") {
    return wgpu::TextureFormat::R16Float;
  }

  if (format == "rg8unorm") {
    return wgpu::TextureFormat::RG8Unorm;
  }

  if (format == "rg8snorm") {
    return wgpu::TextureFormat::RG8Snorm;
  }

  if (format == "rg8uint") {
    return wgpu::TextureFormat::RG8Uint;
  }

  if (format == "rg8sint") {
    return wgpu::TextureFormat::RG8Sint;
  }

  if (format == "r32uint") {
    return wgpu::TextureFormat::R32Uint;
  }

  if (format == "r32sint") {
    return wgpu::TextureFormat::R32Sint;
  }

  if (format == "r32float") {
    return wgpu::TextureFormat::R32Float;
  }

  if (format == "rg16uint") {
    return wgpu::TextureFormat::RG16Uint;
  }

  if (format == "rg16sint") {
    return wgpu::TextureFormat::RG16Sint;
  }

  if (format == "rg16float") {
    return wgpu::TextureFormat::RG16Float;
  }

  if (format == "rgba8unorm") {
    return wgpu::TextureFormat::RGBA8Unorm;
  }

  if (format == "rgba8unorm-srgb") {
    return wgpu::TextureFormat::RGBA8UnormSrgb;
  }

  if (format == "rgba8snorm") {
    return wgpu::TextureFormat::RGBA8Snorm;
  }

  if (format == "rgba8uint") {
    return wgpu::TextureFormat::RGBA8Uint;
  }

  if (format == "rgba8sint") {
    return wgpu::TextureFormat::RGBA8Sint;
  }

  if (format == "bgra8unorm") {
    return wgpu::TextureFormat::BGRA8Unorm;
  }

  if (format == "bgra8unorm-srgb") {
    return wgpu::TextureFormat::BGRA8UnormSrgb;
  }

  if (format == "rgb9e5ufloat") {
    return wgpu::TextureFormat::RGB9E5Ufloat;
  }

  if (format == "rgb10a2unorm") {
    return wgpu::TextureFormat::RGB10A2Unorm;
  }

  if (format == "rg11b10ufloat") {
    return wgpu::TextureFormat::RG11B10Ufloat;
  }

  if (format == "rg32uint") {
    return wgpu::TextureFormat::RG32Uint;
  }

  if (format == "rg32sint") {
    return wgpu::TextureFormat::RG32Sint;
  }

  if (format == "rg32float") {
    return wgpu::TextureFormat::RG32Float;
  }

  if (format == "rgba16uint") {
    return wgpu::TextureFormat::RGBA16Uint;
  }

  if (format == "rgba16sint") {
    return wgpu::TextureFormat::RGBA16Sint;
  }

  if (format == "rgba16float") {
    return wgpu::TextureFormat::RGBA16Float;
  }

  if (format == "rgba32uint") {
    return wgpu::TextureFormat::RGBA32Uint;
  }

  if (format == "rgba32sint") {
    return wgpu::TextureFormat::RGBA32Sint;
  }

  if (format == "rgba32float") {
    return wgpu::TextureFormat::RGBA32Float;
  }

  if (format == "stencil8") {
    return wgpu::TextureFormat::Stencil8;
  }

  if (format == "depth16unorm") {
    return wgpu::TextureFormat::Depth16Unorm;
  }

  if (format == "depth24plus") {
    return wgpu::TextureFormat::Depth24Plus;
  }

  if (format == "depth24plus-stencil8") {
    return wgpu::TextureFormat::Depth24PlusStencil8;
  }

  if (format == "depth32float") {
    return wgpu::TextureFormat::Depth32Float;
  }

  if (format == "depth32float-stencil8") {
    return wgpu::TextureFormat::Depth32FloatStencil8;
  }

  if (format == "bc1-rgba-unorm") {
    return wgpu::TextureFormat::BC1RGBAUnorm;
  }

  if (format == "bc1-rgba-unorm-srgb") {
    return wgpu::TextureFormat::BC1RGBAUnormSrgb;
  }

  if (format == "bc2-rgba-unorm") {
    return wgpu::TextureFormat::BC2RGBAUnorm;
  }

  if (format == "bc2-rgba-unorm-srgb") {
    return wgpu::TextureFormat::BC2RGBAUnormSrgb;
  }

  if (format == "bc3-rgba-unorm") {
    return wgpu::TextureFormat::BC3RGBAUnorm;
  }

  if (format == "bc3-rgba-unorm-srgb") {
    return wgpu::TextureFormat::BC3RGBAUnormSrgb;
  }

  if (format == "bc4-r-unorm") {
    return wgpu::TextureFormat::BC4RUnorm;
  }

  if (format == "bc4-r-snorm") {
    return wgpu::TextureFormat::BC4RSnorm;
  }

  if (format == "bc5-rg-unorm") {
    return wgpu::TextureFormat::BC5RGUnorm;
  }

  if (format == "bc5-rg-snorm") {
    return wgpu::TextureFormat::BC5RGSnorm;
  }

  if (format == "bc6h-rgb-ufloat") {
    return wgpu::TextureFormat::BC6HRGBUfloat;
  }

  if (format == "bc6h-rgb-float") {
    return wgpu::TextureFormat::BC6HRGBFloat;
  }

  if (format == "bc7-rgba-unorm") {
    return wgpu::TextureFormat::BC7RGBAUnorm;
  }

  if (format == "bc7-rgba-unorm-srgb") {
    return wgpu::TextureFormat::BC7RGBAUnormSrgb;
  }

  if (format == "etc2-rgb8unorm") {
    return wgpu::TextureFormat::ETC2RGB8Unorm;
  }

  if (format == "etc2-rgb8unorm-srgb") {
    return wgpu::TextureFormat::ETC2RGB8UnormSrgb;
  }

  if (format == "etc2-rgb8a1unorm") {
    return wgpu::TextureFormat::ETC2RGB8A1Unorm;
  }

  if (format == "etc2-rgb8a1unorm-srgb") {
    return wgpu::TextureFormat::ETC2RGB8A1UnormSrgb;
  }

  if (format == "etc2-rgba8unorm") {
    return wgpu::TextureFormat::ETC2RGBA8Unorm;
  }

  if (format == "etc2-rgba8unorm-srgb") {
    return wgpu::TextureFormat::ETC2RGBA8UnormSrgb;
  }

  if (format == "eac-r11unorm") {
    return wgpu::TextureFormat::EACR11Unorm;
  }

  if (format == "eac-r11snorm") {
    return wgpu::TextureFormat::EACR11Snorm;
  }

  if (format == "eac-rg11unorm") {
    return wgpu::TextureFormat::EACRG11Unorm;
  }

  if (format == "eac-rg11snorm") {
    return wgpu::TextureFormat::EACRG11Snorm;
  }

  if (format == "astc-4x4-unorm") {
    return wgpu::TextureFormat::ASTC4x4Unorm;
  }

  if (format == "astc-4x4-unorm-srgb") {
    return wgpu::TextureFormat::ASTC4x4UnormSrgb;
  }

  if (format == "astc-5x4-unorm") {
    return wgpu::TextureFormat::ASTC5x4Unorm;
  }

  if (format == "astc-5x4-unorm-srgb") {
    return wgpu::TextureFormat::ASTC5x4UnormSrgb;
  }

  if (format == "astc-5x5-unorm") {
    return wgpu::TextureFormat::ASTC5x5Unorm;
  }

  if (format == "astc-5x5-unorm-srgb") {
    return wgpu::TextureFormat::ASTC5x5UnormSrgb;
  }

  if (format == "astc-6x5-unorm") {
    return wgpu::TextureFormat::ASTC6x5Unorm;
  }

  if (format == "astc-6x5-unorm-srgb") {
    return wgpu::TextureFormat::ASTC6x5UnormSrgb;
  }

  if (format == "astc-6x6-unorm") {
    return wgpu::TextureFormat::ASTC6x6Unorm;
  }

  if (format == "astc-6x6-unorm-srgb") {
    return wgpu::TextureFormat::ASTC6x6UnormSrgb;
  }

  if (format == "astc-8x5-unorm") {
    return wgpu::TextureFormat::ASTC8x5Unorm;
  }

  if (format == "astc-8x5-unorm-srgb") {
    return wgpu::TextureFormat::ASTC8x5UnormSrgb;
  }

  if (format == "astc-8x6-unorm") {
    return wgpu::TextureFormat::ASTC8x6Unorm;
  }

  if (format == "astc-8x6-unorm-srgb") {
    return wgpu::TextureFormat::ASTC8x6UnormSrgb;
  }

  if (format == "astc-8x8-unorm") {
    return wgpu::TextureFormat::ASTC8x8Unorm;
  }

  if (format == "astc-8x8-unorm-srgb") {
    return wgpu::TextureFormat::ASTC8x8UnormSrgb;
  }

  if (format == "astc-10x5-unorm") {
    return wgpu::TextureFormat::ASTC10x5Unorm;
  }

  if (format == "astc-10x5-unorm-srgb") {
    return wgpu::TextureFormat::ASTC10x5UnormSrgb;
  }

  if (format == "astc-10x6-unorm") {
    return wgpu::TextureFormat::ASTC10x6Unorm;
  }

  if (format == "astc-10x6-unorm-srgb") {
    return wgpu::TextureFormat::ASTC10x6UnormSrgb;
  }

  if (format == "astc-10x8-unorm") {
    return wgpu::TextureFormat::ASTC10x8Unorm;
  }

  if (format == "astc-10x8-unorm-srgb") {
    return wgpu::TextureFormat::ASTC10x8UnormSrgb;
  }

  if (format == "astc-10x10-unorm") {
    return wgpu::TextureFormat::ASTC10x10Unorm;
  }

  if (format == "astc-10x10-unorm-srgb") {
    return wgpu::TextureFormat::ASTC10x10UnormSrgb;
  }

  if (format == "astc-12x10-unorm") {
    return wgpu::TextureFormat::ASTC12x10Unorm;
  }

  if (format == "astc-12x10-unorm-srgb") {
    return wgpu::TextureFormat::ASTC12x10UnormSrgb;
  }

  if (format == "astc-12x12-unorm") {
    return wgpu::TextureFormat::ASTC12x12Unorm;
  }

  if (format == "astc-12x12-unorm-srgb") {
    return wgpu::TextureFormat::ASTC12x12UnormSrgb;
  }

  KJ_FAIL_REQUIRE("unknown texture format", format);
}

wgpu::StorageTextureAccess parseStorageAccess(kj::StringPtr access) {
  if (access == "write-only") {
    return wgpu::StorageTextureAccess::WriteOnly;
  }

  KJ_FAIL_REQUIRE("unknown storage access", access);
}

wgpu::StorageTextureBindingLayout
parseStorageTextureBindingLayout(GPUStorageTextureBindingLayout &storage) {

  wgpu::StorageTextureBindingLayout s;
  s.access = wgpu::StorageTextureAccess::WriteOnly;
  s.format = parseTextureFormat(storage.format);
  s.viewDimension = wgpu::TextureViewDimension::e2D;

  KJ_IF_MAYBE (access, storage.access) {
    s.access = parseStorageAccess(*access);
  }

  KJ_IF_MAYBE (dimension, storage.viewDimension) {
    s.viewDimension = parseTextureViewDimension(*dimension);
  }

  return kj::mv(s);
}

wgpu::BindGroupLayoutEntry
parseBindGroupLayoutEntry(GPUBindGroupLayoutEntry &entry) {
  wgpu::BindGroupLayoutEntry e;
  e.binding = entry.binding;
  e.visibility = static_cast<wgpu::ShaderStage>(entry.visibility);

  KJ_IF_MAYBE (buffer, entry.buffer) {
    e.buffer = parseBufferBindingLayout(*buffer);
  }

  KJ_IF_MAYBE (sampler, entry.sampler) {
    e.sampler = parseSamplerBindingLayout(*sampler);
  }

  KJ_IF_MAYBE (texture, entry.texture) {
    e.texture = parseTextureBindingLayout(*texture);
  }

  KJ_IF_MAYBE (storage, entry.storageTexture) {
    e.storageTexture = parseStorageTextureBindingLayout(*storage);
  }

  return kj::mv(e);
};

} // namespace workerd::api::gpu
