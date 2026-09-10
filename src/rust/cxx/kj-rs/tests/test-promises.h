#pragma once

#include "shared.h"

#include <kj/async.h>

#include <cstdint>

namespace kj_rs_demo {

kj::Promise<void> new_ready_promise_void();
kj::Promise<void> new_pending_promise_void();
kj::Promise<void> new_coroutine_promise_void();

kj::Promise<void> new_errored_promise_void();
kj::Promise<int32_t> new_ready_promise_i32(int32_t);
kj::Promise<Shared> new_ready_promise_shared_type();

// Cancellation testing helpers. The "cancellation-detecting promise" is a promise that never
// resolves, but increments a file-scope counter when it is destroyed (i.e., cancelled). Test code
// can use reset_cancellation_counter() and get_cancellation_counter() to observe whether and how
// many times cancellation occurred.
void reset_cancellation_counter();
uint64_t get_cancellation_counter();
kj::Promise<void> new_cancellation_detecting_promise_void();

// Manually fulfillable promise helpers. new_fulfillable_promise_void() stores a fulfiller and
// returns a promise; fulfill_stored_promise() fulfills it.
kj::Promise<void> new_fulfillable_promise_void();
void fulfill_stored_promise();

}  // namespace kj_rs_demo
