// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUBuffer : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::Buffer &() const { return buffer_; }
  explicit GPUBuffer(wgpu::Buffer, wgpu::BufferDescriptor);

  JSG_RESOURCE_TYPE(GPUBuffer) {
    JSG_METHOD(getMappedRange);
    JSG_METHOD(unmap);
  }

private:
  // https://www.w3.org/TR/webgpu/#buffer-interface
  enum class State {
    Unmapped,
    Mapped,
    MappedAtCreation,
    MappingPending,
    Destroyed,
  };

  struct Mapping {
    uint64_t start;
    uint64_t end;
    inline bool Intersects(uint64_t s, uint64_t e) const {
      return s < end && e > start;
    }
    kj::ArrayPtr<kj::byte> buffer;
  };

  wgpu::Buffer buffer_;
  wgpu::BufferDescriptor desc_;
  State state_ = State::Unmapped;
  kj::Vector<Mapping> mapped_;

  kj::ArrayPtr<kj::byte> getMappedRange(jsg::Optional<GPUSize64> offset,
                                        jsg::Optional<GPUSize64> size);
  void unmap();
};

struct GPUBufferDescriptor {
  kj::String label;
  GPUSize64 size;
  GPUBufferUsageFlags usage;
  bool mappedAtCreation = false;
  JSG_STRUCT(label, size, usage, mappedAtCreation);
};

} // namespace workerd::api::gpu
