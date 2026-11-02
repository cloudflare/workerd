// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-api-isolate.h"

namespace workerd::server {

void registerJsgWorkerdIsolateTemplates7() {
  JsgWorkerdIsolate_TypeWrapper::registerTemplateShard<7, JSG_WORKERD_ISOLATE_TEMPLATE_SHARDS>();
}

}  // namespace workerd::server
