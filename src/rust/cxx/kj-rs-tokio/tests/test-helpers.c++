#include "kj-rs-tokio-test/test-helpers.h"

#include "kj-rs-tokio-test/lib.rs.h"

#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/thread.h>

#include <chrono>
#include <thread>

namespace kj_rs_tokio_test {
namespace {

// Per thread, like the loop they belong to: a test that runs two Runtimes on two threads
// installs each thread's own.
thread_local kj::Timer *testTimer = nullptr;
thread_local kj::WaitScope *testWaitScope = nullptr;
thread_local kj::Maybe<kj::Own<kj::PromiseFulfiller<int>>> testFulfiller;

using ExecutorSlot = kj::MutexGuarded<kj::Maybe<kj::Own<const kj::Executor>>>;
ExecutorSlot executorSlots[2];

ExecutorSlot &slotAt(uint8_t slot) {
  KJ_REQUIRE(slot < 2, "executor slot out of range", slot);
  return executorSlots[slot];
}

}  // namespace

void setTestTimer(kj::Timer *timer) {
  testTimer = timer;
}

void setTestWaitScope(kj::WaitScope *ws) {
  testWaitScope = ws;
}

void installTestContext(kj_rs_tokio::TokioAsyncIoContext &context) {
  setTestTimer(&context.getTimer());
  setTestWaitScope(&context.getWaitScope());
}

void clearTestContext() {
  setTestTimer(nullptr);
  setTestWaitScope(nullptr);
}

kj::Promise<void> kjTimerDelay(uint64_t ms) {
  KJ_REQUIRE(testTimer != nullptr, "setTestTimer() not called");
  return testTimer->afterDelay(ms * kj::MILLISECONDS);
}

kj::Promise<void> kjNeverPromise() {
  return kj::NEVER_DONE;
}

void nestedWait() {
  KJ_REQUIRE(testWaitScope != nullptr, "setTestWaitScope() not called");
  KJ_REQUIRE(testTimer != nullptr, "setTestTimer() not called");
  // A promise that cannot complete in the turns wait() runs first, so wait() has to sleep.
  testTimer->afterDelay(1 * kj::MILLISECONDS).wait(*testWaitScope);
}

void setTestFulfiller(kj::Maybe<kj::Own<kj::PromiseFulfiller<int>>> fulfiller) {
  testFulfiller = kj::mv(fulfiller);
}

kj::Promise<int32_t> testFulfillerPromise() {
  auto paf = kj::newPromiseAndFulfiller<int>();
  setTestFulfiller(kj::mv(paf.fulfiller));
  return kj::mv(paf.promise);
}

void fulfillTestFulfiller(int32_t value) {
  KJ_ASSERT_NONNULL(testFulfiller, "setTestFulfiller() not called")->fulfill(kj::cp(value));
}

kj::Promise<int32_t> executeSyncFromThread(int32_t value) {
  auto paf = kj::newPromiseAndCrossThreadFulfiller<int32_t>();
  const kj::Executor &executor = kj::getCurrentThreadExecutor();
  auto thread = kj::heap<kj::Thread>(
      [&executor, value, fulfiller = kj::mv(paf.fulfiller)]() mutable noexcept {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // executeSync() blocks this thread until the loop thread -- parked by now -- has drained its
    // Executor and run the function.
    int32_t result = executor.executeSync([value]() { return value * 2; });
    fulfiller->fulfill(kj::mv(result));
  });
  // ~kj::Thread joins, so the thread is gone before the promise's owner is.
  return paf.promise.attach(kj::mv(thread));
}

kj::Promise<int32_t> crossThreadFulfillFromThread(uint64_t delayMs, int32_t value) {
  auto paf = kj::newPromiseAndCrossThreadFulfiller<int32_t>();
  auto thread = kj::heap<kj::Thread>([delayMs, value, fulfiller = kj::mv(paf.fulfiller)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    fulfiller->fulfill(kj::cp(value));
  });
  return paf.promise.attach(kj::mv(thread));
}

void publishExecutor(uint8_t slot) {
  *slotAt(slot).lockExclusive() = kj::getCurrentThreadExecutor().addRef();
}

void clearExecutor(uint8_t slot) {
  *slotAt(slot).lockExclusive() = kj::none;
}

kj::Promise<int32_t> executeAsyncOn(uint8_t slot, int32_t value) {
  kj::Own<const kj::Executor> executor;
  {
    auto lock = slotAt(slot).lockExclusive();
    lock.wait([](auto &v) { return v != kj::none; });
    executor = KJ_ASSERT_NONNULL(*lock)->addRef();
  }
  return executor->executeAsync([value]() { return value; }).attach(kj::mv(executor));
}

kj::Promise<void> kjYieldUntilWouldSleep() {
  return kj::yieldUntilWouldSleep();
}

kj::Promise<void> kjAwaitsRustSleep(uint64_t ms) {
  co_await tokio_sleep_on_runtime(ms);
}

}  // namespace kj_rs_tokio_test
