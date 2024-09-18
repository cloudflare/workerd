// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-pipeline-layout.h"
#include "gpu-shader-module.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPURenderPipeline: public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::RenderPipeline&() const {
    return pipeline_;
  }
  explicit GPURenderPipeline(wgpu::RenderPipeline p): pipeline_(kj::mv(p)) {};
  JSG_RESOURCE_TYPE(GPURenderPipeline) {}

private:
  wgpu::RenderPipeline pipeline_;
};

struct GPUVertexAttribute {
  GPUVertexFormat format;
  GPUSize64 offset;
  GPUIndex32 shaderLocation;

  JSG_STRUCT(format, offset, shaderLocation);
};

struct GPUVertexBufferLayout {
  GPUSize64 arrayStride;
  jsg::Optional<GPUVertexStepMode> stepMode;
  jsg::Sequence<GPUVertexAttribute> attributes;

  JSG_STRUCT(arrayStride, stepMode, attributes);
};

struct GPUVertexState {
  jsg::Ref<GPUShaderModule> module;
  kj::String entryPoint;
  jsg::Optional<jsg::Dict<GPUPipelineConstantValue>> constants;
  jsg::Optional<jsg::Sequence<GPUVertexBufferLayout>> buffers;

  JSG_STRUCT(module, entryPoint, constants, buffers);
};

struct GPUBlendComponent {
  jsg::Optional<GPUBlendOperation> operation;
  jsg::Optional<GPUBlendFactor> srcFactor;
  jsg::Optional<GPUBlendFactor> dstFactor;

  JSG_STRUCT(operation, srcFactor, dstFactor);
};

struct GPUBlendState {
  GPUBlendComponent color;
  GPUBlendComponent alpha;

  JSG_STRUCT(color, alpha);
};

struct GPUColorTargetState {
  GPUTextureFormat format;
  GPUBlendState blend;
  jsg::Optional<GPUFlagsConstant> writeMask;

  JSG_STRUCT(format, blend, writeMask);
};

struct GPUFragmentState {
  jsg::Ref<GPUShaderModule> module;
  kj::String entryPoint;
  jsg::Optional<jsg::Dict<GPUPipelineConstantValue>> constants;
  jsg::Sequence<GPUColorTargetState> targets;

  JSG_STRUCT(module, entryPoint, constants, targets);
};

struct GPUPrimitiveState {
  jsg::Optional<GPUPrimitiveTopology> topology;
  jsg::Optional<GPUIndexFormat> stripIndexFormat;
  jsg::Optional<GPUFrontFace> frontFace;
  jsg::Optional<GPUCullMode> cullMode;
  jsg::Optional<bool> unclippedDepth;

  JSG_STRUCT(topology, stripIndexFormat, frontFace, cullMode, unclippedDepth);
};

struct GPUStencilFaceState {
  jsg::Optional<GPUCompareFunction> compare;
  jsg::Optional<GPUStencilOperation> failOp;
  jsg::Optional<GPUStencilOperation> depthFailOp;
  jsg::Optional<GPUStencilOperation> passOp;

  JSG_STRUCT(compare, failOp, depthFailOp, passOp);
};

struct GPUDepthStencilState {
  GPUTextureFormat format;
  bool depthWriteEnabled;
  GPUCompareFunction depthCompare;
  jsg::Optional<GPUStencilFaceState> stencilFront;
  jsg::Optional<GPUStencilFaceState> stencilBack;
  jsg::Optional<GPUStencilValue> stencilReadMask;
  jsg::Optional<GPUStencilValue> stencilWriteMask;
  jsg::Optional<GPUDepthBias> depthBias;
  jsg::Optional<double> depthBiasSlopeScale;
  jsg::Optional<double> depthBiasClamp;

  JSG_STRUCT(format,
      depthWriteEnabled,
      depthCompare,
      stencilFront,
      stencilBack,
      stencilReadMask,
      stencilWriteMask,
      depthBias,
      depthBiasSlopeScale,
      depthBiasClamp);
};

struct GPUMultisampleState {
  jsg::Optional<GPUSize32> count;
  jsg::Optional<GPUSampleMask> mask;
  jsg::Optional<bool> alphaToCoverageEnabled;

  JSG_STRUCT(count, mask, alphaToCoverageEnabled);
};

struct GPURenderPipelineDescriptor {
  jsg::Optional<kj::String> label;
  GPUPipelineLayoutBase layout;
  GPUVertexState vertex;
  jsg::Optional<GPUPrimitiveState> primitive;
  jsg::Optional<GPUDepthStencilState> depthStencil;
  jsg::Optional<GPUMultisampleState> multisample;
  jsg::Optional<GPUFragmentState> fragment;

  JSG_STRUCT(label, layout, vertex, primitive, depthStencil, multisample, fragment);
};

}  // namespace workerd::api::gpu
