// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "system-streams.h"

#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

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

// Pumps "foobar" from a native source into a native sink through the deferred pump. The pump holds
// counted pointers to both ends, so with pointer counting enabled, destroying either end before
// the pump settles would fail an assertion.
kj::Promise<void> expectNativePump(IoContext& context, kj::Maybe<uint64_t> expectedLength) {
  auto in = kj::newOneWayPipe(expectedLength);
  auto out = kj::newOneWayPipe();
  auto source = newSystemStream(kj::mv(in.in), StreamEncoding::IDENTITY, context);
  auto sink = newSystemStream(kj::mv(out.out), StreamEncoding::IDENTITY, context);
  auto received = out.in->readAllText();

  auto proxy = co_await source->pumpTo(sink->getPtr(), true);
  co_await in.out->write("foobar"_kjb);
  in.out = nullptr;
  co_await proxy.proxyTask;

  // The pump has settled, so both ends may now be destroyed.
  sink = nullptr;
  source = nullptr;
  KJ_EXPECT(co_await received == "foobar");
}

KJ_TEST("Native pump with a known length delivers all bytes") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    return expectNativePump(env.context, static_cast<uint64_t>(6));
  });
}

KJ_TEST("Native pump without a known length delivers all bytes") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    return expectNativePump(env.context, kj::none);
  });
}

}  // namespace
}  // namespace workerd::api
