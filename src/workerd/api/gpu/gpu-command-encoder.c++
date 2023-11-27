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

wgpu::ImageCopyTexture parseGPUImageCopyTexture(GPUImageCopyTexture source) {
  wgpu::ImageCopyTexture src{};
  src.texture = *source.texture;

  KJ_IF_SOME(mipLevel, source.mipLevel) {
    src.mipLevel = mipLevel;
  }
  KJ_IF_SOME(origin, source.origin) {
    KJ_SWITCH_ONEOF(origin) {
      KJ_CASE_ONEOF(coords, jsg::Sequence<GPUIntegerCoordinate>) {
        JSG_REQUIRE(coords.size() == 3, Error, "Wrong number of elements in origin");
        src.origin.x = coords[0];
        src.origin.y = coords[1];
        src.origin.z = coords[2];
      }
      KJ_CASE_ONEOF(gpuOriginDict, GPUOrigin3DDict) {
        KJ_IF_SOME(x, gpuOriginDict.x) {
          src.origin.x = x;
        }
        KJ_IF_SOME(y, gpuOriginDict.y) {
          src.origin.y = y;
        }
        KJ_IF_SOME(z, gpuOriginDict.z) {
          src.origin.z = z;
        }
      }
    }
  }
  KJ_IF_SOME(aspect, source.aspect) {
    src.aspect = parseTextureAspect(aspect);
  }

  return kj::mv(src);
}

wgpu::ImageCopyBuffer parseGPUImageCopyBuffer(GPUImageCopyBuffer destination) {
  wgpu::ImageCopyBuffer dst{};
  dst.buffer = *destination.buffer;

  KJ_IF_SOME(offset, destination.offset) {
    dst.layout.offset = offset;
  }
  KJ_IF_SOME(bytesPerRow, destination.bytesPerRow) {
    dst.layout.bytesPerRow = bytesPerRow;
  }
  KJ_IF_SOME(rowsPerImage, destination.rowsPerImage) {
    dst.layout.rowsPerImage = rowsPerImage;
  }

  return kj::mv(dst);
}

wgpu::Extent3D parseGPUExtent3D(GPUExtent3D copySize) {
  wgpu::Extent3D size{};
  KJ_SWITCH_ONEOF(copySize) {
    KJ_CASE_ONEOF(coords, jsg::Sequence<GPUIntegerCoordinate>) {
      // if we have a sequence of coordinates we assume that the order is: width, heigth, depth, if
      // available, and ignore all the rest.
      switch (coords.size()) {
      default:
      case 3:
        size.depthOrArrayLayers = coords[2];
        KJ_FALLTHROUGH;
      case 2:
        size.height = coords[1];
        KJ_FALLTHROUGH;
      case 1:
        size.width = coords[0];
        break;
      case 0:
        JSG_FAIL_REQUIRE(TypeError, "invalid value for GPUExtent3D");
      }
    }
    KJ_CASE_ONEOF(someSize, GPUExtent3DDict) {
      KJ_IF_SOME(depthOrArrayLayers, someSize.depthOrArrayLayers) {
        size.depthOrArrayLayers = depthOrArrayLayers;
      }
      KJ_IF_SOME(height, someSize.height) {
        size.height = height;
      }
      size.width = someSize.width;
    }
  }

  return kj::mv(size);
}

void GPUCommandEncoder::copyTextureToTexture(GPUImageCopyTexture source,
                                             GPUImageCopyTexture destination,
                                             GPUExtent3D copySize) {
  wgpu::ImageCopyTexture src = parseGPUImageCopyTexture(kj::mv(source));
  wgpu::ImageCopyTexture dst = parseGPUImageCopyTexture(kj::mv(destination));
  wgpu::Extent3D size = parseGPUExtent3D(kj::mv(copySize));

  encoder_.CopyTextureToTexture(&src, &dst, &size);
}

void GPUCommandEncoder::copyBufferToTexture(GPUImageCopyBuffer source,
                                            GPUImageCopyTexture destination, GPUExtent3D copySize) {

  wgpu::ImageCopyBuffer src = parseGPUImageCopyBuffer(kj::mv(source));
  wgpu::ImageCopyTexture dst = parseGPUImageCopyTexture(kj::mv(destination));
  wgpu::Extent3D size = parseGPUExtent3D(kj::mv(copySize));

  encoder_.CopyBufferToTexture(&src, &dst, &size);
}

void GPUCommandEncoder::copyTextureToBuffer(GPUImageCopyTexture source,
                                            GPUImageCopyBuffer destination, GPUExtent3D copySize) {
  wgpu::ImageCopyTexture src = parseGPUImageCopyTexture(kj::mv(source));
  wgpu::ImageCopyBuffer dst = parseGPUImageCopyBuffer(kj::mv(destination));
  wgpu::Extent3D size = parseGPUExtent3D(kj::mv(copySize));

  encoder_.CopyTextureToBuffer(&src, &dst, &size);
}

void GPUCommandEncoder::copyBufferToBuffer(jsg::Ref<GPUBuffer> source, GPUSize64 sourceOffset,
                                           jsg::Ref<GPUBuffer> destination,
                                           GPUSize64 destinationOffset, GPUSize64 size) {

  encoder_.CopyBufferToBuffer(*source, sourceOffset, *destination, destinationOffset, size);
};

void GPUCommandEncoder::clearBuffer(jsg::Ref<GPUBuffer> buffer, jsg::Optional<GPUSize64> offset,
                                    jsg::Optional<GPUSize64> size) {
  uint64_t o = offset.orDefault(0);
  uint64_t s = size.orDefault(wgpu::kWholeSize);

  encoder_.ClearBuffer(*buffer, o, s);
}

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
