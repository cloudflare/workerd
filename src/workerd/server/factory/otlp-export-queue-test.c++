// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "otlp-export-queue.h"

#include <kj/function.h>
#include <kj/test.h>
#include <kj/vector.h>

#include <cstring>

namespace workerd::server {
namespace {

void appendVarint(kj::Vector<kj::byte>& bytes, size_t value) {
  while (value >= 128) {
    bytes.add((value & 127) | 128);
    value >>= 7;
  }
  bytes.add(value);
}

kj::Array<const kj::byte> makeMessage(uint index, size_t padding = 0) {
  // A schema_url-only ResourceSpans gives each encoded export a distinct, adjustable payload.
  auto name = kj::str(index);
  auto payload = kj::heapArray<kj::byte>(name.size() + padding);
  std::memset(payload.begin(), 'x', payload.size());
  payload.slice(0, name.size()).copyFrom(name.asBytes());
  kj::Vector<kj::byte> resource;
  resource.add(0x1a);
  appendVarint(resource, payload.size());
  resource.addAll(payload);
  kj::Vector<kj::byte> request;
  request.add(0x0a);
  appendVarint(request, resource.size());
  request.addAll(resource);
  return request.releaseAsArray();
}

struct CollectorState {
  kj::Vector<kj::Array<const kj::byte>> requests;
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> pending;
  kj::Maybe<kj::Function<void()>> onComplete;
  uint active = 0;
  uint maxActive = 0;
  uint activeAtDestruction = 0;
  bool destroyed = false;
  bool throwNext = false;
};

class MockTransport final: public OtlpExportQueue::Transport {
 public:
  explicit MockTransport(CollectorState& state): state(state) {}
  ~MockTransport() noexcept(false) {
    state.activeAtDestruction = state.active;
    state.destroyed = true;
  }

  kj::Promise<void> post(kj::Array<const kj::byte> request) override {
    if (state.throwNext) {
      state.throwNext = false;
      KJ_FAIL_REQUIRE("collector setup failed");
    }
    state.requests.add(kj::mv(request));
    ++state.active;
    state.maxActive = kj::max(state.maxActive, state.active);
    auto pair = kj::newPromiseAndFulfiller<void>();
    state.pending.add(kj::mv(pair.fulfiller));
    return pair.promise.attach(kj::defer([&state = state]() {
      --state.active;
      KJ_IF_SOME(callback, state.onComplete) {
        callback();
      }
    }));
  }

 private:
  CollectorState& state;
};

struct Fixture {
  kj::EventLoop loop;
  kj::WaitScope ws{loop};
  kj::TimerImpl timer{kj::origin<kj::TimePoint>()};
  CollectorState collector;
  OtlpExportQueue queue{kj::heap<MockTransport>(collector), timer};

  void submit(uint index) {
    queue.exportSpans(makeMessage(index), 1);
  }

  void occupySlots() {
    for (uint i = 0; i < 16; ++i) submit(10000 + i);
    ws.poll();
    KJ_ASSERT(collector.requests.size() == 16);
    KJ_ASSERT(collector.active == 16);
  }
};

KJ_TEST("OTLP export queue bounds pending messages and restores admission after draining") {
  Fixture f;
  f.occupySlots();
  kj::Vector<kj::byte> expected;
  for (uint i = 0; i < 4096; ++i) {
    auto request = makeMessage(i);
    expected.addAll(request);
    f.queue.exportSpans(kj::mv(request), 1);
  }
  {
    KJ_EXPECT_LOG(WARNING, "OTLP export queue is full; dropping spans");
    f.submit(4096);
  }
  f.ws.poll();
  KJ_EXPECT(f.collector.requests.size() == 16);

  f.collector.pending[0]->fulfill();
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 17);
  // Exact body equality checks every accepted message's order and excludes the dropped message.
  KJ_EXPECT(f.collector.requests[16].asPtr() == expected.asPtr());
  KJ_EXPECT(f.collector.active == 16);
  KJ_EXPECT(f.collector.maxActive == 16);

  f.collector.pending[16]->fulfill();
  f.ws.poll();
  f.submit(4097);
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 18);
  KJ_EXPECT(f.collector.requests[17].asPtr() == makeMessage(4097).asPtr());
}

KJ_TEST("OTLP export batches stop at the soft byte threshold and preserve oversized messages") {
  Fixture f;
  f.occupySlots();
  auto first = makeMessage(0, 600 * 1024);
  auto second = makeMessage(1, 600 * 1024);
  auto exact = makeMessage(2, 1024 * 1024 - 9);
  KJ_ASSERT(exact.size() == 1024 * 1024);
  auto oversized = makeMessage(3, 1024 * 1024);
  kj::Vector<kj::byte> expected;
  expected.addAll(first);
  expected.addAll(second);
  auto exactCopy = kj::heapArray(exact.asPtr());
  auto oversizedCopy = kj::heapArray(oversized.asPtr());
  f.queue.exportSpans(kj::mv(first), 1);
  f.queue.exportSpans(kj::mv(second), 1);
  f.queue.exportSpans(kj::mv(exact), 1);
  f.queue.exportSpans(kj::mv(oversized), 1);
  f.submit(4);

  f.collector.pending[0]->fulfill();
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 17);
  KJ_EXPECT(f.collector.requests[16].asPtr() == expected.asPtr());
  f.collector.pending[16]->fulfill();
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 18);
  KJ_EXPECT(f.collector.requests[17].asPtr() == exactCopy.asPtr());
  f.collector.pending[17]->fulfill();
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 19);
  KJ_EXPECT(f.collector.requests[18].asPtr() == oversizedCopy.asPtr());
  f.collector.pending[18]->fulfill();
  f.ws.poll();
  KJ_ASSERT(f.collector.requests.size() == 20);
  KJ_EXPECT(f.collector.requests[19].asPtr() == makeMessage(4).asPtr());
  KJ_EXPECT(f.collector.maxActive == 16);
}

KJ_TEST("OTLP export failures release slots and drain queued messages without retrying") {
  Fixture f;
  f.occupySlots();
  f.queue.exportSpans(makeMessage(0, 1024 * 1024), 1);
  f.submit(1);
  f.collector.throwNext = true;
  {
    KJ_EXPECT_LOG(ERROR, "collector setup failed");
    f.collector.pending[0]->fulfill();
    f.ws.poll();
  }
  KJ_ASSERT(f.collector.requests.size() == 17);
  KJ_EXPECT(f.collector.requests[16].asPtr() == makeMessage(1).asPtr());
  f.submit(2);
  {
    KJ_EXPECT_LOG(ERROR, "collector rejected request");
    f.collector.pending[16]->reject(KJ_EXCEPTION(FAILED, "collector rejected request"));
    f.ws.poll();
  }
  KJ_ASSERT(f.collector.requests.size() == 18);
  KJ_EXPECT(f.collector.requests[17].asPtr() == makeMessage(2).asPtr());
  f.collector.pending[17]->fulfill();
  f.ws.poll();
  KJ_EXPECT(f.collector.requests.size() == 18);
  KJ_EXPECT(f.collector.active == 15);
}

KJ_TEST("OTLP export timeout cancels the request and drains queued messages without retrying") {
  Fixture f;
  f.submit(10000);
  f.ws.poll();
  f.timer.advanceTo(f.timer.now() + kj::SECONDS);
  for (uint i = 1; i < 16; ++i) f.submit(10000 + i);
  f.ws.poll();
  f.submit(0);
  f.submit(1);
  f.timer.advanceTo(f.timer.now() + 8 * kj::SECONDS);
  f.ws.poll();
  KJ_EXPECT(f.collector.requests.size() == 16);
  KJ_EXPECT(f.collector.pending[0]->isWaiting());
  {
    KJ_EXPECT_LOG(ERROR, "operation timed out");
    f.timer.advanceTo(f.timer.now() + kj::SECONDS);
    f.ws.poll();
  }
  KJ_ASSERT(f.collector.requests.size() == 17);
  KJ_EXPECT(!f.collector.pending[0]->isWaiting());
  kj::Vector<kj::byte> expected;
  expected.addAll(makeMessage(0));
  expected.addAll(makeMessage(1));
  KJ_EXPECT(f.collector.requests[16].asPtr() == expected.asPtr());
  KJ_EXPECT(f.collector.active == 16);
  KJ_EXPECT(f.collector.maxActive == 16);
  for (uint i = 1; i < 17; ++i) f.collector.pending[i]->fulfill();
  f.ws.poll();
  KJ_EXPECT(f.collector.requests.size() == 17);
  KJ_EXPECT(f.collector.active == 0);
}

KJ_TEST(
    "OTLP unlink disables reentrant exports before cancelling requests and releasing transport") {
  Fixture f;
  f.occupySlots();
  f.submit(0);
  uint callbacks = 0;
  f.collector.onComplete = [&]() {
    ++callbacks;
    KJ_EXPECT(!f.collector.destroyed);
    f.submit(1);
  };
  f.queue.unlink();
  KJ_EXPECT(callbacks == 16);
  KJ_EXPECT(f.collector.active == 0);
  KJ_EXPECT(f.collector.destroyed);
  KJ_EXPECT(f.collector.activeAtDestruction == 0);
  f.submit(2);
  f.queue.unlink();
  f.ws.poll();
  KJ_EXPECT(f.collector.requests.size() == 16);
}

}  // namespace
}  // namespace workerd::server
