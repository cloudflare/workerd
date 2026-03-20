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

// ======================================================================================
// Shared test helpers

// Simple source that returns EOF immediately
class EofSource final: public ReadableStreamSource {
 public:
  kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
    return static_cast<size_t>(0);
  }
};

// Simple sink that accepts all writes
class NoopSink final: public WritableStreamSink {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const byte>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }
  void abort(kj::Exception) override {}
};

// Creates a TestFixture with common flags for stream tests
TestFixture makeStreamTestFixture() {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  return TestFixture({.featureFlags = flags.asReader()});
}

// Creates a TestFixture with the abortClearsQueue flag for testing abort behavior
TestFixture makeAbortClearsQueueTestFixture() {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  flags.setInternalWritableStreamAbortClearsQueue(true);
  return TestFixture({.featureFlags = flags.asReader()});
}

// Creates a BYOB-capable ReadableStream
jsg::Ref<ReadableStream> makeByteStream(jsg::Lock& js) {
  auto rs = js.alloc<ReadableStream>(newReadableStreamJsController());
  rs->getController().setup(
      js, UnderlyingSource{.type = kj::str("bytes")}, StreamQueuingStrategy{});
  return rs;
}

// ======================================================================================
// ReadableStreamSource test implementations

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

  // In this first case, the stream does not report a length. The read size
  // is min(limit, DEFAULT_BUFFER_CHUNK) = min(10001, 131072) = 10001, so the
  // entire stream is consumed in a single read that returns a short read (10000 < 10001).
  FooStream<10000> stream;

  stream.readAllBytes(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 1);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10001);
}

KJ_TEST("test (text)") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // In this first case, the stream does not report a length. The read size
  // is min(limit, DEFAULT_BUFFER_CHUNK) = min(10001, 131072) = 10001, so the
  // entire stream is consumed in a single read that returns a short read (10000 < 10001).
  FooStream<10000> stream;

  stream.readAllText(10001)
      .then([&](auto bytes) {
    KJ_ASSERT(bytes.size() == 10000);
    KJ_ASSERT(bytes.asBytes() == stream.buf().first(10000));
  }).wait(waitScope);

  KJ_ASSERT(stream.numreads() == 1);
  KJ_ASSERT(stream.maxMaxBytesSeen() == 10001);
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
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Make sure that while an internal sink is being piped into, no other writes are
    // allowed to be queued.

    jsg::Ref<ReadableStream> source = ReadableStream::constructor(env.js, kj::none, kj::none);
    jsg::Ref<WritableStream> sink =
        env.js.alloc<WritableStream>(env.context, kj::heap<NoopSink>(), kj::none);

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

KJ_TEST("WritableStreamInternalController operations reject when piped to") {
  // Tests that close/flush/tryPipeFrom reject with "currently being piped to"
  // during an active pipe operation.
  auto fixture = makeStreamTestFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto source = ReadableStream::constructor(env.js, kj::none, kj::none);
    auto source2 = ReadableStream::constructor(env.js, kj::none, kj::none);
    auto sink = env.js.alloc<WritableStream>(env.context, kj::heap<NoopSink>(), kj::none);

    auto pipeTo = source->pipeTo(env.js, sink.addRef(), PipeToOptions{.preventClose = true});
    KJ_ASSERT(sink->isLocked());

    constexpr auto expectedError =
        "jsg.TypeError: This WritableStream is currently being piped to."_kj;

    auto expectReject = [&](jsg::Promise<void> promise, bool& flag) {
      promise.catch_(env.js, [&](jsg::Lock& js, jsg::Value value) {
        flag = true;
        KJ_ASSERT(js.exceptionToKj(kj::mv(value)).getDescription() == expectedError);
      });
    };

    bool closeFailed = false, flushFailed = false, pipeFailed = false;

    expectReject(sink->getController().close(env.js), closeFailed);
    expectReject(sink->getController().flush(env.js), flushFailed);

    KJ_IF_SOME(secondPipe,
        sink->getController().tryPipeFrom(
            env.js, source2.addRef(), PipeToOptions{.preventClose = true})) {
      expectReject(kj::mv(secondPipe), pipeFailed);
    } else {
      KJ_FAIL_ASSERT("Expected tryPipeFrom to return a promise");
    }

    source->getController().cancel(env.js, kj::none);
    env.js.runMicrotasks();

    KJ_ASSERT(closeFailed);
    KJ_ASSERT(flushFailed);
    KJ_ASSERT(pipeFailed);
  });
}

KJ_TEST("WritableStreamInternalController observability") {
  auto fixture = makeStreamTestFixture();

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
    stream = env.js.alloc<WritableStream>(env.context, kj::heap<NoopSink>(), kj::mv(myObserver));

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
// The fix ensures the Pipe::State is ref-counted and survives until all callbacks complete.
KJ_TEST("WritableStreamInternalController pipeLoop abort during pending read") {
  auto fixture = makeAbortClearsQueueTestFixture();
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
        env.js.alloc<WritableStream>(env.context, kj::heap<NoopSink>(), kj::none);

    auto pipeTo = source->pipeTo(env.js, sink.addRef(), PipeToOptions{});
    pipeTo.markAsHandled(env.js);
    env.js.runMicrotasks();

    // Abort while pipeLoop is waiting for a pending read
    auto abortPromise = sink->getController().abort(env.js, env.js.v8TypeError("Test abort"_kj));
    abortPromise.markAsHandled(env.js);
    env.js.runMicrotasks();

    // If we get here without crashing, the test passes
    KJ_ASSERT(pullCount >= 1);
  });
}

// ======================================================================================
// DrainingReader tests for internal streams
//
// The internal stream implementation's drainingRead() behaves like a normal read() -
// it returns at most one chunk at a time rather than draining all buffered data.
// This is because internal streams are backed by kj I/O which is inherently async
// and doesn't have internal JS-side buffering.

KJ_TEST("DrainingReader basic creation and locking (internal stream)") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<EofSource>());
    KJ_ASSERT(!rs->isLocked());

    KJ_IF_SOME(reader, DrainingReader::create(env.js, *rs)) {
      KJ_ASSERT(rs->isLocked());
      KJ_ASSERT(reader->isAttached());

      reader->releaseLock(env.js);
      KJ_ASSERT(!rs->isLocked());
      KJ_ASSERT(!reader->isAttached());
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader cannot be created on locked internal stream") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<EofSource>());

    KJ_IF_SOME(reader1, DrainingReader::create(env.js, *rs)) {
      KJ_ASSERT(rs->isLocked());
      KJ_ASSERT(DrainingReader::create(env.js, *rs) == kj::none);
      reader1->releaseLock(env.js);
    } else {
      KJ_FAIL_ASSERT("Failed to create first DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader read after releaseLock rejects (internal stream)") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<EofSource>());

    KJ_IF_SOME(reader, DrainingReader::create(env.js, *rs)) {
      reader->releaseLock(env.js);

      bool rejected = false;
      reader->read(env.js).catch_(env.js, [&](jsg::Lock&, jsg::Value) -> DrainingReadResult {
        rejected = true;
        return {.done = true};
      });
      env.js.runMicrotasks();
      KJ_ASSERT(rejected);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader with maxRead parameter (internal stream)") {
  // Test that the maxRead parameter is respected for internal streams
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);

  TestFixture fixture({.featureFlags = flags.asReader()});

  bool testCompleted = false;
  size_t lastMaxBytes = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    class TestSource final: public ReadableStreamSource {
     public:
      explicit TestSource(size_t& maxBytesOut): lastMaxBytesOut(maxBytesOut) {}
      kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
        readCount++;
        // Note: maxBytes should be limited by the maxRead parameter
        lastMaxBytesOut = maxBytes;
        if (readCount == 1) {
          // Return less than maxBytes
          auto toWrite = kj::min(maxBytes, static_cast<size_t>(100));
          memset(buffer, 'x', toWrite);
          return toWrite;
        }
        return static_cast<size_t>(0);  // EOF
      }
      uint readCount = 0;
      size_t& lastMaxBytesOut;
    };

    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<TestSource>(lastMaxBytes));

    auto maybeReader = DrainingReader::create(env.js, *rs);
    KJ_ASSERT(maybeReader != kj::none, "Failed to create DrainingReader");
    auto reader = kj::mv(KJ_ASSERT_NONNULL(maybeReader));

    // Pass a small maxRead value
    auto readPromise = reader->read(env.js, 50);

    return env.context.awaitJs(env.js,
        kj::mv(readPromise)
            .then(env.js,
                [&testCompleted, reader = kj::mv(reader)](
                    jsg::Lock& js, DrainingReadResult&& result) mutable {
      KJ_ASSERT(result.chunks.size() == 1);
      // The internal implementation uses maxRead to allocate the buffer
      KJ_ASSERT(result.chunks[0].size() <= 50);
      KJ_ASSERT(!result.done);
      reader->releaseLock(js);
      testCompleted = true;
    }));
  });

  KJ_ASSERT(testCompleted);
  // Verify maxBytes was limited
  KJ_ASSERT(lastMaxBytes == 50);
}

KJ_TEST("DrainingReader with maxRead = 0 (internal stream)") {
  // Test that the maxRead = 0 parameter is respected for internal streams
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);

  TestFixture fixture({.featureFlags = flags.asReader()});

  bool testCompleted = false;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    class TestSource final: public ReadableStreamSource {
     public:
      explicit TestSource() = default;
      kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
        KJ_FAIL_ASSERT("tryRead should not be called when maxRead = 0");
      }
    };

    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<TestSource>());

    auto maybeReader = DrainingReader::create(env.js, *rs);
    KJ_ASSERT(maybeReader != kj::none, "Failed to create DrainingReader");
    auto reader = kj::mv(KJ_ASSERT_NONNULL(maybeReader));

    auto readPromise = reader->read(env.js, 0);

    return env.context.awaitJs(env.js,
        kj::mv(readPromise)
            .then(env.js,
                [&testCompleted, reader = kj::mv(reader)](
                    jsg::Lock& js, DrainingReadResult&& result) mutable {
      KJ_ASSERT(result.chunks.size() == 0);
      KJ_ASSERT(!result.done);
      reader->releaseLock(js);
      testCompleted = true;
    }));
  });

  KJ_ASSERT(testCompleted);
}

KJ_TEST("DrainingReader on stream with pending closure (internal stream)") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<EofSource>());
    rs->getController().setPendingClosure();

    KJ_IF_SOME(reader, DrainingReader::create(env.js, *rs)) {
      bool rejected = false;
      reader->read(env.js).catch_(
          env.js, [&](jsg::Lock& js, jsg::Value reason) -> DrainingReadResult {
        rejected = true;
        auto msg = kj::str(reason.getHandle(js));
        KJ_ASSERT(msg.contains("closing"), msg);
        return {.done = true};
      });
      env.js.runMicrotasks();
      KJ_ASSERT(rejected);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader on closed stream (internal stream)") {
  auto fixture = makeStreamTestFixture();

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<EofSource>());
    rs->getController().cancel(env.js, kj::none);

    KJ_IF_SOME(reader, DrainingReader::create(env.js, *rs)) {
      bool done = false;
      reader->read(env.js).then(env.js, [&](jsg::Lock&, DrainingReadResult&& result) {
        done = true;
        KJ_ASSERT(result.done);
        KJ_ASSERT(result.chunks.size() == 0);
      });
      env.js.runMicrotasks();
      KJ_ASSERT(done);
    } else {
      KJ_FAIL_ASSERT("Failed to create DrainingReader");
    }
  });
}

KJ_TEST("DrainingReader error propagation (internal stream)") {
  // Tests that I/O errors from tryRead are propagated and put the stream in errored state.
  auto fixture = makeStreamTestFixture();

  bool testCompleted = false;
  KJ_EXPECT_LOG(ERROR, "Simulated I/O error");

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    class TestSource final: public ReadableStreamSource {
     public:
      kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
        return KJ_EXCEPTION(FAILED, "Simulated I/O error");
      }
    };

    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<TestSource>());
    auto maybeReader = DrainingReader::create(env.js, *rs);
    auto reader = kj::mv(KJ_ASSERT_NONNULL(maybeReader));

    return env.context.awaitJs(env.js,
        reader->read(env.js)
            .catch_(env.js,
                [&, reader = kj::mv(reader)](
                    jsg::Lock& js, jsg::Value) mutable -> DrainingReadResult {
      reader->releaseLock(js);
      testCompleted = true;
      return {.done = true};
    }).then(env.js, [](jsg::Lock&, DrainingReadResult&&) {}));
  });

  KJ_ASSERT(testCompleted);
}

KJ_TEST("DrainingReader concurrent read rejection (internal stream)") {
  auto fixture = makeStreamTestFixture();
  bool testCompleted = false;

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto paf = kj::newPromiseAndFulfiller<size_t>();

    class TestSource final: public ReadableStreamSource {
     public:
      explicit TestSource(kj::Promise<size_t> p): promise(kj::mv(p)) {}
      kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
        return kj::mv(promise);
      }
      kj::Promise<size_t> promise;
    };

    auto rs = env.js.alloc<ReadableStream>(env.context, kj::heap<TestSource>(kj::mv(paf.promise)));
    auto maybeReader = DrainingReader::create(env.js, *rs);
    auto reader = kj::mv(KJ_ASSERT_NONNULL(maybeReader));

    auto firstRead = reader->read(env.js);

    // Second read while first is pending should reject synchronously
    bool rejected = false;
    reader->read(env.js).catch_(
        env.js, [&](jsg::Lock& js, jsg::Value reason) -> DrainingReadResult {
      rejected = true;
      auto msg = kj::str(reason.getHandle(js));
      KJ_ASSERT(msg.contains("single pending read"), msg);
      return {.done = true};
    });
    env.js.runMicrotasks();
    KJ_ASSERT(rejected);

    paf.fulfiller->fulfill(0);  // Complete first read with EOF

    return env.context.awaitJs(env.js,
        kj::mv(firstRead).then(
            env.js, [&, reader = kj::mv(reader)](jsg::Lock& js, DrainingReadResult&&) mutable {
      reader->releaseLock(js);
      testCompleted = true;
    }));
  });

  KJ_ASSERT(testCompleted);
}

// ======================================================================================
// ReadableStreamBYOBReader validation tests

KJ_TEST("ReadableStreamBYOBReader rejects read with zero-sized buffer") {
  KJ_EXPECT_LOG(ERROR, "read() on a BYOB reader requires a positive-sized TypedArray");
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = makeByteStream(env.js);
    auto reader = ReadableStreamBYOBReader::constructor(env.js, rs.addRef());

    auto buffer = v8::ArrayBuffer::New(env.js.v8Isolate, 0);
    auto view = v8::Uint8Array::New(buffer, 0, 0);

    bool rejected = false;
    reader->read(env.js, view, kj::none)
        .catch_(env.js, [&](jsg::Lock& js, jsg::Value reason) -> ReadResult {
      rejected = true;
      auto ex = js.exceptionToKj(kj::mv(reason));
      KJ_ASSERT(ex.getDescription().contains(
                    "read() on a BYOB reader requires a positive-sized TypedArray"),
          ex);
      return {.done = true};
    });
    env.js.runMicrotasks();
    KJ_ASSERT(rejected, "Expected read() to reject with zero-sized buffer");
  });
}

KJ_TEST("ReadableStreamBYOBReader rejects read with atLeast=0") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = makeByteStream(env.js);
    auto reader = ReadableStreamBYOBReader::constructor(env.js, rs.addRef());

    auto buffer = v8::ArrayBuffer::New(env.js.v8Isolate, 10);
    auto view = v8::Uint8Array::New(buffer, 0, 10);

    bool rejected = false;
    reader->readAtLeast(env.js, 0, view)
        .catch_(env.js, [&](jsg::Lock& js, jsg::Value reason) -> ReadResult {
      rejected = true;
      auto ex = js.exceptionToKj(kj::mv(reason));
      KJ_ASSERT(
          ex.getDescription().contains("Requested invalid minimum number of bytes to read (0)"),
          ex);
      return {.done = true};
    });
    env.js.runMicrotasks();
    KJ_ASSERT(rejected, "Expected readAtLeast() to reject with atLeast=0");
  });
}

KJ_TEST("ReadableStreamBYOBReader rejects read when atLeast exceeds buffer size") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = makeByteStream(env.js);
    auto reader = ReadableStreamBYOBReader::constructor(env.js, rs.addRef());

    auto buffer = v8::ArrayBuffer::New(env.js.v8Isolate, 10);
    auto view = v8::Uint8Array::New(buffer, 0, 10);

    bool rejected = false;
    reader->readAtLeast(env.js, 20, view)
        .catch_(env.js, [&](jsg::Lock& js, jsg::Value reason) -> ReadResult {
      rejected = true;
      auto ex = js.exceptionToKj(kj::mv(reason));
      KJ_ASSERT(
          ex.getDescription().contains("Minimum bytes to read (20) exceeds size of buffer (10)"),
          ex);
      return {.done = true};
    });
    env.js.runMicrotasks();
    KJ_ASSERT(rejected, "Expected readAtLeast() to reject when atLeast exceeds buffer size");
  });
}

KJ_TEST("ReadableStreamBYOBReader rejects read after releaseLock") {
  auto fixture = makeStreamTestFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto rs = makeByteStream(env.js);
    auto reader = ReadableStreamBYOBReader::constructor(env.js, rs.addRef());
    reader->releaseLock(env.js);

    auto buffer = v8::ArrayBuffer::New(env.js.v8Isolate, 10);
    auto view = v8::Uint8Array::New(buffer, 0, 10);

    bool rejected = false;
    reader->read(env.js, view, kj::none)
        .catch_(env.js, [&](jsg::Lock& js, jsg::Value reason) -> ReadResult {
      rejected = true;
      auto ex = js.exceptionToKj(kj::mv(reason));
      KJ_ASSERT(ex.getDescription().contains("This ReadableStream reader has been released"), ex);
      return {.done = true};
    });
    env.js.runMicrotasks();
    KJ_ASSERT(rejected, "Expected read() to reject after releaseLock");
  });
}

}  // namespace
}  // namespace workerd::api
