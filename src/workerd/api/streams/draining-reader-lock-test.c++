// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A DrainingReader releases its stream's lock when it is destroyed, which updates state that the
// garbage collector traces, so the release must happen under the isolate lock. pumpTo() owns its
// DrainingReader in a coroutine frame on the request's thread, which may drop it without the lock
// while another thread holds the lock.
//
// The test drops a pump of a readable stream without the lock while another thread holds the
// lock and runs a full garbage collection, which traces the stream. It passes unless
// ThreadSanitizer reports a data race between the two: nothing orders them, so it reports any
// access the drop makes to state the collector reads or writes, whichever comes first. Other
// builds cannot detect the bug with this test.

#include "readable.h"
#include "standard.h"

#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <kj/thread.h>

// These tests exercise the legacy C++ streams implementation directly, so they allocate the
// legacy ReadableStream rather than going through the JsReadableStream implementation dispatch.
// NOLINTBEGIN(workerd-legacy-stream-alloc)

namespace workerd::api {
namespace {

kj::Own<TestFixture> makeJsStreamsFixture(capnp::MallocMessageBuilder& flagsBuilder) {
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  return kj::heap<TestFixture>(TestFixture::SetupParams{.featureFlags = featureFlags.asReader()});
}

// Creates a JavaScript-backed stream that a garbage collection traces, by giving it a wrapper.
// The stream's start() algorithm enqueues one chunk, and the stream stays readable after it.
jsg::Ref<ReadableStream> newTracedStream(jsg::Lock& js) {
  auto stream = ReadableStream::constructor(js,
      UnderlyingSource{
        .start =
            [](jsg::Lock& js, auto controller) {
    auto& c =
        KJ_REQUIRE_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
    c->enqueue(js, jsg::JsValue(v8::ArrayBuffer::New(js.v8Isolate, 10)));
    return js.resolvedPromise();
  },
        // Never enqueues, so a read after the first chunk stays pending.
        .pull = [](jsg::Lock& js, auto controller) { return js.resolvedPromise(); },
      },
      kj::none);

  v8::HandleScope handleScope(js.v8Isolate);
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStream>>());
  handler.wrap(js, stream.addRef());
  return stream;
}

// Starts a thread that takes the isolate lock, fulfills `locked`, and then runs a full garbage
// collection, which traces every stream with a wrapper.
kj::Own<kj::Thread> collectGarbageOnAnotherThread(
    TestFixture& fixture, kj::Own<kj::CrossThreadPromiseFulfiller<void>> locked) {
  return kj::heap<kj::Thread>([&fixture, locked = kj::mv(locked)]() mutable {
    fixture.enterWorkerLockSynchronously([&](Worker::Lock& lock) {
      locked->fulfill();
      lock.getIsolate()->RequestGarbageCollectionForTesting(
          v8::Isolate::kFullGarbageCollection, v8::StackState::kNoHeapPointers);
    });
  });
}

// Accepts every write, and calls `onFirstWrite` on the first one.
class SignalingSink final: public WritableStreamSink {
 public:
  explicit SignalingSink(kj::Function<void()> onFirstWrite): onFirstWrite(kj::mv(onFirstWrite)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    signal();
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    signal();
    return kj::READY_NOW;
  }

  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {}

 private:
  kj::Maybe<kj::Function<void()>> onFirstWrite;

  void signal() {
    KJ_IF_SOME(f, onFirstWrite) {
      f();
      onFirstWrite = kj::none;
    }
  }
};

KJ_TEST("ReadableStream pumpTo dropped after its request releases its reader under the lock") {
  // Once the IoContext that a pump runs in has no request left, nothing would run a cancel, so
  // cancelSourceOfDroppedPump() leaves the source alone and drops the reader where it is,
  // without the lock. The stream is still readable then.
  capnp::MallocMessageBuilder flagsBuilder;
  auto fixture = makeJsStreamsFixture(flagsBuilder);
  auto firstWrite = kj::newPromiseAndFulfiller<void>();
  kj::Maybe<kj::Promise<DeferredProxy<void>>> pump;

  // The test holds a reference to the IoContext, so that it outlives the request. It is destroyed
  // only once the other thread is done, as ~IoContext drops handles of its own without the lock.
  auto context = fixture->newIoContext();
  auto request = fixture->newIncomingRequest(*context);
  fixture->enterContext(*request, [&](const TestFixture::Environment& env) {
    auto stream = newTracedStream(env.js);
    auto sink = kj::heap<SignalingSink>([&]() { firstWrite.fulfiller->fulfill(); });
    pump = stream->pumpTo(env.js, kj::mv(sink), true);
  });
  firstWrite.promise.wait(fixture->getWaitScope());
  fixture->drainAndDestroy(kj::mv(request));

  auto locked = kj::newPromiseAndCrossThreadFulfiller<void>();
  auto otherThread = collectGarbageOnAnotherThread(*fixture, kj::mv(locked.fulfiller));
  locked.promise.wait(fixture->getWaitScope());
  pump = kj::none;
  otherThread = nullptr;

  context = nullptr;
  // Run whatever the drop deferred to the isolate lock.
  fixture->enterWorkerLock([](Worker::Lock&) {});
}

}  // namespace
}  // namespace workerd::api

// NOLINTEND(workerd-legacy-stream-alloc)
