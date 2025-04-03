// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-buffer.h"
#include "gpu-command-buffer.h"
#include "gpu-compute-pass-encoder.h"
#include "gpu-render-pass-encoder.h"
#include "gpu-texture.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

struct GPUOrigin3DDict {
  jsg::Optional<GPUIntegerCoordinate> x, y, z;
  JSG_STRUCT(x, y, z);
};

using GPUOrigin3D = kj::OneOf<jsg::Sequence<GPUIntegerCoordinate>, GPUOrigin3DDict>;

struct GPUImageCopyTexture {
  jsg::Ref<GPUTexture> texture;
  jsg::Optional<GPUIntegerCoordinate> mipLevel;
  jsg::Optional<GPUOrigin3D> origin;
  jsg::Optional<GPUTextureAspect> aspect;
  JSG_STRUCT(texture, mipLevel, origin, aspect);
};

struct GPUImageCopyBuffer {
  jsg::Ref<GPUBuffer> buffer;
  jsg::Optional<GPUSize64> offset;
  jsg::Optional<GPUSize32> bytesPerRow;
  jsg::Optional<GPUSize32> rowsPerImage;
  JSG_STRUCT(buffer, offset, bytesPerRow, rowsPerImage);
};

class GPUCommandEncoder: public jsg::Object {
 public:
  explicit GPUCommandEncoder(wgpu::CommandEncoder e, kj::String label)
      : encoder_(kj::mv(e)),
        label_(kj::mv(label)) {};
  JSG_RESOURCE_TYPE(GPUCommandEncoder) {
    JSG_READONLY_PROTOTYPE_PROPERTY(label, getLabel);
    JSG_METHOD(beginComputePass);
    JSG_METHOD(beginRenderPass);
    JSG_METHOD(copyBufferToBuffer);
    JSG_METHOD(finish);
    JSG_METHOD(copyTextureToBuffer);
    JSG_METHOD(copyBufferToTexture);
    JSG_METHOD(copyTextureToTexture);
    JSG_METHOD(clearBuffer);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("label", label_);
  }

 private:
  wgpu::CommandEncoder encoder_;
  kj::String label_;
  kj::StringPtr getLabel() {
    return label_;
  }

  jsg::Ref<GPUComputePassEncoder> beginComputePass(
      jsg::Lock& js, jsg::Optional<GPUComputePassDescriptor> descriptor);
  jsg::Ref<GPURenderPassEncoder> beginRenderPass(jsg::Lock& js, GPURenderPassDescriptor descriptor);
  jsg::Ref<GPUCommandBuffer> finish(jsg::Lock& js, jsg::Optional<GPUCommandBufferDescriptor>);
  void copyBufferToBuffer(jsg::Ref<GPUBuffer> source,
      GPUSize64 sourceOffset,
      jsg::Ref<GPUBuffer> destination,
      GPUSize64 destinationOffset,
      GPUSize64 size);
  void copyTextureToBuffer(
      GPUImageCopyTexture source, GPUImageCopyBuffer destination, GPUExtent3D copySize);
  void copyBufferToTexture(
      GPUImageCopyBuffer source, GPUImageCopyTexture destination, GPUExtent3D copySize);
  void copyTextureToTexture(
      GPUImageCopyTexture source, GPUImageCopyTexture destination, GPUExtent3D copySize);
  void clearBuffer(
      jsg::Ref<GPUBuffer> buffer, jsg::Optional<GPUSize64> offset, jsg::Optional<GPUSize64> size);
};

struct GPUCommandEncoderDescriptor {
  jsg::Optional<kj::String> label;

  JSG_STRUCT(label);
};

}  // namespace workerd::api::gpu
