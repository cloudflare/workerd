#include "kj-rs/promise.h"

#include <kj/debug.h>

namespace kj_rs {

// If these static assertions ever fire, we must update the `pub struct OwnPromiseNode` definition
// in promise.rs to match the new C++ size/layout.
//
// TODO(cleanup): Integrate bindgen into build system to obviate this.
static_assert(sizeof(OwnPromiseNode) == sizeof(uint64_t) * 1, "OwnPromiseNode size changed");
static_assert(alignof(OwnPromiseNode) == alignof(uint64_t) * 1, "OwnPromiseNode alignment changed");

void own_promise_node_drop_in_place(OwnPromiseNode* node) {
  kj::dtor(*node);
}

}  // namespace kj_rs
