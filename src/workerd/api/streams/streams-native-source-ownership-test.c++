// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "internal.h"
#include "standard.h"

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/tests/test-fixture.h>

namespace workerd::api {
namespace {

KJ_TEST("native source ownership stays flat across repeated transfers") {
  struct State {
    size_t canceled = 0;
    size_t destroyed = 0;
  } state;
  class CountingSource final: public ReadableStreamSource {
   public:
    explicit CountingSource(State& state): state(state) {}
    ~CountingSource() noexcept(false) {
      ++state.destroyed;
    }
    kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
      return size_t(0);
    }
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding) override {
      return uint64_t(1);
    }
    void cancel(kj::Exception) override {
      ++state.canceled;
    }

   private:
    State& state;
  };

  TestFixture fixture;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto controller =
        newReadableStreamInternalController(env.context, kj::heap<CountingSource>(state));
    for (size_t i = 0; i < 100000; ++i) {
      controller = controller->detach(env.js, false);
    }
    KJ_ASSERT(KJ_ASSERT_NONNULL(controller->tryGetLength(StreamEncoding::IDENTITY)) == 1);
    KJ_ASSERT(state.canceled == 0 && state.destroyed == 0);
    auto canceled = controller->cancel(env.js, kj::none);
    KJ_ASSERT(state.canceled == 1 && state.destroyed == 1);
    return env.context.awaitJs(env.js, kj::mv(canceled));
  });
}

KJ_TEST("native pipe read retains its source until the read is destroyed") {
  struct State {
    bool readPending = false;
    bool sourceCanceled = false;
    bool sourceDestroyed = false;
    bool destroyedDuringRead = false;
  };
  auto state = kj::rc<State>();
  class PendingReadSource final: public ReadableStreamSource {
   public:
    PendingReadSource(kj::Rc<State> state,
        kj::Promise<size_t> readPromise,
        kj::Own<kj::PromiseFulfiller<void>> destroyed)
        : state(kj::mv(state)),
          readPromise(kj::mv(readPromise)),
          destroyed(kj::mv(destroyed)) {}
    ~PendingReadSource() noexcept(false) {
      state->sourceDestroyed = true;
      state->destroyedDuringRead = state->readPending;
      destroyed->fulfill();
    }
    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      KJ_ASSERT(minBytes <= 1 && maxBytes >= 1);
      static_cast<kj::byte*>(buffer)[0] = 1;
      state->readPending = true;
      return kj::mv(readPromise).attach(kj::defer([state = state.addRef()]() mutable {
        state->readPending = false;
      }));
    }
    void cancel(kj::Exception) override {
      state->sourceCanceled = true;
    }

   private:
    kj::Rc<State> state;
    kj::Promise<size_t> readPromise;
    kj::Own<kj::PromiseFulfiller<void>> destroyed;
  };

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto read = kj::newPromiseAndFulfiller<size_t>();
    auto destroyed = kj::newPromiseAndFulfiller<void>();
    auto source = JsReadableStream::create(env.js, env.context,
        kj::heap<PendingReadSource>(
            state.addRef(), kj::mv(read.promise), kj::mv(destroyed.fulfiller)));
    kj::Maybe<jsg::Ref<WritableStreamDefaultController>> sinkController;
    UnderlyingSink sink;
    sink.start = [&](jsg::Lock& js, UnderlyingSink::Controller controller) {
      sinkController = controller.addRef();
      return js.resolvedPromise();
    };
    auto destination = WritableStream::constructor(env.js, kj::mv(sink), kj::none);
    JsWritableStream writable(destination.addRef());
    env.js.runMicrotasks();

    auto pipe = source.pipeTo(env.js, writable).catch_(env.js, [](jsg::Lock&, jsg::Value) {});
    KJ_ASSERT(state->readPending);
    KJ_ASSERT_NONNULL(sinkController)->error(env.js, env.js.typeError("destination failed"_kj));
    KJ_ASSERT(state->sourceCanceled);
    KJ_ASSERT(!state->destroyedDuringRead);

    read.fulfiller->fulfill(1);
    auto pipeDone = env.context.awaitJs(env.js, kj::mv(pipe));
    return destroyed.promise.then([pipeDone = kj::mv(pipeDone), state = state.addRef()]() mutable {
      KJ_ASSERT(state->sourceDestroyed && !state->readPending && !state->destroyedDuringRead);
      return kj::mv(pipeDone);
    });
  });
}

}  // namespace
}  // namespace workerd::api
