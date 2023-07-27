// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-buffer.h"
#include <_types/_uint64_t.h>

namespace workerd::api::gpu {

GPUBuffer::GPUBuffer(wgpu::Buffer b, wgpu::BufferDescriptor desc)
    : buffer_(kj::mv(b)), desc_(kj::mv(desc)) {

  if (desc.mappedAtCreation) {
    state_ = State::MappedAtCreation;
  }
};

kj::ArrayPtr<kj::byte>
GPUBuffer::getMappedRange(jsg::Optional<GPUSize64> offset,
                          jsg::Optional<GPUSize64> size) {

  JSG_REQUIRE(state_ == State::Mapped || state_ == State::MappedAtCreation,
              TypeError, "trying to get mapped range of unmapped buffer");

  uint64_t o = 0;
  KJ_IF_MAYBE (real_offset, offset) {
    o = *real_offset;
  }

  uint64_t s = desc_.size - o;
  KJ_IF_MAYBE (real_size, size) {
    s = *real_size;
  }

  uint64_t start = o;
  uint64_t end = o + s;
  for (auto &mapping : mapped_) {
    if (mapping.Intersects(start, end)) {
      KJ_FAIL_REQUIRE("mapping intersects with existing one");
    }
  }

  auto *ptr = (desc_.usage & wgpu::BufferUsage::MapWrite)
                  ? buffer_.GetMappedRange(o, s)
                  : const_cast<void *>(buffer_.GetConstMappedRange(o, s));

  JSG_REQUIRE(ptr, TypeError, "could not obtain mapped range");

  auto arrayBuffer = kj::arrayPtr((byte *)ptr, s);
  mapped_.add(Mapping{start, end, arrayBuffer});
  return arrayBuffer;
}

void GPUBuffer::unmap() {
  buffer_.Unmap();

  if (state_ != State::Destroyed && state_ != State::Unmapped) {
    mapped_.clear();
    state_ = State::Unmapped;
  }
}

} // namespace workerd::api::gpu
