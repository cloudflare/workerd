// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-utils.h"

#include <webgpu/webgpu_cpp.h>

#include <map>

namespace workerd::api::gpu {

wgpu::StoreOp parseGPUStoreOp(kj::StringPtr storeOp) {
  static std::map<kj::StringPtr, wgpu::StoreOp> mapping{
    {"store", wgpu::StoreOp::Store},
    {"discard", wgpu::StoreOp::Discard},
  };
  auto found = mapping.find(storeOp);
  JSG_REQUIRE(found != mapping.end(), TypeError, "unload GPU store operation: ", storeOp);
  return found->second;
}

wgpu::LoadOp parseGPULoadOp(kj::StringPtr loadOp) {
  static std::map<kj::StringPtr, wgpu::LoadOp> mapping{
    {"load", wgpu::LoadOp::Load},
    {"clear", wgpu::LoadOp::Clear},
  };
  auto found = mapping.find(loadOp);
  JSG_REQUIRE(found != mapping.end(), TypeError, "unload GPU load operation: ", loadOp);
  return found->second;
}

// TODO(soon): use a static std::map for most of these functions, as seen
// in kj::StringPtr CryptoImpl::getAsymmetricKeyType().
wgpu::FeatureName parseFeatureName(GPUFeatureName& str) {
  if (str == "depth-clip-control") {
    return wgpu::FeatureName::DepthClipControl;
  }
  if (str == "depth32float-stencil8") {
    return wgpu::FeatureName::Depth32FloatStencil8;
  }
  if (str == "texture-compression-bc") {
    return wgpu::FeatureName::TextureCompressionBC;
  }
  if (str == "texture-compression-etc2") {
    return wgpu::FeatureName::TextureCompressionETC2;
  }
  if (str == "texture-compression-astc") {
    return wgpu::FeatureName::TextureCompressionASTC;
  }
  if (str == "timestamp-query") {
    return wgpu::FeatureName::TimestampQuery;
  }
  if (str == "indirect-first-instance") {
    return wgpu::FeatureName::IndirectFirstInstance;
  }
  if (str == "shader-f16") {
    return wgpu::FeatureName::ShaderF16;
  }
  if (str == "rg11b10ufloat-renderable") {
    return wgpu::FeatureName::RG11B10UfloatRenderable;
  }
  if (str == "bgra8unorm-storage") {
    return wgpu::FeatureName::BGRA8UnormStorage;
  }
  if (str == "float32-filterable") {
    return wgpu::FeatureName::Float32Filterable;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown GPU feature: ", str);
}

GPUTextureDimension getTextureDimension(wgpu::TextureDimension& dimension) {

  switch (dimension) {
    case wgpu::TextureDimension::e1D:
      return kj::str("1d");
    case wgpu::TextureDimension::e2D:
      return kj::str("2d");
    case wgpu::TextureDimension::e3D:
      return kj::str("3d");
    default:
      KJ_UNREACHABLE
  }
}

GPUTextureFormat getTextureFormat(wgpu::TextureFormat& format) {
  switch (format) {
    case wgpu::TextureFormat::R8Unorm:
      return kj::str("r8unorm");

    case wgpu::TextureFormat::R8Snorm:
      return kj::str("r8snorm");

    case wgpu::TextureFormat::R8Uint:
      return kj::str("r8uint");

    case wgpu::TextureFormat::R8Sint:
      return kj::str("r8sint");

    case wgpu::TextureFormat::R16Uint:
      return kj::str("r16uint");

    case wgpu::TextureFormat::R16Sint:
      return kj::str("r16sint");

    case wgpu::TextureFormat::R16Float:
      return kj::str("r16float");

    case wgpu::TextureFormat::RG8Unorm:
      return kj::str("rg8unorm");

    case wgpu::TextureFormat::RG8Snorm:
      return kj::str("rg8snorm");

    case wgpu::TextureFormat::RG8Uint:
      return kj::str("rg8uint");

    case wgpu::TextureFormat::RG8Sint:
      return kj::str("rg8sint");

    case wgpu::TextureFormat::R32Uint:
      return kj::str("r32uint");

    case wgpu::TextureFormat::R32Sint:
      return kj::str("r32sint");

    case wgpu::TextureFormat::R32Float:
      return kj::str("r32float");

    case wgpu::TextureFormat::RG16Uint:
      return kj::str("rg16uint");

    case wgpu::TextureFormat::RG16Sint:
      return kj::str("rg16sint");

    case wgpu::TextureFormat::RG16Float:
      return kj::str("rg16float");

    case wgpu::TextureFormat::RGBA8Unorm:
      return kj::str("rgba8unorm");

    case wgpu::TextureFormat::RGBA8UnormSrgb:
      return kj::str("rgba8unorm-srgb");

    case wgpu::TextureFormat::RGBA8Snorm:
      return kj::str("rgba8snorm");

    case wgpu::TextureFormat::RGBA8Uint:
      return kj::str("rgba8uint");

    case wgpu::TextureFormat::RGBA8Sint:
      return kj::str("rgba8sint");

    case wgpu::TextureFormat::BGRA8Unorm:
      return kj::str("bgra8unorm");

    case wgpu::TextureFormat::BGRA8UnormSrgb:
      return kj::str("bgra8unorm-srgb");

    case wgpu::TextureFormat::RGB9E5Ufloat:
      return kj::str("rgb9e5ufloat");

    case wgpu::TextureFormat::RGB10A2Unorm:
      return kj::str("rgb10a2unorm");

    case wgpu::TextureFormat::RG11B10Ufloat:
      return kj::str("rg11b10ufloat");

    case wgpu::TextureFormat::RG32Uint:
      return kj::str("rg32uint");

    case wgpu::TextureFormat::RG32Sint:
      return kj::str("rg32sint");

    case wgpu::TextureFormat::RG32Float:
      return kj::str("rg32float");

    case wgpu::TextureFormat::RGBA16Uint:
      return kj::str("rgba16uint");

    case wgpu::TextureFormat::RGBA16Sint:
      return kj::str("rgba16sint");

    case wgpu::TextureFormat::RGBA16Float:
      return kj::str("rgba16float");

    case wgpu::TextureFormat::RGBA32Uint:
      return kj::str("rgba32uint");

    case wgpu::TextureFormat::RGBA32Sint:
      return kj::str("rgba32sint");

    case wgpu::TextureFormat::RGBA32Float:
      return kj::str("rgba32float");

    case wgpu::TextureFormat::Stencil8:
      return kj::str("stencil8");

    case wgpu::TextureFormat::Depth16Unorm:
      return kj::str("depth16unorm");

    case wgpu::TextureFormat::Depth24Plus:
      return kj::str("depth24plus");

    case wgpu::TextureFormat::Depth24PlusStencil8:
      return kj::str("depth24plus-stencil8");

    case wgpu::TextureFormat::Depth32Float:
      return kj::str("depth32float");

    case wgpu::TextureFormat::Depth32FloatStencil8:
      return kj::str("depth32float-stencil8");

    case wgpu::TextureFormat::BC1RGBAUnorm:
      return kj::str("bc1-rgba-unorm");

    case wgpu::TextureFormat::BC1RGBAUnormSrgb:
      return kj::str("bc1-rgba-unorm-srgb");

    case wgpu::TextureFormat::BC2RGBAUnorm:
      return kj::str("bc2-rgba-unorm");

    case wgpu::TextureFormat::BC2RGBAUnormSrgb:
      return kj::str("bc2-rgba-unorm-srgb");

    case wgpu::TextureFormat::BC3RGBAUnorm:
      return kj::str("bc3-rgba-unorm");

    case wgpu::TextureFormat::BC3RGBAUnormSrgb:
      return kj::str("bc3-rgba-unorm-srgb");

    case wgpu::TextureFormat::BC4RUnorm:
      return kj::str("bc4-r-unorm");

    case wgpu::TextureFormat::BC4RSnorm:
      return kj::str("bc4-r-snorm");

    case wgpu::TextureFormat::BC5RGUnorm:
      return kj::str("bc5-rg-unorm");

    case wgpu::TextureFormat::BC5RGSnorm:
      return kj::str("bc5-rg-snorm");

    case wgpu::TextureFormat::BC6HRGBUfloat:
      return kj::str("bc6h-rgb-ufloat");

    case wgpu::TextureFormat::BC6HRGBFloat:
      return kj::str("bc6h-rgb-float");

    case wgpu::TextureFormat::BC7RGBAUnorm:
      return kj::str("bc7-rgba-unorm");

    case wgpu::TextureFormat::BC7RGBAUnormSrgb:
      return kj::str("bc7-rgba-unorm-srgb");

    case wgpu::TextureFormat::ETC2RGB8Unorm:
      return kj::str("etc2-rgb8unorm");

    case wgpu::TextureFormat::ETC2RGB8UnormSrgb:
      return kj::str("etc2-rgb8unorm-srgb");

    case wgpu::TextureFormat::ETC2RGB8A1Unorm:
      return kj::str("etc2-rgb8a1unorm");

    case wgpu::TextureFormat::ETC2RGB8A1UnormSrgb:
      return kj::str("etc2-rgb8a1unorm-srgb");

    case wgpu::TextureFormat::ETC2RGBA8Unorm:
      return kj::str("etc2-rgba8unorm");

    case wgpu::TextureFormat::ETC2RGBA8UnormSrgb:
      return kj::str("etc2-rgba8unorm-srgb");

    case wgpu::TextureFormat::EACR11Unorm:
      return kj::str("eac-r11unorm");

    case wgpu::TextureFormat::EACR11Snorm:
      return kj::str("eac-r11snorm");

    case wgpu::TextureFormat::EACRG11Unorm:
      return kj::str("eac-rg11unorm");

    case wgpu::TextureFormat::EACRG11Snorm:
      return kj::str("eac-rg11snorm");

    case wgpu::TextureFormat::ASTC4x4Unorm:
      return kj::str("astc-4x4-unorm");

    case wgpu::TextureFormat::ASTC4x4UnormSrgb:
      return kj::str("astc-4x4-unorm-srgb");

    case wgpu::TextureFormat::ASTC5x4Unorm:
      return kj::str("astc-5x4-unorm");

    case wgpu::TextureFormat::ASTC5x4UnormSrgb:
      return kj::str("astc-5x4-unorm-srgb");

    case wgpu::TextureFormat::ASTC5x5Unorm:
      return kj::str("astc-5x5-unorm");

    case wgpu::TextureFormat::ASTC5x5UnormSrgb:
      return kj::str("astc-5x5-unorm-srgb");

    case wgpu::TextureFormat::ASTC6x5Unorm:
      return kj::str("astc-6x5-unorm");

    case wgpu::TextureFormat::ASTC6x5UnormSrgb:
      return kj::str("astc-6x5-unorm-srgb");

    case wgpu::TextureFormat::ASTC6x6Unorm:
      return kj::str("astc-6x6-unorm");

    case wgpu::TextureFormat::ASTC6x6UnormSrgb:
      return kj::str("astc-6x6-unorm-srgb");

    case wgpu::TextureFormat::ASTC8x5Unorm:
      return kj::str("astc-8x5-unorm");

    case wgpu::TextureFormat::ASTC8x5UnormSrgb:
      return kj::str("astc-8x5-unorm-srgb");

    case wgpu::TextureFormat::ASTC8x6Unorm:
      return kj::str("astc-8x6-unorm");

    case wgpu::TextureFormat::ASTC8x6UnormSrgb:
      return kj::str("astc-8x6-unorm-srgb");

    case wgpu::TextureFormat::ASTC8x8Unorm:
      return kj::str("astc-8x8-unorm");

    case wgpu::TextureFormat::ASTC8x8UnormSrgb:
      return kj::str("astc-8x8-unorm-srgb");

    case wgpu::TextureFormat::ASTC10x5Unorm:
      return kj::str("astc-10x5-unorm");

    case wgpu::TextureFormat::ASTC10x5UnormSrgb:
      return kj::str("astc-10x5-unorm-srgb");

    case wgpu::TextureFormat::ASTC10x6Unorm:
      return kj::str("astc-10x6-unorm");

    case wgpu::TextureFormat::ASTC10x6UnormSrgb:
      return kj::str("astc-10x6-unorm-srgb");

    case wgpu::TextureFormat::ASTC10x8Unorm:
      return kj::str("astc-10x8-unorm");

    case wgpu::TextureFormat::ASTC10x8UnormSrgb:
      return kj::str("astc-10x8-unorm-srgb");

    case wgpu::TextureFormat::ASTC10x10Unorm:
      return kj::str("astc-10x10-unorm");

    case wgpu::TextureFormat::ASTC10x10UnormSrgb:
      return kj::str("astc-10x10-unorm-srgb");

    case wgpu::TextureFormat::ASTC12x10Unorm:
      return kj::str("astc-12x10-unorm");

    case wgpu::TextureFormat::ASTC12x10UnormSrgb:
      return kj::str("astc-12x10-unorm-srgb");

    case wgpu::TextureFormat::ASTC12x12Unorm:
      return kj::str("astc-12x12-unorm");

    case wgpu::TextureFormat::ASTC12x12UnormSrgb:
      return kj::str("astc-12x12-unorm-srgb");
    default:
      KJ_UNREACHABLE
  }
}

kj::Maybe<GPUFeatureName> getFeatureName(wgpu::FeatureName& feature) {
  switch (feature) {
    case wgpu::FeatureName::DepthClipControl:
      return kj::str("depth-clip-control");
    case wgpu::FeatureName::Depth32FloatStencil8:
      return kj::str("depth32float-stencil8");
    case wgpu::FeatureName::TextureCompressionBC:
      return kj::str("texture-compression-bc");
    case wgpu::FeatureName::TextureCompressionETC2:
      return kj::str("texture-compression-etc2");
    case wgpu::FeatureName::TextureCompressionASTC:
      return kj::str("texture-compression-astc");
    case wgpu::FeatureName::TimestampQuery:
      return kj::str("timestamp-query");
    case wgpu::FeatureName::IndirectFirstInstance:
      return kj::str("indirect-first-instance");
    case wgpu::FeatureName::ShaderF16:
      return kj::str("shader-f16");
    case wgpu::FeatureName::RG11B10UfloatRenderable:
      return kj::str("rg11b10ufloat-renderable");
    case wgpu::FeatureName::BGRA8UnormStorage:
      return kj::str("bgra8unorm-storage");
    case wgpu::FeatureName::Float32Filterable:
      return kj::str("float32-filterable");
    default:
      break;
  }

  return kj::none;
}

wgpu::TextureDimension parseTextureDimension(kj::StringPtr dimension) {
  if (dimension == "1d") {
    return wgpu::TextureDimension::e1D;
  }

  if (dimension == "2d") {
    return wgpu::TextureDimension::e2D;
  }

  if (dimension == "3d") {
    return wgpu::TextureDimension::e3D;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown texture dimension: ", dimension);
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

  JSG_FAIL_REQUIRE(TypeError, "unknown texture format: ", format);
}

wgpu::TextureAspect parseTextureAspect(kj::StringPtr aspect) {
  if (aspect == "all") {
    return wgpu::TextureAspect::All;
  }

  if (aspect == "stencil-only") {
    return wgpu::TextureAspect::StencilOnly;
  }

  if (aspect == "depth-only") {
    return wgpu::TextureAspect::DepthOnly;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown aspect: ", aspect);
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

  JSG_FAIL_REQUIRE(TypeError, "unknown texture view dimension: ", dim);
}

wgpu::StorageTextureAccess parseStorageAccess(kj::StringPtr access) {
  if (access == "write-only") {
    return wgpu::StorageTextureAccess::WriteOnly;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown storage access: ", access);
}

wgpu::PrimitiveTopology parsePrimitiveTopology(kj::StringPtr topology) {
  if (topology == "point-list") {
    return wgpu::PrimitiveTopology::PointList;
  }

  if (topology == "line-list") {
    return wgpu::PrimitiveTopology::LineList;
  }

  if (topology == "line-strip") {
    return wgpu::PrimitiveTopology::LineStrip;
  }

  if (topology == "triangle-list") {
    return wgpu::PrimitiveTopology::TriangleList;
  }

  if (topology == "triangle-strip") {
    return wgpu::PrimitiveTopology::TriangleList;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown primitive topology: ", topology);
}

wgpu::IndexFormat parseIndexFormat(kj::StringPtr format) {
  if (format == "uint16") {
    return wgpu::IndexFormat::Uint16;
  }

  if (format == "uint32") {
    return wgpu::IndexFormat::Uint32;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown index format: ", format);
}

wgpu::FrontFace parseFrontFace(kj::StringPtr frontFace) {
  if (frontFace == "ccw") {
    return wgpu::FrontFace::CCW;
  }

  if (frontFace == "cw") {
    return wgpu::FrontFace::CW;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown front face: ", frontFace);
}

wgpu::CullMode parseCullMode(kj::StringPtr mode) {
  if (mode == "none") {
    return wgpu::CullMode::None;
  }

  if (mode == "front") {
    return wgpu::CullMode::Front;
  }

  if (mode == "back") {
    return wgpu::CullMode::Back;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown cull mode: ", mode);
}

wgpu::StencilOperation parseStencilOperation(kj::StringPtr operation) {

  if (operation == "keep") {
    return wgpu::StencilOperation::Keep;
  }

  if (operation == "zero") {
    return wgpu::StencilOperation::Zero;
  }

  if (operation == "replace") {
    return wgpu::StencilOperation::Replace;
  }

  if (operation == "invert") {
    return wgpu::StencilOperation::Invert;
  }

  if (operation == "increment-clamp") {
    return wgpu::StencilOperation::IncrementClamp;
  }

  if (operation == "decrement-clamp") {
    return wgpu::StencilOperation::DecrementClamp;
  }

  if (operation == "increment-wrap") {
    return wgpu::StencilOperation::IncrementWrap;
  }

  if (operation == "decrement-wrap") {
    return wgpu::StencilOperation::DecrementWrap;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown stencil operation: ", operation);
}

wgpu::VertexStepMode parseVertexStepMode(kj::StringPtr stepMode) {
  if (stepMode == "vertex") {
    return wgpu::VertexStepMode::Vertex;
  }

  if (stepMode == "instance") {
    return wgpu::VertexStepMode::Instance;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown vertex step mode: ", stepMode);
}

wgpu::VertexFormat parseVertexFormat(kj::StringPtr format) {
  if (format == "uint8x2") {
    return wgpu::VertexFormat::Uint8x2;
  }
  if (format == "uint8x4") {
    return wgpu::VertexFormat::Uint8x4;
  }
  if (format == "sint8x2") {
    return wgpu::VertexFormat::Sint8x2;
  }
  if (format == "sint8x4") {
    return wgpu::VertexFormat::Sint8x2;
  }
  if (format == "unorm8x2") {
    return wgpu::VertexFormat::Unorm8x2;
  }
  if (format == "unorm8x4") {
    return wgpu::VertexFormat::Unorm8x4;
  }
  if (format == "snorm8x2") {
    return wgpu::VertexFormat::Snorm8x2;
  }
  if (format == "snorm8x4") {
    return wgpu::VertexFormat::Snorm8x4;
  }
  if (format == "uint16x2") {
    return wgpu::VertexFormat::Uint16x2;
  }
  if (format == "uint16x4") {
    return wgpu::VertexFormat::Uint16x4;
  }
  if (format == "sint16x2") {
    return wgpu::VertexFormat::Sint16x2;
  }
  if (format == "sint16x4") {
    return wgpu::VertexFormat::Sint16x4;
  }
  if (format == "unorm16x2") {
    return wgpu::VertexFormat::Unorm16x2;
  }
  if (format == "unorm16x4") {
    return wgpu::VertexFormat::Unorm16x4;
  }
  if (format == "snorm16x2") {
    return wgpu::VertexFormat::Snorm16x2;
  }
  if (format == "snorm16x4") {
    return wgpu::VertexFormat::Snorm16x4;
  }
  if (format == "float16x2") {
    return wgpu::VertexFormat::Float16x2;
  }
  if (format == "float16x4") {
    return wgpu::VertexFormat::Float16x4;
  }
  if (format == "float32") {
    return wgpu::VertexFormat::Float32;
  }
  if (format == "float32x2") {
    return wgpu::VertexFormat::Float32x2;
  }
  if (format == "float32x3") {
    return wgpu::VertexFormat::Float32x3;
  }
  if (format == "float32x4") {
    return wgpu::VertexFormat::Float32x4;
  }
  if (format == "uint32") {
    return wgpu::VertexFormat::Uint32;
  }
  if (format == "uint32x2") {
    return wgpu::VertexFormat::Uint32x2;
  }
  if (format == "uint32x3") {
    return wgpu::VertexFormat::Uint32x3;
  }
  if (format == "uint32x4") {
    return wgpu::VertexFormat::Uint32x4;
  }
  if (format == "sint32") {
    return wgpu::VertexFormat::Sint32;
  }
  if (format == "sint32x2") {
    return wgpu::VertexFormat::Sint32x2;
  }
  if (format == "sint32x3") {
    return wgpu::VertexFormat::Sint32x3;
  }
  if (format == "sint32x4") {
    return wgpu::VertexFormat::Sint32x4;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown vertex format: ", format);
}

wgpu::BlendFactor parseBlendFactor(kj::StringPtr factor) {

  if (factor == "zero") {
    return wgpu::BlendFactor::Zero;
  }

  if (factor == "one") {
    return wgpu::BlendFactor::One;
  }

  if (factor == "src") {
    return wgpu::BlendFactor::Src;
  }

  if (factor == "one-minus-src") {
    return wgpu::BlendFactor::OneMinusSrc;
  }

  if (factor == "src-alpha") {
    return wgpu::BlendFactor::SrcAlpha;
  }

  if (factor == "one-minus-src-alpha") {
    return wgpu::BlendFactor::OneMinusSrcAlpha;
  }

  if (factor == "dst") {
    return wgpu::BlendFactor::Dst;
  }

  if (factor == "one-minus-dst") {
    return wgpu::BlendFactor::OneMinusDst;
  }

  if (factor == "dst-alpha") {
    return wgpu::BlendFactor::DstAlpha;
  }

  if (factor == "one-minus-dst-alpha") {
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }

  if (factor == "src-alpha-saturated") {
    return wgpu::BlendFactor::SrcAlphaSaturated;
  }

  if (factor == "constant") {
    return wgpu::BlendFactor::Constant;
  }

  if (factor == "one-minus-constant") {
    return wgpu::BlendFactor::OneMinusConstant;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown blend factor: ", factor);
}

wgpu::BlendOperation parseBlendOperation(kj::StringPtr operation) {
  if (operation == "add") {
    return wgpu::BlendOperation::Add;
  }
  if (operation == "subtract") {
    return wgpu::BlendOperation::Subtract;
  }
  if (operation == "reverse-subtract") {
    return wgpu::BlendOperation::ReverseSubtract;
  }
  if (operation == "min") {
    return wgpu::BlendOperation::Min;
  }
  if (operation == "max") {
    return wgpu::BlendOperation::Max;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown blend operation: ", operation);
}

}  // namespace workerd::api::gpu
