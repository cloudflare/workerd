#pragma once

#include <workerd/rust/async/promise.h>

namespace workerd::rust::async {

kj::Promise<void> new_ready_promise_void();
kj::Promise<void> new_pending_promise_void();
kj::Promise<void> new_coroutine_promise_void();

}  // namespace workerd::rust::async
