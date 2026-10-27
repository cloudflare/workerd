// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <cstdint>

// What test_bridge.rs calls back in io-context-test.c++, which defines it.
namespace workerd::io_context::test {

// Records the async context frame that is current, in the isolate `isolate`.
void record_async_context(uintptr_t isolate);

}  // namespace workerd::io_context::test
