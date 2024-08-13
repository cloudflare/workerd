// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-async-runner.h"
#include <webgpu/webgpu_cpp.h>

namespace workerd::api::gpu {

// This class will own the needed dawn objects
// for a specific implementation and will also
// be used to flush dawn commands.
class DawnContainer : public Flusher {
public:
  virtual wgpu::Instance getInstance() = 0;
  virtual ~DawnContainer() = default;
};


} // namespace workerd::api::gpu
