#pragma once

#include <workerd/rust/async/awaiter.h>
#include <workerd/rust/async/promise.h>

namespace workerd::rust::async {

// TODO(now): Generate boilerplate with a macro.
using PromiseVoid = Promise<void>;
using PtrPromiseVoid = PromiseVoid*;
void own_promise_node_unwrap_void(OwnPromiseNode);
void promise_drop_in_place_void(PtrPromiseVoid);
OwnPromiseNode promise_into_own_promise_node_void(PromiseVoid);

using PromiseI32 = Promise<int32_t>;
using PtrPromiseI32 = PromiseI32*;
int32_t own_promise_node_unwrap_i32(OwnPromiseNode);
void promise_drop_in_place_i32(PtrPromiseI32);
OwnPromiseNode promise_into_own_promise_node_i32(PromiseI32);

}  // namespace workerd::rust::async
