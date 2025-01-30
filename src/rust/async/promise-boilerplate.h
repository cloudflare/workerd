#pragma once

#include <workerd/rust/async/await.h>
#include <workerd/rust/async/promise.h>

namespace workerd::rust::async {

// TODO(now): Generate boilerplate with a macro.
using PromiseVoid = Promise<void>;
using PtrPromiseVoid = PromiseVoid*;

void own_promise_node_unwrap_void(OwnPromiseNode);
void promise_drop_in_place_void(PtrPromiseVoid);
OwnPromiseNode promise_into_own_promise_node_void(PromiseVoid);

}  // namespace workerd::rust::async
