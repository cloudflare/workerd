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
using GPUBufferUsageFlags = uint32_t;
using GPUFlagsConstant = uint32_t;
using GPUShaderStageFlags = uint32_t;
using GPUIndex32 = uint32_t;
using GPUAddressMode = kj::String;
using GPUFilterMode = kj::String;
using GPUMipmapFilterMode = kj::String;
using GPUCompareFunction = kj::String;

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

wgpu::FeatureName parseFeatureName(GPUFeatureName &);

} // namespace workerd::api::gpu
