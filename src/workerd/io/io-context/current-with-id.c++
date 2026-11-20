// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "current-with-id.h"

#include <bit>

namespace workerd::io_context {

IoContext& currentWithId(uint64_t id) {
  auto& context = KJ_REQUIRE_NONNULL(IoContext::tryCurrent(), "there is no current request");
  KJ_REQUIRE(context.getId() == std::bit_cast<IoContext::Id>(id),
      "the IoContext is not the one of the current request");
  return context;
}

}  // namespace workerd::io_context
