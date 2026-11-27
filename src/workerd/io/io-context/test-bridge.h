// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/io/io-context/ffi.h>

#include <cstdint>

// What test_bridge.rs calls back in C++, defined in test-bridge.c++.
namespace workerd::io_context::test {

// Records the async context frame that is current, in the isolate `isolate`.
void record_async_context(uintptr_t isolate);

// Records the IoContext that is current, from the callback of an await_io_and_run().
void record_await_context();

// Records the IoContext that is current, if any, when the data of register_reaction() is dropped.
void record_drop_context();

// Keeps `channel`, which Rust took from a Fetcher in a reentry callback, for the test to check
// (keptChannel in test-bridge-state.h).
void keep_channel(kj::Rc<IoChannelFactory::SubrequestChannel> channel);

// A prelude for run_in_wait_until() that returns the number 42, or, if `throws`, throws a
// JavaScript Error ("the prelude threw"), leaving it pending, as a class entrypoint's constructor
// does when it throws.
kj::Own<RunPrelude> new_run_prelude(bool throws);

}  // namespace workerd::io_context::test
