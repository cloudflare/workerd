#include <workerd/rust/async/promise.h>

#include <kj/debug.h>

namespace workerd::rust::async {

// If these static assertions ever fire, we must update the `pub struct OwnPromiseNode` definition
// in promise.rs to match the new C++ size/layout.
//
// TODO(cleanup): Integrate bindgen into build system to obviate this.
static_assert(sizeof(OwnPromiseNode) == sizeof(uint64_t) * 1,
    "OwnPromiseNode size changed");
static_assert(alignof(OwnPromiseNode) == alignof(uint64_t) * 1,
    "OwnPromiseNode alignment changed");

void own_promise_node_drop_in_place(PtrOwnPromiseNode node) {
  kj::dtor(*node);
}

OwnPromiseNode promise_into_own_promise_node_void(PromiseVoid promise) {
  return kj::_::PromiseNode::from(kj::mv(promise));
};

void promise_drop_in_place_void(PtrPromiseVoid promise) {
  kj::dtor(*promise);
}

}  // namespace workerd::rust::async
