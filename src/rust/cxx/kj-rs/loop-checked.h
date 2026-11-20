#pragma once

#include <rust/cxx.h>

#include <kj/async.h>

namespace kj_rs {

// The id of a KJ event loop. Rust holds one with a value that belongs to the loop, so that from
// any thread it can tell whether the loop is current.
using EventLoopId = kj::EventLoop::Id;

// Throws if the thread has no event loop.
EventLoopId event_loop_id_current();
bool event_loop_id_is_current(EventLoopId id);
// Throws the exception for using a value off its event loop.
void event_loop_id_wrong_loop();
// Aborts the process for dropping a value off its event loop.
void loop_checked_dropped_off_loop();
// Aborts the process in a `kj::DisallowAsyncDestructorsScope`, as destroying a KJ async object
// does.
void require_async_destructors_allowed();

}  // namespace kj_rs

namespace rust {

template <>
struct IsRelocatable<::kj_rs::EventLoopId>: std::true_type {};

}  // namespace rust
