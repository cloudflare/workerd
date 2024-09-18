// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"

#include <workerd/jsg/jsg.h>

#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

class GPUSupportedLimits: public jsg::Object {
public:
  explicit GPUSupportedLimits(wgpu::SupportedLimits limits): limits_(kj::mv(limits)) {};
  JSG_RESOURCE_TYPE(GPUSupportedLimits) {
    JSG_READONLY_PROTOTYPE_PROPERTY(maxTextureDimension1D, getMaxTextureDimension1D);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxTextureDimension2D, getMaxTextureDimension2D);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxTextureDimension3D, getMaxTextureDimension3D);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxTextureArrayLayers, getMaxTextureArrayLayers);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxBindGroups, getMaxBindGroups);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxBindingsPerBindGroup, getMaxBindingsPerBindGroup);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxDynamicUniformBuffersPerPipelineLayout, getMaxDynamicUniformBuffersPerPipelineLayout);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxDynamicStorageBuffersPerPipelineLayout, getMaxDynamicStorageBuffersPerPipelineLayout);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxSampledTexturesPerShaderStage, getMaxSampledTexturesPerShaderStage);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxSamplersPerShaderStage, getMaxSamplersPerShaderStage);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxStorageBuffersPerShaderStage, getMaxStorageBuffersPerShaderStage);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxStorageTexturesPerShaderStage, getMaxStorageTexturesPerShaderStage);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxUniformBuffersPerShaderStage, getMaxUniformBuffersPerShaderStage);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxUniformBufferBindingSize, getMaxUniformBufferBindingSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxStorageBufferBindingSize, getMaxStorageBufferBindingSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        minUniformBufferOffsetAlignment, getMinUniformBufferOffsetAlignment);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        minStorageBufferOffsetAlignment, getMinStorageBufferOffsetAlignment);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxVertexBuffers, getMaxVertexBuffers);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxBufferSize, getMaxBufferSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxVertexAttributes, getMaxVertexAttributes);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxVertexBufferArrayStride, getMaxVertexBufferArrayStride);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxInterStageShaderComponents, getMaxInterStageShaderComponents);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxInterStageShaderVariables, getMaxInterStageShaderVariables);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxColorAttachments, getMaxColorAttachments);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxColorAttachmentBytesPerSample, getMaxColorAttachmentBytesPerSample);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxComputeWorkgroupStorageSize, getMaxComputeWorkgroupStorageSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxComputeInvocationsPerWorkgroup, getMaxComputeInvocationsPerWorkgroup);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxComputeWorkgroupSizeX, getMaxComputeWorkgroupSizeX);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxComputeWorkgroupSizeY, getMaxComputeWorkgroupSizeY);
    JSG_READONLY_PROTOTYPE_PROPERTY(maxComputeWorkgroupSizeZ, getMaxComputeWorkgroupSizeZ);
    JSG_READONLY_PROTOTYPE_PROPERTY(
        maxComputeWorkgroupsPerDimension, getMaxComputeWorkgroupsPerDimension);
  }

private:
  wgpu::SupportedLimits limits_;
  uint32_t getMaxTextureDimension1D() {
    return limits_.limits.maxTextureDimension1D;
  }

  uint32_t getMaxTextureDimension2D() {
    return limits_.limits.maxTextureDimension2D;
  }

  uint32_t getMaxTextureDimension3D() {
    return limits_.limits.maxTextureDimension3D;
  }

  uint32_t getMaxTextureArrayLayers() {
    return limits_.limits.maxTextureArrayLayers;
  }

  uint32_t getMaxBindGroups() {
    return limits_.limits.maxBindGroups;
  }

  uint32_t getMaxBindingsPerBindGroup() {
    return limits_.limits.maxBindingsPerBindGroup;
  }

  uint32_t getMaxDynamicUniformBuffersPerPipelineLayout() {
    return limits_.limits.maxDynamicUniformBuffersPerPipelineLayout;
  }

  uint32_t getMaxDynamicStorageBuffersPerPipelineLayout() {
    return limits_.limits.maxDynamicStorageBuffersPerPipelineLayout;
  }

  uint32_t getMaxSampledTexturesPerShaderStage() {
    return limits_.limits.maxSampledTexturesPerShaderStage;
  }

  uint32_t getMaxSamplersPerShaderStage() {
    return limits_.limits.maxSamplersPerShaderStage;
  }

  uint32_t getMaxStorageBuffersPerShaderStage() {
    return limits_.limits.maxStorageBuffersPerShaderStage;
  }

  uint32_t getMaxStorageTexturesPerShaderStage() {
    return limits_.limits.maxStorageTexturesPerShaderStage;
  }

  uint32_t getMaxUniformBuffersPerShaderStage() {
    return limits_.limits.maxUniformBuffersPerShaderStage;
  }

  uint64_t getMaxUniformBufferBindingSize() {
    return limits_.limits.maxUniformBufferBindingSize;
  }

  uint64_t getMaxStorageBufferBindingSize() {
    return limits_.limits.maxStorageBufferBindingSize;
  }

  uint32_t getMinUniformBufferOffsetAlignment() {
    return limits_.limits.minUniformBufferOffsetAlignment;
  }

  uint32_t getMinStorageBufferOffsetAlignment() {
    return limits_.limits.minStorageBufferOffsetAlignment;
  }

  uint32_t getMaxVertexBuffers() {
    return limits_.limits.maxVertexBuffers;
  }

  uint64_t getMaxBufferSize() {
    return limits_.limits.maxBufferSize;
  }

  uint32_t getMaxVertexAttributes() {
    return limits_.limits.maxVertexAttributes;
  }

  uint32_t getMaxVertexBufferArrayStride() {
    return limits_.limits.maxVertexBufferArrayStride;
  }

  uint32_t getMaxInterStageShaderComponents() {
    return limits_.limits.maxInterStageShaderComponents;
  }

  uint32_t getMaxInterStageShaderVariables() {
    return limits_.limits.maxInterStageShaderVariables;
  }

  uint32_t getMaxColorAttachments() {
    return limits_.limits.maxColorAttachments;
  }

  uint32_t getMaxColorAttachmentBytesPerSample() {
    return limits_.limits.maxColorAttachmentBytesPerSample;
  }

  uint32_t getMaxComputeWorkgroupStorageSize() {
    return limits_.limits.maxComputeWorkgroupStorageSize;
  }

  uint32_t getMaxComputeInvocationsPerWorkgroup() {
    return limits_.limits.maxComputeInvocationsPerWorkgroup;
  }

  uint32_t getMaxComputeWorkgroupSizeX() {
    return limits_.limits.maxComputeWorkgroupSizeX;
  }

  uint32_t getMaxComputeWorkgroupSizeY() {
    return limits_.limits.maxComputeWorkgroupSizeY;
  }

  uint32_t getMaxComputeWorkgroupSizeZ() {
    return limits_.limits.maxComputeWorkgroupSizeZ;
  }

  uint32_t getMaxComputeWorkgroupsPerDimension() {
    return limits_.limits.maxComputeWorkgroupsPerDimension;
  }
};

}  // namespace workerd::api::gpu
