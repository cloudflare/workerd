// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

using GPUSize64 = uint64_t;
using GPUFeatureName = kj::String;
using GPUBufferBindingType = kj::String;
using GPUSamplerBindingType = kj::String;
using GPUTextureSampleType = kj::String;
using GPUTextureViewDimension = kj::String;
using GPUStorageTextureAccess = kj::String;
using GPUTextureFormat = kj::String;
using GPUTextureDimension = kj::String;
using GPUBufferUsageFlags = uint32_t;
using GPUTextureUsageFlags = uint32_t;
using GPUFlagsConstant = uint32_t;
using GPUShaderStageFlags = uint32_t;
using GPUIndex32 = uint32_t;
using GPUStencilValue = uint32_t;
using GPUSampleMask = uint32_t;
using GPUDepthBias = int32_t;
using GPUIntegerCoordinate = uint32_t;
using GPUIntegerCoordinateOut = uint32_t;
using GPUAddressMode = kj::String;
using GPUFilterMode = kj::String;
using GPUMipmapFilterMode = kj::String;
using GPUCompareFunction = kj::String;
using GPUSize32 = uint32_t;
using GPUSize32Out = uint32_t;
using GPUBufferDynamicOffset = uint32_t;
using GPUPowerPreference = kj::String;
using GPUErrorFilter = kj::String;
using GPUDeviceLostReason = kj::String;
using GPUCompilationMessageType = kj::String;
using GPUBufferMapState = kj::String;
using GPUTextureAspect = kj::String;
using GPUPipelineConstantValue = double;
using GPUVertexStepMode = kj::String;
using GPUVertexFormat = kj::String;
using GPUPrimitiveTopology = kj::String;
using GPUFrontFace = kj::String;
using GPUCullMode = kj::String;
using GPUIndexFormat = kj::String;
using GPUStencilOperation = kj::String;
using GPUBlendOperation = kj::String;
using GPUBlendFactor = kj::String;

struct GPUMapMode : public jsg::Object {
  static constexpr GPUFlagsConstant READ = 0x0001;
  static constexpr GPUFlagsConstant WRITE = 0x0002;

  JSG_RESOURCE_TYPE(GPUMapMode) {
    JSG_STATIC_CONSTANT(READ);
    JSG_STATIC_CONSTANT(WRITE);
  }
};

struct GPUShaderStage : public jsg::Object {
  static constexpr GPUFlagsConstant VERTEX = 0x1;
  static constexpr GPUFlagsConstant FRAGMENT = 0x2;
  static constexpr GPUFlagsConstant COMPUTE = 0x4;

  JSG_RESOURCE_TYPE(GPUShaderStage) {
    JSG_STATIC_CONSTANT(VERTEX);
    JSG_STATIC_CONSTANT(FRAGMENT);
    JSG_STATIC_CONSTANT(COMPUTE);
  };
};

struct GPUBufferUsage : public jsg::Object {
  static constexpr GPUFlagsConstant MAP_READ = 0x0001;
  static constexpr GPUFlagsConstant MAP_WRITE = 0x0002;
  static constexpr GPUFlagsConstant COPY_SRC = 0x0004;
  static constexpr GPUFlagsConstant COPY_DST = 0x0008;
  static constexpr GPUFlagsConstant INDEX = 0x0010;
  static constexpr GPUFlagsConstant VERTEX = 0x0020;
  static constexpr GPUFlagsConstant UNIFORM = 0x0040;
  static constexpr GPUFlagsConstant STORAGE = 0x0080;
  static constexpr GPUFlagsConstant INDIRECT = 0x0100;
  static constexpr GPUFlagsConstant QUERY_RESOLVE = 0x0200;

  JSG_RESOURCE_TYPE(GPUBufferUsage) {
    JSG_STATIC_CONSTANT(MAP_READ);
    JSG_STATIC_CONSTANT(MAP_WRITE);
    JSG_STATIC_CONSTANT(COPY_SRC);
    JSG_STATIC_CONSTANT(COPY_DST);
    JSG_STATIC_CONSTANT(INDEX);
    JSG_STATIC_CONSTANT(VERTEX);
    JSG_STATIC_CONSTANT(UNIFORM);
    JSG_STATIC_CONSTANT(STORAGE);
    JSG_STATIC_CONSTANT(INDIRECT);
    JSG_STATIC_CONSTANT(QUERY_RESOLVE);
  };
};

struct GPUColorWrite : public jsg::Object {
  static constexpr GPUFlagsConstant RED = 0x1;
  static constexpr GPUFlagsConstant GREEN = 0x2;
  static constexpr GPUFlagsConstant BLUE = 0x4;
  static constexpr GPUFlagsConstant ALPHA = 0x8;
  static constexpr GPUFlagsConstant ALL = 0xF;

  JSG_RESOURCE_TYPE(GPUColorWrite) {
    JSG_STATIC_CONSTANT(RED);
    JSG_STATIC_CONSTANT(GREEN);
    JSG_STATIC_CONSTANT(BLUE);
    JSG_STATIC_CONSTANT(ALPHA);
    JSG_STATIC_CONSTANT(ALL);
  };
};

struct GPUTextureUsage : public jsg::Object {
  static constexpr GPUFlagsConstant COPY_SRC = 0x01;
  static constexpr GPUFlagsConstant COPY_DST = 0x02;
  static constexpr GPUFlagsConstant TEXTURE_BINDING = 0x04;
  static constexpr GPUFlagsConstant STORAGE_BINDING = 0x08;
  static constexpr GPUFlagsConstant RENDER_ATTACHMENT = 0x10;

  JSG_RESOURCE_TYPE(GPUTextureUsage) {
    JSG_STATIC_CONSTANT(COPY_SRC);
    JSG_STATIC_CONSTANT(COPY_DST);
    JSG_STATIC_CONSTANT(TEXTURE_BINDING);
    JSG_STATIC_CONSTANT(STORAGE_BINDING);
    JSG_STATIC_CONSTANT(RENDER_ATTACHMENT);
  };
};

wgpu::FeatureName parseFeatureName(GPUFeatureName&);
kj::Maybe<GPUFeatureName> getFeatureName(wgpu::FeatureName& feature);
wgpu::TextureDimension parseTextureDimension(kj::StringPtr dimension);
wgpu::TextureFormat parseTextureFormat(kj::StringPtr format);
GPUTextureDimension getTextureDimension(wgpu::TextureDimension& dimension);
GPUTextureFormat getTextureFormat(wgpu::TextureFormat& format);
wgpu::TextureViewDimension parseTextureViewDimension(kj::StringPtr dim);
wgpu::StorageTextureAccess parseStorageAccess(kj::StringPtr access);
wgpu::TextureAspect parseTextureAspect(kj::StringPtr aspect);
wgpu::PrimitiveTopology parsePrimitiveTopology(kj::StringPtr topology);
wgpu::IndexFormat parseIndexFormat(kj::StringPtr format);
wgpu::FrontFace parseFrontFace(kj::StringPtr frontFace);
wgpu::CullMode parseCullMode(kj::StringPtr mode);
wgpu::StencilOperation parseStencilOperation(kj::StringPtr operation);

} // namespace workerd::api::gpu
