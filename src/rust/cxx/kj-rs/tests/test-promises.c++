#include "kj-rs-demo/lib.rs.h"

#include <kj-rs-demo/test-promises.h>

#include <kj/debug.h>

namespace kj_rs_demo {

namespace {
uint64_t cancellationCounter = 0;
}

kj::Promise<void> new_ready_promise_void() {
  return kj::READY_NOW;
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

kj::Promise<Shared> new_ready_promise_shared_type() {
  return Shared{42};
}

void reset_cancellation_counter() {
  cancellationCounter = 0;
}

uint64_t get_cancellation_counter() {
  return cancellationCounter;
}

kj::Promise<void> new_cancellation_detecting_promise_void() {
  return kj::Promise<void>(kj::NEVER_DONE).attach(kj::defer([]() { ++cancellationCounter; }));
}

kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> storedFulfiller;

kj::Promise<void> new_fulfillable_promise_void() {
  auto paf = kj::newPromiseAndFulfiller<void>();
  storedFulfiller = kj::mv(paf.fulfiller);
  return kj::mv(paf.promise);
}

void fulfill_stored_promise() {
  KJ_ASSERT_NONNULL(storedFulfiller)->fulfill();
  storedFulfiller = kj::none;
}

}  // namespace kj_rs_demo
