// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "eventsource.h"

#include <workerd/api/streams/common.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

struct SourceState {
  bool readPending = false;
  bool canceled = false;
  bool destroyed = false;
  uint chunks = 0;
};

class GatedSource final: public ReadableStreamSource {
 public:
  GatedSource(kj::Promise<void> gate, SourceState& state): gate(kj::mv(gate)), state(state) {}

  ~GatedSource() noexcept(false) {
    state.destroyed = true;
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (state.chunks == 0) {
      state.readPending = true;
      co_await gate;
      state.readPending = false;
    }
    kj::StringPtr chunk;
    switch (state.chunks) {
      case 0:
        chunk = "id: first\nretry: 1000\ndata: one\n\n"_kj;
        break;
      case 1:
        chunk = "id: second\ndata: two\n\n"_kj;
        break;
      default:
        co_return 0;
    }
    KJ_REQUIRE(maxBytes >= chunk.size());
    kj::arrayPtr(static_cast<kj::byte*>(buffer), chunk.size()).copyFrom(chunk.asBytes());
    ++state.chunks;
    co_return chunk.size();
  }

  void cancel(kj::Exception reason) override {
    state.canceled = true;
  }

 private:
  kj::Promise<void> gate;
  SourceState& state;
};

enum class PumpOutcome { COMPLETE, CANCEL };

KJ_TEST("EventSource native pump owns parser state after its EventSource is destroyed") {
  for (auto typescript: kj::arr(false, true)) {
    for (auto outcome: kj::arr(PumpOutcome::COMPLETE, PumpOutcome::CANCEL)) {
      SourceState state;
      capnp::MallocMessageBuilder message;
      auto flags = message.initRoot<CompatibilityFlags>();
      flags.setTypeScriptImplementedStreams(typescript);
      TestFixture fixture({
        .featureFlags = flags.asReader(),
        .autogates = kj::arr("per-isolate-javascript-bootstrap"_kj),
      });
      auto gate = kj::newPromiseAndFulfiller<void>();
      fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
        kj::Promise<void> pump = nullptr;
        jsg::WeakRef<EventSource> weakSource(nullptr);
        {
          auto source = env.js.alloc<EventSource>(env.js);
          weakSource = source.getWeakRef(env.js);
          auto readable = JsReadableStream::create(
              env.js, env.context, kj::heap<GatedSource>(kj::mv(gate.promise), state));
          pump = env.context.waitForDeferredProxy(
              readable.pumpTo(env.js, source->newSink(env.js), EndStream::YES));
          KJ_ASSERT(state.readPending);
        }
        // Keep a read pending while releasing the EventSource owner. No wrapper was
        // exported to JS, so destruction is synchronous.
        KJ_ASSERT(!weakSource.isAlive());
        if (outcome == PumpOutcome::CANCEL) {
          pump = nullptr;
          KJ_EXPECT(state.canceled);
          return kj::READY_NOW;
        }
        gate.fulfiller->fulfill();
        return kj::mv(pump);
      });
      KJ_EXPECT(state.destroyed);
      KJ_EXPECT(state.canceled == (outcome == PumpOutcome::CANCEL));
      KJ_EXPECT(state.chunks == (outcome == PumpOutcome::COMPLETE ? 2 : 0));
    }
  }
}

}  // namespace
}  // namespace workerd::api
