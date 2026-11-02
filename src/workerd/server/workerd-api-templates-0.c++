// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-api-isolate.h"

namespace workerd::server {

JSG_DEFINE_ISOLATE_TEMPLATE_SHARD(JsgWorkerdIsolate, 0);

}  // namespace workerd::server
