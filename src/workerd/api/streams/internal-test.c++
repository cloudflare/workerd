// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "internal.h"
#include "readable.h"
#include "standard.h"
#include "writable.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <openssl/rand.h>

namespace workerd::api {
namespace {

template <int size>
class FooStream: public ReadableStreamSource {
 public:
  FooStream(): ptr(&data[0]), remaining_(size) {
    KJ_ASSERT(RAND_bytes(data, size) == 1);
  }
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    maxMaxBytesSeen_ = kj::max(maxMaxBytesSeen_, maxBytes);
    numreads_++;
    if (remaining_ == 0) return static_cast<size_t>(0);
    KJ_ASSERT(minBytes == maxBytes);
    KJ_ASSERT(maxBytes <= size);
    auto amount = kj::min(remaining_, maxBytes);
    memcpy(buffer, ptr, amount);
    ptr += amount;
    remaining_ -= amount;
    return amount;
  }

  kj::ArrayPtr<uint8_t> buf() {
    return data;
  }

  size_t remaining() {
    return remaining_;
  }

  size_t numreads() {
    return numreads_;
  }

  size_t maxMaxBytesSeen() {
    return maxMaxBytesSeen_;
  }

 private:
  uint8_t data[size];
  uint8_t* ptr;
  size_t remaining_;
  size_t numreads_ = 0;
  size_t maxMaxBytesSeen_ = 0;
};

template <int size>
class BarStream: public FooStream<size> {
 public:
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    return size;
  }
};

KJ_TEST("test") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this first case, the stream does not report a length. The maximum
  // number of reads should be 3, and each allocation should be 4096
  FooStream<10000> stream;

  stream.readAllBytes(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 3);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 4096);
}

KJ_TEST("test (text)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this first case, the stream does not report a length. The maximum
  // number of reads should be 3, and each allocation should be 4096
  FooStream<10000> stream;

  stream.readAllText(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes.asBytes() == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 3);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 4096);
}

KJ_TEST("test2") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this second case, the stream does report a size, so we should see
  // only one read.
  BarStream<10000> stream;

  stream.readAllBytes(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10000);
}

KJ_TEST("test2 (text)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this second case, the stream does report a size, so we should see
  // only one read.
  BarStream<10000> stream;

  stream.readAllText(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes.asBytes() == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10000);
}

KJ_TEST("zero-length stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class Zero: public ReadableStreamSource {
   public:
    kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
      return static_cast<size_t>(0);
    }
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
      return static_cast<size_t>(0);
    }
  };

  Zero zero;
  zero.readAllBytes(10).then([&](kj::Array<kj::byte> bytes) {
    KJ_ASSERT(bytes.size() == 0);
  }).wait(waitScope);
}

KJ_TEST("lying stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class Dishonest: public FooStream<10000> {
   public:
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
      return static_cast<size_t>(10);
    }
  };

  Dishonest stream;
  stream.readAllBytes(10001)
      .then([&](kj::Array<kj::byte> bytes) {
    // The stream lies! it says there are only 10 bytes but there are more.
    // oh well, we at least make sure we get the right result.
    KJ_ASSERT(bytes.size() == 10000);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 1001);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10);
}

KJ_TEST("honest small stream") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class HonestSmall: public FooStream<100> {
   public:
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
      return static_cast<size_t>(100);
    }
  };

  HonestSmall stream;
  stream.readAllBytes(1001).then([&](kj::Array<kj::byte> bytes) {
    KJ_ASSERT(bytes.size() == 100);
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 2);
  KJ_ASSERT(stream.maxMaxBytesSeen(), 100);
}

KJ_TEST("WritableStreamInternalController queue size assertion") {

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);
  flags.setStreamsJavaScriptControllers(true);

  TestFixture fixture({.featureFlags = flags.asReader()});

  class MySink final: public WritableStreamSink {
   public:
    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> end() override {
      return kj::READY_NOW;
    }
    void abort(kj::Exception reason) override {}
  };

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Make sure that while an internal sink is being piped into, no other writes are
    // allowed to be queued.

    jsg::Ref<ReadableStream> source = ReadableStream::constructor(env.js, kj::none, kj::none);
    jsg::Ref<WritableStream> sink =
        env.js.alloc<WritableStream>(env.context, kj::heap<MySink>(), kj::none);

    auto pipeTo = source->pipeTo(env.js, sink.addRef(), PipeToOptions{.preventClose = true});

    KJ_ASSERT(sink->isLocked());
    try {
      sink->getWriter(env.js);
      KJ_FAIL_ASSERT("Expected getWriter to throw");
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription() ==
          "expected !stream->isLocked(); jsg.TypeError: This WritableStream "
          "is currently locked to a writer.");
    }

    auto buffersource = env.js.bytes(kj::heapArray<kj::byte>(10));

    bool writeFailed = false;

    auto write = sink->getController()
                     .write(env.js, buffersource.getHandle(env.js))
                     .catch_(env.js, [&](jsg::Lock& js, jsg::Value value) {
      writeFailed = true;
      auto ex = js.exceptionToKj(kj::mv(value));
      KJ_ASSERT(
          ex.getDescription() == "jsg.TypeError: This WritableStream is currently being piped to.");
    });

    source->getController().cancel(env.js, kj::none);

    env.js.runMicrotasks();

    KJ_ASSERT(!sink->isLocked());
    KJ_ASSERT(!sink->getController().isClosedOrClosing());
    KJ_ASSERT(!sink->getController().isErrored());
    KJ_ASSERT(sink->getController().isErroring(env.js) == kj::none);

    // Getting a writer at this point does not throw...
    sink->getWriter(env.js);
  });
}

KJ_TEST("WritableStreamInternalController observability") {

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);
  flags.setStreamsJavaScriptControllers(true);

  TestFixture::SetupParams params;
  TestFixture fixture({.featureFlags = flags.asReader()});

  class MySink final: public WritableStreamSink {
   public:
    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      ++writeCount;
      return kj::READY_NOW;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> end() override {
      return kj::READY_NOW;
    }
    void abort(kj::Exception reason) override {}
    uint getWriteCount() {
      return writeCount;
    }

   private:
    uint writeCount = 0;
  };

  class MyObserver final: public ByteStreamObserver {
   public:
    void onChunkEnqueued(size_t bytes) override {
      ++queueSize;
      queueSizeBytes += bytes;
    };
    void onChunkDequeued(size_t bytes) override {
      queueSizeBytes -= bytes;
      --queueSize;
    };
    uint64_t queueSize = 0;
    uint64_t queueSizeBytes = 0;
  };

  auto myObserver = kj::heap<MyObserver>();
  auto& observer = *myObserver;
  kj::Maybe<jsg::Ref<WritableStream>> stream;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    stream = env.js.alloc<WritableStream>(env.context, kj::heap<MySink>(), kj::mv(myObserver));

    auto write = [&](size_t size) {
      auto buffersource = env.js.bytes(kj::heapArray<kj::byte>(size));
      return env.context.awaitJs(env.js,
          KJ_ASSERT_NONNULL(stream)->getController().write(env.js, buffersource.getHandle(env.js)));
    };

    KJ_ASSERT(observer.queueSize == 0);
    KJ_ASSERT(observer.queueSizeBytes == 0);

    auto builder = kj::heapArrayBuilder<kj::Promise<void>>(2);
    builder.add(write(1));

    KJ_ASSERT(observer.queueSize == 1);
    KJ_ASSERT(observer.queueSizeBytes == 1);

    builder.add(write(10));

    KJ_ASSERT(observer.queueSize == 2);
    KJ_ASSERT(observer.queueSizeBytes == 11);

    return kj::joinPromises(builder.finish());
  });

  KJ_ASSERT(observer.queueSize == 0);
  KJ_ASSERT(observer.queueSizeBytes == 0);
}

// Test for use-after-free fix in pipeLoop when abort is called during pending read.
// This tests the scenario where:
// 1. A JavaScript-backed ReadableStream is piped to an internal WritableStream
// 2. The pipeLoop is waiting for a read from the JS stream
// 3. abort() is called on the writable stream, which triggers drain()
// 4. drain() destroys the Pipe object
// 5. The pending read callback must not access the freed Pipe
//
// The fix ensures the Pipe::State is ref-counted and survives until all callbacks complete.
KJ_TEST("WritableStreamInternalController pipeLoop abort during pending read") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setWorkerdExperimental(true);
  flags.setStreamsJavaScriptControllers(true);
  // Enable the flag that causes abort to call drain() immediately
  flags.setInternalWritableStreamAbortClearsQueue(true);

  TestFixture fixture({.featureFlags = flags.asReader()});

  class MySink final: public WritableStreamSink {
   public:
    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> end() override {
      return kj::READY_NOW;
    }
    void abort(kj::Exception reason) override {}
  };

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Create a JavaScript-backed ReadableStream.
    // The pull function will be called when the pipe tries to read.
    // We use a JS-backed stream so that pipeLoop is used (not the kj pipe path).
    //
    // We need to simulate:
    // 1. First read succeeds with some data
    // 2. Second read is pending (the promise from pull is not resolved)
    // 3. While pending, we abort the writable stream
    //
    // Using an UnderlyingSource with a pull callback that enqueues data once,
    // then on the second call returns without enqueuing (leaving the read pending).

    int pullCount = 0;
    jsg::Ref<ReadableStream> source = ReadableStream::constructor(env.js,
        UnderlyingSource{.pull =
                             [&pullCount](jsg::Lock& js, UnderlyingSource::Controller controller) {
      pullCount++;
      auto& c = KJ_ASSERT_NONNULL(controller.tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      if (pullCount == 1) {
        // First pull: enqueue some data so the pipe loop can make progress
        auto data = js.bytes(kj::heapArray<kj::byte>({1, 2, 3, 4}));
        c->enqueue(js, data.getHandle(js));
      }
      // Second pull onwards: don't enqueue anything, leaving the read pending.
      // This simulates an async data source that hasn't received data yet.
      // The promise returned by read() will be pending.
      return js.resolvedPromise();
    }},
        kj::none);

    jsg::Ref<WritableStream> sink =
        env.js.alloc<WritableStream>(env.context, kj::heap<MySink>(), kj::none);

    // Start the pipe. This will:
    // 1. Call pull() which enqueues data
    // 2. pipeLoop reads the data and writes it to the sink
    // 3. pipeLoop calls read() again, which calls pull()
    // 4. pull() returns without enqueuing, so read() returns a pending promise
    // 5. pipeLoop's callback is now waiting for that promise
    auto pipeTo = source->pipeTo(env.js, sink.addRef(), PipeToOptions{});
    pipeTo.markAsHandled(env.js);

    // Run microtasks to let the pipe make progress (first read/write cycle)
    env.js.runMicrotasks();

    // At this point, pipeLoop should be waiting for the second read.
    // Now abort the writable stream. This should:
    // 1. Call doAbort() which calls drain()
    // 2. drain() destroys the Pipe (setting state->aborted = true)
    // 3. The pending read callback should check aborted and bail out safely

    // Before the fix, this would cause a use-after-free when the pending callback
    // tried to access the freed Pipe.
    auto abortPromise = sink->getController().abort(env.js, env.js.v8TypeError("Test abort"_kj));
    abortPromise.markAsHandled(env.js);

    // Run microtasks to process the abort and any pending callbacks
    env.js.runMicrotasks();

    // If we get here without crashing, the test passes.
    // The fix ensures that the Pipe::State survives until all callbacks complete.
    KJ_ASSERT(pullCount >= 1);  // Verify pull was called at least once
  });
}

}  // namespace
}  // namespace workerd::api
