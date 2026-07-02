// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "system-streams.h"

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>
#include <zstd.h>

namespace workerd::api {
namespace {

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
    kj::byte buffer[100];
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

KJ_TEST("ZstdAsyncInputStream decompresses correctly") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    constexpr kj::StringPtr plaintext = "hello zstd"_kj;

    // Compress synchronously using the zstd one-shot API.
    size_t bound = ZSTD_compressBound(plaintext.size());
    auto compressed = kj::heapArray<kj::byte>(bound);
    size_t compressedSize = ZSTD_compress(compressed.begin(), compressed.size(),
        plaintext.begin(), plaintext.size(), ZSTD_CLEVEL_DEFAULT);
    KJ_REQUIRE(!ZSTD_isError(compressedSize), ZSTD_getErrorName(compressedSize));

    // Wrap the compressed bytes in a synchronous AsyncInputStream.
    struct ArrayStream: kj::AsyncInputStream {
      kj::Array<kj::byte> data;
      size_t pos = 0;
      ArrayStream(kj::Array<kj::byte> d): data(kj::mv(d)) {}
      virtual ~ArrayStream() = default;
      kj::Promise<size_t> tryRead(void* buf, size_t, size_t max) override {
        size_t n = kj::min(max, data.size() - pos);
        memcpy(buf, data.begin() + pos, n);
        pos += n;
        return n;
      }
    };
    auto inner = kj::heap<ArrayStream>(kj::heapArray(compressed.slice(0, compressedSize)));

    // Decode through a ZSTD-encoded system stream.
    auto stream = newSystemStream(kj::mv(inner), StreamEncoding::ZSTD, env.context);
    auto outBuf = kj::heapArray<kj::byte>(64);
    size_t expectedSize = plaintext.size();
    return stream->tryRead(outBuf.begin(), expectedSize, outBuf.size())
        .then([outBuf = kj::mv(outBuf), stream = kj::mv(stream), expectedSize](size_t n) {
          KJ_EXPECT(n == expectedSize);
          KJ_EXPECT(
              kj::StringPtr(reinterpret_cast<const char*>(outBuf.begin()), n) == "hello zstd"_kj);
        });
  });
}

}  // namespace
}  // namespace workerd::api
