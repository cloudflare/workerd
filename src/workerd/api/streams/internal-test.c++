// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "internal.h"
#include "readable.h"
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
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
    maxMaxBytesSeen_ = kj::max(maxMaxBytesSeen_, maxBytes);
    numreads_++;
    if (remaining_ == 0) return (size_t)0;
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
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
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
    kj::Promise<size_t> tryRead(void*, size_t, size_t) {
      return (size_t)0;
    }
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)0;
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
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)10;
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
    kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) {
      return (size_t)100;
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
    virtual void onChunkEnqueued(size_t bytes) {
      ++queueSize;
      queueSizeBytes += bytes;
    };
    virtual void onChunkDequeued(size_t bytes) {
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

}  // namespace
}  // namespace workerd::api
