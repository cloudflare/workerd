// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-async-runner.h"
#include "gpu-utils.h"
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUBuffer : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::Buffer&() const { return buffer_; }
  explicit GPUBuffer(jsg::Lock& js, wgpu::Buffer, wgpu::BufferDescriptor,
                     wgpu::Device, kj::Own<AsyncRunner>);

  JSG_RESOURCE_TYPE(GPUBuffer) {
    JSG_METHOD(getMappedRange);
    JSG_METHOD(unmap);
    JSG_METHOD(destroy);
    JSG_METHOD(mapAsync);
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
    std::unique_ptr<jsg::V8Ref<v8::ArrayBuffer>> buffer;
  };

  wgpu::Buffer buffer_;
  wgpu::Device device_;
  wgpu::BufferDescriptor desc_;
  kj::Own<AsyncRunner> async_;
  State state_ = State::Unmapped;
  kj::Vector<Mapping> mapped_;
  std::unique_ptr<jsg::V8Ref<v8::Object>> detachKey_;

  v8::Local<v8::ArrayBuffer> getMappedRange(jsg::Lock&,
                                            jsg::Optional<GPUSize64> offset,
                                            jsg::Optional<GPUSize64> size);
  void unmap(jsg::Lock& js);
  void destroy(jsg::Lock& js);
  jsg::Promise<void> mapAsync(GPUFlagsConstant mode,
                              jsg::Optional<GPUSize64> offset,
                              jsg::Optional<GPUSize64> size);
  void DetachMappings(jsg::Lock& js);
};

struct GPUBufferDescriptor {
  kj::String label;
  GPUSize64 size;
  GPUBufferUsageFlags usage;
  bool mappedAtCreation = false;
  JSG_STRUCT(label, size, usage, mappedAtCreation);
};

} // namespace workerd::api::gpu
