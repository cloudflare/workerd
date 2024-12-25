#include <workerd/rust/async/promise.h>

#include <kj/debug.h>

namespace workerd::rust::async {

void own_promise_node_drop_in_place(OwnPromiseNode* node) {
  node->~OwnPromiseNode();
}

}  // namespace workerd::rust::async
