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

class AbortTrackingAsyncOutputStream final: public kj::AsyncOutputStream {
 public:
  explicit AbortTrackingAsyncOutputStream(kj::Vector<kj::Exception>& aborts): aborts(aborts) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte>) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>>) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  void abortWrite(kj::Exception&& reason) override {
    aborts.add(kj::mv(reason));
  }

 private:
  kj::Vector<kj::Exception>& aborts;
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

void testOutputForwardsAbort(StreamEncoding encoding) {
  TestFixture fixture;
  fixture.runInIoContext([encoding](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& context = env.context;
    for (auto flushAfterWrite: {FlushCompressionAfterWrite::NO, FlushCompressionAfterWrite::YES}) {
      for (auto writesBeforeAbort: {0, 1}) {
        kj::Vector<kj::Exception> aborts;
        auto sink = newSystemStream(
            kj::heap<AbortTrackingAsyncOutputStream>(aborts), encoding, context, flushAfterWrite);

        if (writesBeforeAbort != 0) {
          co_await sink->write("data: event\n\n"_kjb);
        }
        sink->abort(KJ_EXCEPTION(FAILED, "response aborted"));

        KJ_ASSERT(aborts.size() == 1);
        KJ_EXPECT(aborts[0].getType() == kj::Exception::Type::FAILED);
        KJ_EXPECT(aborts[0].getDescription() == "response aborted");

        sink->abort(KJ_EXCEPTION(FAILED, "duplicate abort"));
        KJ_EXPECT(aborts.size() == 1);
      }
    }
  });
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

KJ_TEST("EncodedAsyncOutputStream forwards gzip aborts") {
  testOutputForwardsAbort(StreamEncoding::GZIP);
}

KJ_TEST("EncodedAsyncOutputStream forwards brotli aborts") {
  testOutputForwardsAbort(StreamEncoding::BROTLI);
}

KJ_TEST("EncodedAsyncOutputStream forwards identity aborts") {
  testOutputForwardsAbort(StreamEncoding::IDENTITY);
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

// Pumps "foobar" from a native source into a native sink through the deferred pump. The pump holds
// counted pointers to both ends, so with pointer counting enabled, destroying either end before
// the pump settles would fail an assertion.
kj::Promise<void> expectNativePump(
    IoContext& context, kj::Maybe<uint64_t> transportLength, uint64_t identityLength) {
  auto in = kj::newOneWayPipe(transportLength);
  auto out = kj::newOneWayPipe();
  auto source = newSystemStream(kj::mv(in.in), StreamEncoding::IDENTITY, context, identityLength);
  auto sink = newSystemStream(kj::mv(out.out), StreamEncoding::IDENTITY, context);
  kj::byte buffer[6]{};
  auto read = out.in->read(buffer, 6);

  KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == identityLength);
  auto proxy = co_await source->pumpTo(sink->getPtr(), true);
  KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == transportLength);
  co_await in.out->write("foobar"_kjb);
  co_await read;
  KJ_EXPECT(kj::arrayPtr(buffer) == "foobar"_kjb);
  in.out = nullptr;
  // The pump must settle while the sink's reader stays open without reading EOF.
  co_await proxy.proxyTask;
  KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});

  // The pump has settled, so both ends may now be destroyed.
  sink = nullptr;
  source = nullptr;
}

KJ_TEST("Native pumps consume identity length metadata independently of transport length") {
  TestFixture fixture;
  kj::Maybe<uint64_t> transportLengths[] = {kj::none, uint64_t{6}};
  for (auto transportLength: transportLengths) {
    for (uint64_t identityLength: {uint64_t{0}, uint64_t{5}, uint64_t{6}, uint64_t{7}}) {
      fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
        return expectNativePump(env.context, transportLength, identityLength);
      });
    }
  }
}

class BufferedInput final: public kj::AsyncInputStream {
 public:
  explicit BufferedInput(kj::ArrayPtr<const kj::byte> bytes): bytes(bytes) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return KJ_ASSERT_NONNULL(
        tryReadSync(kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes), minBytes));
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    auto amount = kj::min(buffer.size(), bytes.size());
    buffer.write(bytes.first(amount));
    bytes = bytes.slice(amount);
    return amount;
  }

 private:
  kj::ArrayPtr<const kj::byte> bytes;
};

kj::Array<kj::byte> encode(kj::ArrayPtr<const kj::byte> bytes, StreamEncoding encoding) {
  kj::VectorOutputStream output;
  if (encoding == StreamEncoding::GZIP) {
    kj::GzipOutputStream compressor(output);
    compressor.write(bytes);
  } else if (encoding == StreamEncoding::BROTLI) {
    kj::BrotliOutputStream compressor(output);
    compressor.write(bytes);
  } else {
    output.write(bytes);
  }
  return kj::heapArray(output.getArray());
}

KJ_TEST("System stream identity length tracks decoded reads and independent tee branches") {
  TestFixture fixture;
  for (auto encoding: {StreamEncoding::IDENTITY, StreamEncoding::GZIP, StreamEncoding::BROTLI}) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto bytes = encode("hello world"_kjb, encoding);
      auto source =
          newSystemStream(kj::heap<BufferedInput>(bytes), encoding, env.context, uint64_t{11});
      KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{11});
      if (encoding != StreamEncoding::IDENTITY) {
        // Identity metadata is never a compressed-byte length.
        KJ_EXPECT(source->tryGetLength(encoding) == kj::none);
      }
      kj::byte buffer[3]{};
      KJ_IF_SOME(amount, source->tryReadSync(buffer, 3)) {
        KJ_EXPECT(amount == 3);
      } else {
        KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{11});
        KJ_EXPECT(co_await source->tryRead(buffer, 3, 3) == 3);
      }
      KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{8});
      auto tee = KJ_ASSERT_NONNULL(source->tryTee(kj::maxValue));
      for (auto& branch: tee.branches) {
        KJ_EXPECT(branch->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{8});
      }
      KJ_EXPECT(co_await tee.branches[0]->readAllText(100) == "lo world");
      KJ_EXPECT(tee.branches[0]->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});
      KJ_EXPECT(tee.branches[1]->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{8});
      KJ_EXPECT(co_await tee.branches[1]->readAllText(100) == "lo world");
      KJ_EXPECT(tee.branches[1]->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});
    });
  }
}

KJ_TEST("System stream identity length survives declined synchronous reads and async reads") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto pipe = kj::newOneWayPipe();
    auto source =
        newSystemStream(kj::mv(pipe.in), StreamEncoding::IDENTITY, env.context, uint64_t{6});
    kj::byte buffer[3]{};
    KJ_EXPECT(source->tryReadSync(buffer, 3) == kj::none);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{6});
    auto read = source->tryRead(buffer, 3, 3);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{6});
    auto write = pipe.out->write("foobar"_kjb);
    KJ_EXPECT(co_await read == 3);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{3});
    KJ_EXPECT(co_await source->tryRead(buffer, 3, 3) == 3);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});
    co_await write;
    pipe.out = nullptr;
    KJ_EXPECT(co_await source->tryRead(buffer, 1, 3) == 0);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});
  });
}

KJ_TEST("System streams without identity metadata retain transport lengths and decoding behavior") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto pipe = kj::newOneWayPipe(uint64_t{6});
    auto source = newSystemStream(kj::mv(pipe.in), StreamEncoding::IDENTITY, env.context);
    KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{6});
    auto bytes = encode("foobar"_kjb, StreamEncoding::GZIP);
    auto compressed = kj::newOneWayPipe(bytes.size());
    auto decoded = newSystemStream(kj::mv(compressed.in), StreamEncoding::GZIP, env.context);
    KJ_EXPECT(decoded->tryGetLength(StreamEncoding::GZIP) == bytes.size());
    KJ_EXPECT(decoded->tryGetLength(StreamEncoding::IDENTITY) == kj::none);
    auto write = compressed.out->write(bytes);
    auto read = decoded->readAllText(100);
    co_await write;
    compressed.out = nullptr;
    KJ_EXPECT(co_await read == "foobar");
  });
}

KJ_TEST("System stream identity length becomes unknown on overrun and zero at early EOF") {
  TestFixture fixture;
  for (uint64_t length: {uint64_t{0}, uint64_t{2}, uint64_t{4}}) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto source = newSystemStream(
          kj::heap<BufferedInput>("foo"_kjb), StreamEncoding::IDENTITY, env.context, length);
      KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == length);
      kj::byte buffer[3]{};
      KJ_EXPECT(KJ_ASSERT_NONNULL(source->tryReadSync(buffer, 3)) == 3);
      KJ_EXPECT(kj::arrayPtr(buffer) == "foo"_kjb);
      if (length < 3) {
        KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == kj::none);
      } else {
        KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{1});
      }
      KJ_EXPECT(co_await source->tryRead(buffer, 1, 3) == 0);
      if (length < 3) {
        KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == kj::none);
      } else {
        KJ_EXPECT(source->tryGetLength(StreamEncoding::IDENTITY) == uint64_t{0});
      }
    });
  }
}

}  // namespace
}  // namespace workerd::api
