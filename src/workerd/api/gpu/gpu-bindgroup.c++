// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-bindgroup.h"

namespace workerd::api::gpu {

wgpu::BindGroupEntry parseBindGroupEntry(GPUBindGroupEntry &entry) {
  wgpu::BindGroupEntry e;
  e.binding = entry.binding;

  KJ_SWITCH_ONEOF(entry.resource) {
    KJ_CASE_ONEOF(buffer, GPUBufferBinding) {
      e.buffer = *buffer.buffer;
      KJ_IF_MAYBE (offset, buffer.offset) {
        e.offset = *offset;
      }
      KJ_IF_MAYBE(size, buffer.size) {
        e.size = *size;
      }
    }
    KJ_CASE_ONEOF(sampler, jsg::Ref<GPUSampler>) {
      e.sampler = *sampler;
    }
  }

  return kj::mv(e);
};

} // namespace workerd::api::gpu
