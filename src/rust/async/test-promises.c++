#include <workerd/rust/async/test-promises.h>

namespace workerd::rust::async {

OwnPromiseNode new_ready_promise_node() {
  return kj::_::PromiseNode::from(kj::Promise<void>(kj::READY_NOW));
}

OwnPromiseNode new_pending_promise_node() {
  return kj::_::PromiseNode::from(kj::Promise<void>(kj::NEVER_DONE));
}

OwnPromiseNode new_coroutine_promise_node() {
  return kj::_::PromiseNode::from([]() -> kj::Promise<void> {
    co_await kj::Promise<void>(kj::READY_NOW);
    co_await kj::Promise<void>(kj::READY_NOW);
    co_await kj::Promise<void>(kj::READY_NOW);
  }());
}

}  // namespace workerd::rust::async
