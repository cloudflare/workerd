// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-query-set.h"

#include <workerd/jsg/exception.h>

namespace workerd::api::gpu {

wgpu::QueryType parseQueryType(kj::StringPtr type) {
  if (type == "occlusion") {
    return wgpu::QueryType::Occlusion;
  }

  if (type == "timestamp") {
    return wgpu::QueryType::Timestamp;
  }

  JSG_FAIL_REQUIRE(TypeError, "unknown Query type", type);
}

}  // namespace workerd::api::gpu
