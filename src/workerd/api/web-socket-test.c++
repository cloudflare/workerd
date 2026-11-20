// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "web-socket.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Bytes allocated on the JavaScript heap of the current isolate so far. V8 counts a linear
// allocation buffer when it hands it out, so a difference is exact to within one buffer per space.
uint64_t totalAllocatedBytes(Worker::Lock& lock) {
  v8::HeapStatistics stats;
  lock.getIsolate()->GetHeapStatistics(&stats);
  return stats.total_allocated_bytes();
}

// Enters the isolate `count` times. When `ws` is set, each entry sends a message on it, and the
// peer receives the message before the next entry, so each send finds the queue drained and starts
// a new pump. Returns the bytes allocated on the JavaScript heap meanwhile.
kj::Promise<uint64_t> enterRepeatedly(
    IoContext& context, uint count, kj::Maybe<jsg::Ref<WebSocket>&> ws, kj::WebSocket& peer) {
  auto start = co_await context.run([](Worker::Lock& lock) { return totalAllocatedBytes(lock); });
  for (auto i KJ_UNUSED: kj::zeroTo(count)) {
    co_await context.run([&](Worker::Lock& lock) {
      KJ_IF_SOME(w, ws) {
        w->send(lock, kj::str("message"));
      }
    });
    if (ws != kj::none) {
      auto message = co_await peer.receive();
      KJ_EXPECT(message.get<kj::String>() == "message");
    }
    // Let the pump end, and whatever its end runs, before the next send.
    co_await kj::evalLast([]() {});
  }
  auto end = co_await context.run([](Worker::Lock& lock) { return totalAllocatedBytes(lock); });
  co_return end - start;
}

// Checks the WebSocket's state. Its getters dereference IoOwns, so they need the IoContext to be
// current, which it isn't after a co_await.
kj::Promise<void> expectState(IoContext& context, WebSocket& ws, int readyState, bool released) {
  co_await context.run([&](Worker::Lock&) {
    KJ_EXPECT(ws.getReadyState() == readyState, ws.getReadyState(), readyState);
    KJ_EXPECT(ws.isReleased() == released);
  });
}

void expectDrainedSendsAllocateNothing(TestFixture::SetupParams&& params) {
  TestFixture fixture(kj::mv(params));
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& context = env.context;
    auto pipe = kj::newWebSocketPipe();
    auto peer = kj::mv(pipe.ends[1]);
    auto ws = env.js.alloc<WebSocket>(env.js, kj::mv(pipe.ends[0]));
    ws->accept(env.js, kj::none);

    // Warm up, then compare sending against entering the isolate without sending.
    co_await enterRepeatedly(context, 64, ws, *peer);
    constexpr uint COUNT = 4096;
    auto entering = co_await enterRepeatedly(context, COUNT, kj::none, *peer);
    auto sending = co_await enterRepeatedly(context, COUNT, ws, *peer);

    // Waiting for each pump with JavaScript promises would allocate about 860 bytes per send
    // (promises, their reactions, functions and opaque wrappers), 3.5 MB here. Allow a few
    // allocation buffers.
    KJ_EXPECT(sending < entering + 128 * 1024, sending, entering);
    co_await expectState(context, *ws, WebSocket::READY_STATE_OPEN, false);
  });
}

KJ_TEST("WebSocket sends after the queue drains allocate nothing on the JavaScript heap") {
  // A pump ends whenever the outgoing queue drains, so a server that sends to many WebSockets
  // every frame starts a pump per socket per frame. V8 frees what JavaScript allocates for that
  // only in a full GC.
  expectDrainedSendsAllocateNothing({.useRealTimers = false});
}

KJ_TEST(
    "WebSocket sends after the queue drains allocate nothing on the JavaScript heap in an actor") {
  expectDrainedSendsAllocateNothing({
    .actorId = Worker::Actor::Id(kj::str("web-socket-test")),
    .useRealTimers = false,
  });
}

KJ_TEST("WebSocket is released when the pump ends after both directions close") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& context = env.context;
    auto pipe = kj::newWebSocketPipe();
    auto peer = kj::mv(pipe.ends[1]);
    auto ws = env.js.alloc<WebSocket>(env.js, kj::mv(pipe.ends[0]));
    ws->accept(env.js, WebSocket::AcceptOptions{.allowHalfOpen = true});

    // The peer closes first. With a half-open WebSocket the read loop can't release the native
    // socket, since the outgoing side is still open.
    co_await peer->close(1000, "peer done");
    co_await kj::evalLast([]() {});
    co_await expectState(context, *ws, WebSocket::READY_STATE_CLOSING, false);

    // Closing the outgoing side starts a pump that sends the Close frame. When it ends, both
    // directions are closed, so its end releases the native socket.
    co_await context.run(
        [&](Worker::Lock& lock) { ws->close(lock, 1000, jsg::USVString(kj::str("done"))); });
    auto message = co_await peer->receive();
    KJ_EXPECT(message.get<kj::WebSocket::Close>().code == 1000);
    co_await kj::evalLast([]() {});
    co_await expectState(context, *ws, WebSocket::READY_STATE_CLOSED, true);
  });
}

KJ_TEST("WebSocket reports an error when its pump fails") {
  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& context = env.context;
    auto pipe = kj::newWebSocketPipe();
    auto peer = kj::mv(pipe.ends[1]);
    auto ws = env.js.alloc<WebSocket>(env.js, kj::mv(pipe.ends[0]));
    ws->accept(env.js, kj::none);

    // The peer goes away without receiving, so the pump's send fails.
    co_await context.run([&](Worker::Lock& lock) { ws->send(lock, kj::str("message")); });
    peer = nullptr;
    co_await kj::evalLast([]() {});

    // The read loop reports the disconnect as a close event and doesn't set an error, so CLOSED
    // here means the pump's failure was reported.
    co_await expectState(context, *ws, WebSocket::READY_STATE_CLOSED, true);
  });
}

}  // namespace
}  // namespace workerd::api
