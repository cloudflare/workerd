// Tests for TokioEventPort / setupTokioAsyncIo: a kj::EventLoop driven by a per-thread tokio
// current_thread runtime. Following kj-rs conventions, C++ KJ_TESTs drive; Rust helpers (see
// tests/lib.rs, bridged by workerd-cxx) provide async behaviors.

#include "kj-rs-tokio-test/lib.rs.h"
#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace kj_rs_tokio_test {
namespace {

using kj_rs_tokio::setupTokioAsyncIo;

void delayMillis(uint64_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

// =======================================================================================
// Basics: the KJ event loop works as usual on top of the tokio-backed port.

KJ_TEST("promises resolve on a TokioEventPort loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  KJ_EXPECT(kj::evalLater([]() { return 123; }).wait(ws) == 123);
  KJ_EXPECT(kj::Promise<int>(42).wait(ws) == 42);
}

KJ_TEST("evalLater ordering is preserved") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj::Vector<int> order;
  // eagerlyEvaluate arms each promise immediately (KJ promises are otherwise lazy: continuations
  // only get scheduled once awaited), so all three events sit in the queue in creation order.
  auto p1 = kj::evalLater([&]() { order.add(1); }).eagerlyEvaluate(nullptr);
  auto p2 = kj::evalLater([&]() { order.add(2); }).eagerlyEvaluate(nullptr);
  auto p3 = kj::evalLater([&]() { order.add(3); }).eagerlyEvaluate(nullptr);

  // Waiting on the last promise runs all three events in FIFO order.
  p3.wait(ws);
  p1.wait(ws);
  p2.wait(ws);

  KJ_ASSERT(order.size() == 3);
  KJ_EXPECT(order[0] == 1);
  KJ_EXPECT(order[1] == 2);
  KJ_EXPECT(order[2] == 3);
}

KJ_TEST("promise chains resolve") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  auto promise =
      kj::evalLater([]() { return 1; }).then([](int v) {
    return kj::Promise<int>(v + 1);
  }).then([](int v) { return v * 10; });
  KJ_EXPECT(promise.wait(ws) == 20);
}

KJ_TEST("evalLast fires when the loop would sleep") {
  // kj::evalLast events live on the would-sleep queue, which is only serviced through the
  // EventLoop::poll() path (EventLoop::wait() switches to poll() when would-sleep waiters
  // exist). This test hangs or misorders if the port's poll() is broken.
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  kj::Vector<int> order;
  auto last = kj::evalLast([&]() { order.add(2); });
  auto later = kj::evalLater([&]() { order.add(1); });

  later.wait(ws);
  // evalLast must not have run yet: the loop never ran out of work while waiting on `later`.
  KJ_ASSERT(order.size() == 1);
  KJ_EXPECT(order[0] == 1);

  last.wait(ws);
  KJ_ASSERT(order.size() == 2);
  KJ_EXPECT(order[1] == 2);
}

}  // namespace
}  // namespace kj_rs_tokio_test
