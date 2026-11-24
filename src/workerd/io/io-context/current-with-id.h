// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>

#include <cstdint>

namespace workerd::io_context {

// The current IoContext, which must be the one `id` identifies; throws otherwise. Rust identifies
// an IoContext by its IoContext::Id as a uint64_t (`io_context::IoContext::as_ffi()`), and the
// C++ shims of ffi.h and of other crates find the IoContext with this.
IoContext& currentWithId(uint64_t id);

}  // namespace workerd::io_context
