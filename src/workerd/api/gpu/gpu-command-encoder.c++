// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-command-encoder.h"
#include "gpu-command-buffer.h"
#include "gpu-utils.h"

namespace workerd::api::gpu {

jsg::Ref<GPUCommandBuffer>
GPUCommandEncoder::finish(jsg::Optional<GPUCommandBufferDescriptor> descriptor) {
  wgpu::CommandBufferDescriptor desc{};

  KJ_IF_SOME(d, descriptor) {
    KJ_IF_SOME(label, d.label) {
      desc.label = label.cStr();
    }
  }

  auto buffer = encoder_.Finish(&desc);
  return jsg::alloc<GPUCommandBuffer>(kj::mv(buffer));
};

void GPUCommandEncoder::copyBufferToBuffer(jsg::Ref<GPUBuffer> source, GPUSize64 sourceOffset,
                                           jsg::Ref<GPUBuffer> destination,
                                           GPUSize64 destinationOffset, GPUSize64 size) {

  encoder_.CopyBufferToBuffer(*source, sourceOffset, *destination, destinationOffset, size);
};

jsg::Ref<GPURenderPassEncoder>
GPUCommandEncoder::beginRenderPass(GPURenderPassDescriptor descriptor) {

  wgpu::RenderPassDescriptor desc{};

  KJ_IF_SOME(label, descriptor.label) {
    desc.label = label.cStr();
  }

  kj::Vector<wgpu::RenderPassColorAttachment> colorAttachments;
  for (auto& attach : descriptor.colorAttachments) {
    wgpu::RenderPassColorAttachment cAttach{};
    cAttach.view = *attach.view;
    // TODO: depthSlice is not yet supported by dawn
    KJ_IF_SOME(resolveTarget, attach.resolveTarget) {
      cAttach.resolveTarget = *resolveTarget;
    }
    KJ_IF_SOME(clearValue, attach.clearValue) {
      KJ_SWITCH_ONEOF(clearValue) {
        KJ_CASE_ONEOF(colors, jsg::Sequence<double>) {
          JSG_REQUIRE(colors.size() == 4, Error, "Wrong number of elements in clearValue");
          cAttach.clearValue.r = colors[0];
          cAttach.clearValue.g = colors[1];
          cAttach.clearValue.b = colors[2];
          cAttach.clearValue.a = colors[3];
        }
        KJ_CASE_ONEOF(gpuColorDict, GPUColorDict) {
          cAttach.clearValue.r = gpuColorDict.r;
          cAttach.clearValue.g = gpuColorDict.g;
          cAttach.clearValue.b = gpuColorDict.b;
          cAttach.clearValue.a = gpuColorDict.a;
        }
      }
    }

    cAttach.loadOp = parseGPULoadOp(attach.loadOp);
    cAttach.storeOp = parseGPUStoreOp(attach.storeOp);
    colorAttachments.add(kj::mv(cAttach));
  }
  desc.colorAttachments = colorAttachments.begin();
  desc.colorAttachmentCount = colorAttachments.size();

  KJ_IF_SOME(depthStencilAttachment, descriptor.depthStencilAttachment) {
    wgpu::RenderPassDepthStencilAttachment dAttach{};
    dAttach.view = *depthStencilAttachment.view;
    KJ_IF_SOME(depthClearValue, depthStencilAttachment.depthClearValue) {
      dAttach.depthClearValue = depthClearValue;
    }
    KJ_IF_SOME(depthLoadOp, depthStencilAttachment.depthLoadOp) {
      dAttach.depthLoadOp = parseGPULoadOp(depthLoadOp);
    }
    KJ_IF_SOME(depthStoreOp, depthStencilAttachment.depthStoreOp) {
      dAttach.depthStoreOp = parseGPUStoreOp(depthStoreOp);
    }
    KJ_IF_SOME(depthReadOnly, depthStencilAttachment.depthReadOnly) {
      dAttach.depthReadOnly = depthReadOnly;
    }
    KJ_IF_SOME(stencilClearValue, depthStencilAttachment.stencilClearValue) {
      dAttach.stencilClearValue = stencilClearValue;
    }
    KJ_IF_SOME(stencilLoadOp, depthStencilAttachment.stencilLoadOp) {
      dAttach.stencilLoadOp = parseGPULoadOp(stencilLoadOp);
    }
    KJ_IF_SOME(stencilStoreOp, depthStencilAttachment.stencilStoreOp) {
      dAttach.stencilStoreOp = parseGPUStoreOp(stencilStoreOp);
    }
    KJ_IF_SOME(stencilReadOnly, depthStencilAttachment.stencilReadOnly) {
      dAttach.stencilReadOnly = stencilReadOnly;
    }

    desc.depthStencilAttachment = &dAttach;
  }

  KJ_IF_SOME(occlusionQuerySet, descriptor.occlusionQuerySet) {
    desc.occlusionQuerySet = *occlusionQuerySet;
  }

  kj::Vector<wgpu::RenderPassTimestampWrite> timestamps;
  KJ_IF_SOME(timestampWrites, descriptor.timestampWrites) {
    for (auto& timestamp : timestampWrites) {
      wgpu::RenderPassTimestampWrite t{};
      t.querySet = *timestamp.querySet;
      t.queryIndex = timestamp.queryIndex;
      t.location = parseRenderPassTimestampLocation(timestamp.location);
      timestamps.add(kj::mv(t));
    }
  }
  desc.timestampWrites = timestamps.begin();
  desc.timestampWriteCount = timestamps.size();

  // TODO: maxDrawCount is not supported by dawn yet

  auto renderPassEncoder = encoder_.BeginRenderPass(&desc);
  return jsg::alloc<GPURenderPassEncoder>(kj::mv(renderPassEncoder));
}

jsg::Ref<GPUComputePassEncoder>
GPUCommandEncoder::beginComputePass(jsg::Optional<GPUComputePassDescriptor> descriptor) {

  wgpu::ComputePassDescriptor desc{};

  kj::Vector<wgpu::ComputePassTimestampWrite> timestamps;
  KJ_IF_SOME(d, descriptor) {
    KJ_IF_SOME(label, d.label) {
      desc.label = label.cStr();
    }

    KJ_IF_SOME(timestampWrites, d.timestampWrites) {
      for (auto& timestamp : timestampWrites) {
        wgpu::ComputePassTimestampWrite t{};
        t.querySet = *timestamp.querySet;
        t.queryIndex = timestamp.queryIndex;
        t.location = parseComputePassTimestampLocation(timestamp.location);
        timestamps.add(kj::mv(t));
      }
    }
  }
  desc.timestampWrites = timestamps.begin();
  desc.timestampWriteCount = timestamps.size();

  auto computePassEncoder = encoder_.BeginComputePass(&desc);
  return jsg::alloc<GPUComputePassEncoder>(kj::mv(computePassEncoder));
}

} // namespace workerd::api::gpu
