// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-texture-view.h"
#include "gpu-utils.h"

#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUTexture: public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::Texture&() const {
    return texture_;
  }
  explicit GPUTexture(wgpu::Texture t): texture_(kj::mv(t)) {};
  JSG_RESOURCE_TYPE(GPUTexture) {
    JSG_METHOD(createView);
    JSG_METHOD(destroy);
    JSG_READONLY_PROTOTYPE_PROPERTY(width, getWidth);
    JSG_READONLY_PROTOTYPE_PROPERTY(height, getHeight);
    JSG_READONLY_PROTOTYPE_PROPERTY(depthOrArrayLayers, getDepthOrArrayLayers);
    JSG_READONLY_PROTOTYPE_PROPERTY(mipLevelCount, getMipLevelCount);
    JSG_READONLY_PROTOTYPE_PROPERTY(dimension, getDimension);
    JSG_READONLY_PROTOTYPE_PROPERTY(format, getFormat);
    JSG_READONLY_PROTOTYPE_PROPERTY(usage, getUsage);
  }

private:
  wgpu::Texture texture_;

  jsg::Ref<GPUTextureView> createView(jsg::Optional<GPUTextureViewDescriptor> descriptor);

  GPUIntegerCoordinateOut getWidth() {
    return texture_.GetWidth();
  }

  GPUIntegerCoordinateOut getHeight() {
    return texture_.GetHeight();
  }

  GPUIntegerCoordinateOut getDepthOrArrayLayers() {
    return texture_.GetDepthOrArrayLayers();
  }

  GPUIntegerCoordinateOut getMipLevelCount() {
    return texture_.GetMipLevelCount();
  }

  GPUSize32Out getSampleCount() {
    return texture_.GetSampleCount();
  }

  GPUTextureDimension getDimension() {
    auto dim = texture_.GetDimension();
    return getTextureDimension(dim);
  }

  GPUTextureFormat getFormat() {
    auto format = texture_.GetFormat();
    return getTextureFormat(format);
  }

  GPUFlagsConstant getUsage() {
    return GPUFlagsConstant(texture_.GetUsage());
  }

  void destroy() {
    texture_.Destroy();
  }
};

struct GPUExtent3DDict {
  GPUIntegerCoordinate width;
  jsg::Optional<GPUIntegerCoordinate> height;
  jsg::Optional<GPUIntegerCoordinate> depthOrArrayLayers;
  JSG_STRUCT(width, height, depthOrArrayLayers);
};

using GPUExtent3D = kj::OneOf<jsg::Sequence<GPUIntegerCoordinate>, GPUExtent3DDict>;

struct GPUTextureDescriptor {
  kj::String label;
  GPUExtent3D size;
  jsg::Optional<GPUIntegerCoordinate> mipLevelCount;
  jsg::Optional<GPUSize32> sampleCount;
  jsg::Optional<GPUTextureDimension> dimension;
  GPUTextureFormat format;
  GPUTextureUsageFlags usage;
  jsg::Optional<jsg::Sequence<GPUTextureFormat>> viewFormats;
  JSG_STRUCT(label, size, mipLevelCount, sampleCount, dimension, format, usage, viewFormats);
};

}  // namespace workerd::api::gpu
