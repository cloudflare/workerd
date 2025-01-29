#include <workerd/rust/async/test-promises.h>

namespace workerd::rust::async {

kj::Promise<void> new_ready_promise_void() {
  return kj::Promise<void>(kj::READY_NOW);
}

kj::Promise<void> new_pending_promise_void() {
  return kj::Promise<void>(kj::NEVER_DONE);
}

kj::Promise<void> new_coroutine_promise_void() {
  return []() -> kj::Promise<void> {
    co_await kj::Promise<void>(kj::READY_NOW);
    co_await kj::Promise<void>(kj::READY_NOW);
    co_await kj::Promise<void>(kj::READY_NOW);
  }();
}

}  // namespace workerd::rust::async
