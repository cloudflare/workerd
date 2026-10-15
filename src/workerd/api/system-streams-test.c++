// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "system-streams.h"

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/brotli.h>
#include <kj/compat/gzip.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

class CollectingAsyncOutputStream final: public kj::AsyncOutputStream {
 public:
  explicit CollectingAsyncOutputStream(kj::Vector<kj::byte>& output): output(output) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    output.addAll(buffer);
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      output.addAll(piece);
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

 private:
  kj::Vector<kj::byte>& output;
};

class RejectingAsyncOutputStream final: public kj::AsyncOutputStream {
 public:
  void rejectWrites() {
    rejecting = true;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte>) override {
    return write();
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>>) override {
    return write();
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

 private:
  kj::Promise<void> write() {
    return rejecting ? kj::Promise<void>(KJ_EXCEPTION(DISCONNECTED, "downstream write failed"))
                     : kj::Promise<void>(kj::READY_NOW);
  }

  bool rejecting = false;
};

template <typename Decompressor>
void expectDecodedPrefix(
    kj::ArrayPtr<const kj::byte> compressed, kj::ArrayPtr<const kj::byte> expected) {
  kj::ArrayInputStream input(compressed);
  Decompressor decompressor(input);
  auto decoded = kj::heapArray<kj::byte>(expected.size());
  KJ_ASSERT(decompressor.tryRead(decoded, decoded.size()) == decoded.size());
  KJ_EXPECT(decoded == expected);
}

template <typename Decompressor>
void testCompressionFlushesAfterWrites(StreamEncoding encoding) {
  TestFixture fixture;
  kj::Vector<kj::byte> compressed;

  fixture.runInIoContext(
      [&compressed, encoding](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto sink = newSystemStream(kj::heap<CollectingAsyncOutputStream>(compressed), encoding,
        env.context, FlushCompressionAfterWrite::YES);

    KJ_EXPECT(!sink->tryWriteSync("not written"_kjb));

    co_await sink->write("foo"_kjb);
    expectDecodedPrefix<Decompressor>(compressed.asPtr(), "foo"_kjb);

    kj::ArrayPtr<const kj::byte> pieces[] = {"bar"_kjb, "baz"_kjb};
    co_await sink->write(pieces);
    expectDecodedPrefix<Decompressor>(compressed.asPtr(), "foobarbaz"_kjb);

    co_await sink->end();
  });

  kj::ArrayInputStream input(compressed.asPtr());
  Decompressor decompressor(input);
  KJ_EXPECT(decompressor.readAllBytes() == "foobarbaz"_kjb);
}

void testCompressionPropagatesDownstreamFailure(StreamEncoding encoding) {
  TestFixture fixture;
  KJ_EXPECT_THROW_MESSAGE("downstream write failed",
      fixture.runInIoContext([encoding](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto output = kj::heap<RejectingAsyncOutputStream>();
    auto& outputRef = *output;
    auto sink =
        newSystemStream(kj::mv(output), encoding, env.context, FlushCompressionAfterWrite::YES);

    auto write = sink->write("foo"_kjb);
    outputRef.rejectWrites();
    co_await write;
  }));
}

template <typename Decompressor>
void testCompressionFlushLifecycle(StreamEncoding encoding) {
  TestFixture fixture;
  fixture.runInIoContext([encoding](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& context = env.context;
    constexpr size_t STREAM_COUNT = 32;
    constexpr size_t WRITES_PER_STREAM = 64;
    constexpr auto payload = "data: event\n\n"_kjb;

    for (size_t streamNumber = 0; streamNumber < STREAM_COUNT; ++streamNumber) {
      kj::Vector<kj::byte> compressed;
      auto sink = newSystemStream(kj::heap<CollectingAsyncOutputStream>(compressed), encoding,
          context, FlushCompressionAfterWrite::YES);

      for (size_t writeNumber = 0; writeNumber < WRITES_PER_STREAM; ++writeNumber) {
        co_await sink->write(payload);
      }
      co_await sink->end();

      kj::ArrayInputStream input(compressed.asPtr());
      Decompressor decompressor(input);
      auto decoded = decompressor.readAllBytes();
      KJ_ASSERT(decoded.size() == payload.size() * WRITES_PER_STREAM);
      for (size_t writeNumber = 0; writeNumber < WRITES_PER_STREAM; ++writeNumber) {
        KJ_EXPECT(decoded.asPtr().slice(
                      writeNumber * payload.size(), (writeNumber + 1) * payload.size()) == payload);
      }
    }
  });
}

KJ_TEST("EncodedAsyncOutputStream flushes gzip after each write") {
  testCompressionFlushesAfterWrites<kj::GzipInputStream>(StreamEncoding::GZIP);
}

KJ_TEST("EncodedAsyncOutputStream flushes brotli after each write") {
  testCompressionFlushesAfterWrites<kj::BrotliInputStream>(StreamEncoding::BROTLI);
}

KJ_TEST("EncodedAsyncOutputStream propagates downstream gzip failure") {
  testCompressionPropagatesDownstreamFailure(StreamEncoding::GZIP);
}

KJ_TEST("EncodedAsyncOutputStream propagates downstream brotli failure") {
  testCompressionPropagatesDownstreamFailure(StreamEncoding::BROTLI);
}

KJ_TEST("EncodedAsyncOutputStream survives repeated gzip flush lifecycles") {
  testCompressionFlushLifecycle<kj::GzipInputStream>(StreamEncoding::GZIP);
}

KJ_TEST("EncodedAsyncOutputStream survives repeated brotli flush lifecycles") {
  testCompressionFlushLifecycle<kj::BrotliInputStream>(StreamEncoding::BROTLI);
}

KJ_TEST("EncodedAsyncOutputStream preserves buffered compression by default") {
  TestFixture fixture;
  kj::Vector<kj::byte> compressed;

  fixture.runInIoContext([&compressed](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto sink = newSystemStream(
        kj::heap<CollectingAsyncOutputStream>(compressed), StreamEncoding::GZIP, env.context);

    co_await sink->write("foo"_kjb);
    auto sizeAfterFirstWrite = compressed.size();
    co_await sink->write("bar"_kjb);
    KJ_EXPECT(compressed.size() == sizeAfterFirstWrite);
    co_await sink->end();
  });

  kj::ArrayInputStream input(compressed.asPtr());
  kj::GzipInputStream decompressor(input);
  KJ_EXPECT(decompressor.readAllBytes() == "foobar"_kjb);
}

KJ_TEST("EncodedAsyncInputStream cancel with pending read on AsyncPipe") {
  // This test reproduces a use-after-free crash that occurred when:
  // 1. A read operation is started on an EncodedAsyncInputStream backed by an AsyncPipe
  // 2. The stream is cancelled (e.g., via Socket::close())
  // 3. The AsyncPipe is destroyed while the read is still pending
  //
  // Without the fix (kj::Canceler in EncodedAsyncInputStream), the BlockedRead destructor
  // would try to access the freed AsyncPipe, causing a use-after-free.

  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    // Create an in-memory pipe (AsyncPipe)
    auto pipe = kj::newTwoWayPipe();

    // Create an EncodedAsyncInputStream wrapping one end of the pipe
    kj::Own<kj::AsyncInputStream> inputStream = kj::mv(pipe.ends[0]);
    auto stream = newSystemStream(kj::mv(inputStream), StreamEncoding::IDENTITY, env.context);

    // Start a read operation - this will block because no data has been written to the pipe
    kj::byte buffer[100]{};
    auto readPromise = stream->tryRead(buffer, 1, sizeof(buffer));

    // Cancel the stream - this simulates what Socket::close() does
    stream->cancel(KJ_EXCEPTION(DISCONNECTED, "stream cancelled"));

    // Now destroy the other end of the pipe - this destroys the AsyncPipe
    // Without the fix, this would cause a use-after-free when the BlockedRead
    // destructor tries to access the freed pipe.
    pipe.ends[1] = nullptr;

    // The read promise should be cancelled - try to wait for it
    // It should reject with the cancellation exception
    return readPromise.then(
        [](size_t) { KJ_FAIL_ASSERT("read should have been cancelled"); }, [](kj::Exception&& e) {
      // Expected the read to be cancelled
    });
  });
}

}  // namespace
}  // namespace workerd::api
