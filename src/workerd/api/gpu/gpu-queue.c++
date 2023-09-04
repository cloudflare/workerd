// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-queue.h"
#include "workerd/jsg/exception.h"

namespace workerd::api::gpu {

void GPUQueue::submit(kj::Array<jsg::Ref<GPUCommandBuffer>> commandBuffers) {
  kj::Vector<wgpu::CommandBuffer> bufs(commandBuffers.size());
  for (auto& cb : commandBuffers) {
    bufs.add(*cb);
  }

  queue_.Submit(bufs.size(), bufs.begin());
}

void GPUQueue::writeBuffer(jsg::Ref<GPUBuffer> buffer, GPUSize64 bufferOffset,
                           jsg::BufferSource data, jsg::Optional<GPUSize64> dataOffsetElements,
                           jsg::Optional<GPUSize64> sizeElements) {
  wgpu::Buffer buf = *buffer;

  uint64_t dataOffset = 0;
  KJ_IF_MAYBE (offset, dataOffsetElements) {
    // In the JS semantics of WebGPU, writeBuffer works in number of
    // elements of the typed arrays.
    dataOffset = *offset * data.getElementSize();
    JSG_REQUIRE(dataOffset <= data.size(), TypeError, "dataOffset is larger than data's size.");
  }

  auto dataPtr = reinterpret_cast<uint8_t*>(data.asArrayPtr().begin()) + dataOffset;
  size_t dataSize = data.size() - dataOffset;
  KJ_IF_MAYBE (size, sizeElements) {
    JSG_REQUIRE(*size <= std::numeric_limits<uint64_t>::max() / data.getElementSize(), TypeError,
                "size overflows.");
    dataSize = *size * data.getElementSize();
    JSG_REQUIRE(dataOffset + dataSize <= data.size(), TypeError,
                "size + dataOffset is larger than data's size.");

    JSG_REQUIRE(dataSize % 4 == 0, TypeError, "size is not a multiple of 4 bytes.");
  }

  KJ_ASSERT(dataSize <= std::numeric_limits<size_t>::max());
  queue_.WriteBuffer(buf, bufferOffset, dataPtr, static_cast<size_t>(dataSize));
}

} // namespace workerd::api::gpu
