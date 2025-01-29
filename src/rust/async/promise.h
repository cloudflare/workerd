#pragma once

#include <kj/async.h>
#include <rust/cxx.h>

namespace workerd::rust::async {

using OwnPromiseNode = kj::_::OwnPromiseNode;
using PtrOwnPromiseNode = OwnPromiseNode*;

void own_promise_node_drop_in_place(OwnPromiseNode*);

template <typename T>
using Promise = kj::Promise<T>;

// TODO(now): Generate boilerplate with a macro.
using PromiseVoid = Promise<void>;
using PtrPromiseVoid = PromiseVoid*;

OwnPromiseNode promise_into_own_promise_node_void(PromiseVoid);
void promise_drop_in_place_void(PtrPromiseVoid);

}  // namespace workerd::rust::async

namespace rust {

// OwnPromiseNodes happen to follow Rust move semantics.
template <>
struct IsRelocatable<::workerd::rust::async::OwnPromiseNode>: std::true_type {};

// Promises also follow Rust move semantics.
template <typename T>
struct IsRelocatable<::workerd::rust::async::Promise<T>>: std::true_type {};

}  // namespace rust
