#pragma once

#include <kj/async.h>
#include <rust/cxx.h>

namespace workerd::rust::async {

using OwnPromiseNode = kj::_::OwnPromiseNode;
using PtrOwnPromiseNode = OwnPromiseNode*;

void own_promise_node_drop_in_place(OwnPromiseNode*);

OwnPromiseNode new_ready_promise_node();
OwnPromiseNode new_coroutine_promise_node();

}  // namespace workerd::rust::async

namespace rust {

// OwnPromiseNodes happen to follow Rust move semantics.
template <>
struct IsRelocatable<::workerd::rust::async::OwnPromiseNode>: std::true_type {};

}  // namespace rust
