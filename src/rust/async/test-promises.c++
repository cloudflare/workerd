#include <workerd/rust/async/test-promises.h>

#include <kj/debug.h>

namespace workerd::rust::async {

kj::Promise<void> new_ready_promise_void() {
  return kj::Promise<void>(kj::READY_NOW);
}

kj::Promise<int32_t> new_ready_promise_i32(int32_t value) {
  return kj::Promise<int32_t>(value);
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

kj::Promise<void> new_errored_promise_void() {
  return KJ_EXCEPTION(FAILED, "test error");
}

}  // namespace workerd::rust::async
