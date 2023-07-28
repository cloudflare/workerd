// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-utils.h"
#include <webgpu/webgpu_cpp.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::gpu {

class GPUQuerySet : public jsg::Object {
public:
  // Implicit cast operator to Dawn GPU object
  inline operator const wgpu::QuerySet &() const { return querySet_; }
  explicit GPUQuerySet(wgpu::QuerySet q) : querySet_(kj::mv(q)){};
  JSG_RESOURCE_TYPE(GPUQuerySet) {}

private:
  wgpu::QuerySet querySet_;
};

struct GPUQuerySetDescriptor {
  jsg::Optional<kj::String> label;

  kj::String type;
  GPUSize32 count;

  JSG_STRUCT(label);
};

} // namespace workerd::api::gpu
