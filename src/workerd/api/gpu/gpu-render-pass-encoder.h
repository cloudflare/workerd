// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-query-set.h"
#include "gpu-render-pipeline.h"
#include "gpu-texture-view.h"
#include "gpu-utils.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPURenderPassEncoder: public jsg::Object {
public:
  explicit GPURenderPassEncoder(wgpu::RenderPassEncoder e): encoder_(kj::mv(e)) {};
  JSG_RESOURCE_TYPE(GPURenderPassEncoder) {
    JSG_METHOD(setPipeline);
    JSG_METHOD(draw);
    JSG_METHOD(end);
  }

private:
  wgpu::RenderPassEncoder encoder_;
  void setPipeline(jsg::Ref<GPURenderPipeline> pipeline);
  void draw(GPUSize32 vertexCount,
      jsg::Optional<GPUSize32> instanceCount,
      jsg::Optional<GPUSize32> firstVertex,
      jsg::Optional<GPUSize32> firstInstance);
  void end() {
    encoder_.End();
  };
};

struct GPURenderPassDepthStencilAttachment {
  jsg::Ref<GPUTextureView> view;
  jsg::Optional<double> depthClearValue;
  jsg::Optional<GPULoadOp> depthLoadOp;
  jsg::Optional<GPUStoreOp> depthStoreOp;
  jsg::Optional<bool> depthReadOnly;
  jsg::Optional<GPUStencilValue> stencilClearValue;
  jsg::Optional<GPULoadOp> stencilLoadOp;
  jsg::Optional<GPUStoreOp> stencilStoreOp;
  jsg::Optional<bool> stencilReadOnly;
  JSG_STRUCT(view,
      depthClearValue,
      depthLoadOp,
      depthStoreOp,
      depthReadOnly,
      stencilClearValue,
      stencilLoadOp,
      stencilStoreOp,
      stencilReadOnly);
};

struct GPUColorDict {
  double r, g, b, a;
  JSG_STRUCT(r, g, b, a);
};

using GPUColor = kj::OneOf<jsg::Sequence<double>, GPUColorDict>;

struct GPURenderPassColorAttachment {
  jsg::Ref<GPUTextureView> view;
  jsg::Optional<GPUIntegerCoordinate> depthSlice;
  jsg::Optional<jsg::Ref<GPUTextureView>> resolveTarget;
  jsg::Optional<GPUColor> clearValue;
  GPULoadOp loadOp;
  GPUStoreOp storeOp;
  JSG_STRUCT(view, depthSlice, resolveTarget, clearValue, loadOp, storeOp);
};

struct GPURenderPassTimestampWrites {
  jsg::Ref<GPUQuerySet> querySet;
  jsg::Optional<GPUSize32> beginningOfPassWriteIndex;
  jsg::Optional<GPUSize32> endOfPassWriteIndex;

  JSG_STRUCT(querySet, beginningOfPassWriteIndex, endOfPassWriteIndex);
};

struct GPURenderPassDescriptor {
  jsg::Optional<kj::String> label;
  jsg::Sequence<GPURenderPassColorAttachment> colorAttachments;
  jsg::Optional<GPURenderPassDepthStencilAttachment> depthStencilAttachment;
  jsg::Optional<jsg::Ref<GPUQuerySet>> occlusionQuerySet;
  jsg::Optional<GPURenderPassTimestampWrites> timestampWrites;
  jsg::Optional<GPUSize64> maxDrawCount;
  JSG_STRUCT(label,
      colorAttachments,
      depthStencilAttachment,
      occlusionQuerySet,
      timestampWrites,
      maxDrawCount);
};

}  // namespace workerd::api::gpu
