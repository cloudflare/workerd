#pragma once

#include <workerd/rust/async/promise.h>

namespace workerd::rust::async {

OwnPromiseNode new_ready_promise_node();
OwnPromiseNode new_pending_promise_node();
OwnPromiseNode new_coroutine_promise_node();

}  // namespace workerd::rust::async
