// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <cstdint>

// What test_bridge.rs calls back in C++, defined in test-bridge.c++.
namespace workerd::io_context::test {

// Records the async context frame that is current, in the isolate `isolate`.
void record_async_context(uintptr_t isolate);

// Records the IoContext that is current, from the callback of an await_io_and_run().
void record_await_context();

// Records the IoContext that is current, if any, when the data of register_reaction() is dropped.
void record_drop_context();

}  // namespace workerd::io_context::test
