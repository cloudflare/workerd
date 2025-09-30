#include "readable-source.h"

#include <workerd/api/streams/writable-sink.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>

#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd::api::streams {
namespace {

// Mock WritableStreamSink for testing pumpTo functionality
class MockWritableStreamSink: public WritableStreamSink {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    writeCallCount++;
    totalBytesWritten += buffer.size();
    writtenData.addAll(buffer);

    if (shouldFailWrite) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    multiWriteCallCount++;
    for (auto piece: pieces) {
      totalBytesWritten += piece.size();
      writtenData.addAll(piece);
    }

    if (shouldFailWrite) {
      return KJ_EXCEPTION(FAILED, "Mock multi-write failure");
    }

    return kj::READY_NOW;
  }

  kj::Promise<void> end() override {
    endCallCount++;
    isEnded = true;

    if (shouldFailEnd) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    co_return;
  }

  void abort(kj::Exception reason) override {
    abortCallCount++;
    isAborted = true;
    abortReason = kj::mv(reason);
  }

  rpc::StreamEncoding disownEncodingResponsibility() override {
    disownCallCount++;
    return encoding;
  }

  rpc::StreamEncoding getEncoding() override {
    return encoding;
  }

  // Test state
  uint32_t writeCallCount = 0;
  uint32_t multiWriteCallCount = 0;
  uint32_t endCallCount = 0;
  uint32_t abortCallCount = 0;
  uint32_t disownCallCount = 0;

  size_t totalBytesWritten = 0;
  bool isEnded = false;
  bool isAborted = false;
  kj::Maybe<kj::Exception> abortReason;

  kj::Vector<kj::byte> writtenData;
  rpc::StreamEncoding encoding = rpc::StreamEncoding::IDENTITY;

  // Control behavior
  bool shouldFailWrite = false;
  bool shouldFailEnd = false;
};

// Memory-based AsyncInputStream for factory function tests
class MemoryAsyncInputStream: public kj::AsyncInputStream {
 public:
  MemoryAsyncInputStream(kj::ArrayPtr<const kj::byte> data): data_(data) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto dest = kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes);
    size_t amount = kj::min(dest.size(), data_.size());
    dest.first(amount).copyFrom(data_.first(amount));
    data_ = data_.slice(amount);
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return data_.size();
  }

 private:
  kj::ArrayPtr<const kj::byte> data_;
};

class MemoryAsyncOutputStream final: public kj::AsyncOutputStream {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    data.addAll(buffer);

    if (writeShouldError) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      data.addAll(piece);
    }

    if (writeShouldError) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  bool writeShouldError = false;

  kj::Vector<kj::byte> data;
};

// ======================================================================================
// Core ReadableStreamSource Interface Tests

KJ_TEST("ReadableStreamSource basic read operations (full)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 15> buffer;

    KJ_ASSERT(KJ_ASSERT_NONNULL(source->tryGetLength(rpc::StreamEncoding::IDENTITY)) == 10);
    KJ_ASSERT(source->tryGetLength(rpc::StreamEncoding::GZIP) == kj::none);

    // Read at least 5 bytes, at most 15.
    auto bytesRead = co_await source->read(buffer, 5);
    KJ_ASSERT(bytesRead == 10);  // Should read all available data

    KJ_ASSERT(buffer.asPtr().first(bytesRead) == testData);

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSource basic read operations (partial)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;

    KJ_ASSERT(KJ_ASSERT_NONNULL(source->tryGetLength(rpc::StreamEncoding::IDENTITY)) == 10);
    KJ_ASSERT(source->tryGetLength(rpc::StreamEncoding::GZIP) == kj::none);

    // Read at most 5 bytes
    auto bytesRead = co_await source->read(buffer, 5);
    KJ_ASSERT(bytesRead == 5);  // Should read all available data

    KJ_ASSERT(buffer.asPtr().first(bytesRead) == kj::arrayPtr(testData, 5));

    // Next read should return 5 byte
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 5);  // EOF
    KJ_ASSERT(buffer.asPtr().first(bytesRead) == kj::arrayPtr(testData + 5, 5));

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSource concurrent reads forbidden") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;

    auto readPromise1 = source->read(buffer, 5);
    auto readPromise2 = source->read(buffer, 5);

    try {
      co_await readPromise2;
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("already being read"));
    }

    // But the first read should still succeed.
    auto bytesRead = co_await readPromise1;
    KJ_ASSERT(bytesRead == 5);
  });
}

// ======================================================================================
// PumpTo Tests

KJ_TEST("ReadableStreamSource pumpTo with end") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, true));
    KJ_ASSERT(sink.totalBytesWritten == 10);
    KJ_ASSERT(sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableStreamSource pumpTo without end") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, false));
    KJ_ASSERT(sink.totalBytesWritten == 10);
    KJ_ASSERT(!sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableStreamSource large pumpTo with end") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, true));
    KJ_ASSERT(sink.totalBytesWritten == 52 * 1024);
    KJ_ASSERT(sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableStreamSource large pumpTo canceled") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise = environment.context.waitForDeferredProxy(source->pumpTo(sink, true));
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    try {
      co_await promise;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableStreamSource large pumpTo canceled before") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    auto promise = environment.context.waitForDeferredProxy(source->pumpTo(sink, true));
    try {
      co_await promise;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableStreamSource large pumpTo closed") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto& context = environment.context;
    co_await source->readAllBytes(kj::maxValue);
    co_await context.waitForDeferredProxy(source->pumpTo(sink, true));
    KJ_ASSERT(sink.totalBytesWritten == 0);
  });
}

KJ_TEST("ReadableStreamSource large pumpTo, concurrent read fails") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise = environment.context.waitForDeferredProxy(source->pumpTo(sink, true));

    // Concurrent read should fail.
    try {
      co_await source->readAllBytes(kj::maxValue);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("already being read"));
    }

    // But the pump should still succeed.
    co_await promise;
    KJ_ASSERT(sink.totalBytesWritten == 52 * 1024);

    // And we can read again afterwards, but will be at EOF.
    auto allBytes = co_await source->readAllBytes(kj::maxValue);
    KJ_ASSERT(allBytes.size() == 0);
  });
}

// ======================================================================================
// Read all tests

KJ_TEST("ReadableStreamSource read all bytes (small)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes = co_await source->readAllBytes(100);
    KJ_ASSERT(allBytes.size() == 10);
    KJ_ASSERT(allBytes.asPtr() == kj::ArrayPtr<const kj::byte>(testData, 10));
  });
}

KJ_TEST("ReadableStreamSource read all bytes (large)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes = co_await source->readAllBytes((52 * 1024) + 1);
    KJ_ASSERT(allBytes.size() == 52 * 1024);
    KJ_ASSERT(allBytes.asPtr() == testData);
  });
}

KJ_TEST("ReadableStreamSource read all text (small)") {
  TestFixture fixture;
  kj::byte testData[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allText = co_await source->readAllBytes(100);
    KJ_ASSERT(allText.size() == 10);
    KJ_ASSERT(allText.asPtr() == kj::ArrayPtr<const kj::byte>(testData, 10));
  });
}

KJ_TEST("ReadableStreamSource read all text (large)") {
  TestFixture fixture;
  auto testData = kj::heapArray<char>(52 * 1024);
  testData.asPtr().fill('a');

  MemoryAsyncInputStream input(testData.asBytes());
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allText = co_await source->readAllText((52 * 1024) + 1);
    KJ_ASSERT(allText.size() == 52 * 1024);
    KJ_ASSERT(allText.asPtr() == testData);
  });
}

KJ_TEST("ReadableStreamSource read all aborted (after read)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise = source->readAllBytes(52 * 1024);
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    try {
      co_await promise;
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableStreamSource read all aborted (prior to read)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    auto promise = source->readAllBytes(52 * 1024);
    try {
      co_await promise;
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableStreamSource read all aborted (dropped)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise = source->readAllBytes(52 * 1024);
    { auto freeme = kj::mv(source); }
    try {
      co_await promise;
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("stream was dropped"));
    }
  });
}

// ======================================================================================
// Tee tests

KJ_TEST("ReadableStreamSource tee (small, no limit)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  auto tee = source->tee(kj::none);
  auto branch1 = kj::mv(tee.branch1);
  auto branch2 = kj::mv(tee.branch2);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes1 = co_await branch1->readAllBytes(100);
    auto allBytes2 = co_await branch2->readAllBytes(100);
    KJ_ASSERT(allBytes1.size() == 10);
    KJ_ASSERT(allBytes1 == testData);
    KJ_ASSERT(allBytes2.size() == 10);
    KJ_ASSERT(allBytes2 == testData);

    // Original source should be closed and return EOF
    kj::FixedArray<kj::byte, 10> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSource tee (small, no limit, independent)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  auto tee = source->tee(kj::none);
  auto branch1 = kj::mv(tee.branch1);
  auto branch2 = kj::mv(tee.branch2);
  branch2->cancel(KJ_EXCEPTION(FAILED, "test abort"));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes1 = co_await branch1->readAllBytes(100);

    try {
      co_await branch2->readAllBytes(100);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
    KJ_ASSERT(allBytes1.size() == 10);
    KJ_ASSERT(allBytes1.asPtr() == testData);

    // Original source should be closed and return EOF
    kj::FixedArray<kj::byte, 10> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSource tee (large, no limit)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  auto tee = source->tee(kj::none);
  auto branch1 = kj::mv(tee.branch1);
  auto branch2 = kj::mv(tee.branch2);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes1 = co_await branch1->readAllBytes(kj::maxValue);
    auto allBytes2 = co_await branch2->readAllBytes(kj::maxValue);
    KJ_ASSERT(allBytes1.size() == 52 * 1024);
    KJ_ASSERT(allBytes1 == testData);
    KJ_ASSERT(allBytes2.size() == 52 * 1024);
    KJ_ASSERT(allBytes2 == testData);

    // Original source should be closed and return EOF
    kj::FixedArray<kj::byte, 10> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSource tee (large, buffer limit)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  auto tee = source->tee(100);
  auto branch1 = kj::mv(tee.branch1);
  auto branch2 = kj::mv(tee.branch2);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    try {
      co_await branch1->readAllBytes(kj::maxValue);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("buffer limit exceeded"));
    }
  });
}

KJ_TEST("ReadableStreamSource after read") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 512> buffer;
    buffer.asPtr().first(512).fill(0);
    buffer.asPtr().slice(512).fill(1);
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 512);
    KJ_ASSERT(buffer.asPtr().first(bytesRead) == testData.asPtr().first(bytesRead));

    auto tee = source->tee(kj::none);
    auto branch1 = kj::mv(tee.branch1);
    auto branch2 = kj::mv(tee.branch2);

    // Each branch should get the remaining data
    auto allBytes1 = co_await branch1->readAllBytes(kj::maxValue);
    auto allBytes2 = co_await branch2->readAllBytes(kj::maxValue);
    KJ_ASSERT(allBytes1.size() == 512);
    KJ_ASSERT(allBytes1 == testData.asPtr().slice(512));
    KJ_ASSERT(allBytes2.size() == 512);
    KJ_ASSERT(allBytes2 == testData.asPtr().slice(512));
  });
}

// ======================================================================================
// ReadableStreamSourceWrapper Tests

KJ_TEST("ReadableStreamSourceWrapper delegation") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  class TestWrapper: public ReadableStreamSourceWrapper {
   public:
    TestWrapper(kj::Own<ReadableStreamSource> inner): ReadableStreamSourceWrapper(kj::mv(inner)) {}
  };
  auto wrapper = kj::heap<TestWrapper>(kj::mv(source));

  KJ_ASSERT(wrapper->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 15> buffer;

    KJ_ASSERT(KJ_ASSERT_NONNULL(wrapper->tryGetLength(rpc::StreamEncoding::IDENTITY)) == 10);
    KJ_ASSERT(wrapper->tryGetLength(rpc::StreamEncoding::GZIP) == kj::none);

    // Read at least 5 bytes, at most 15.
    auto bytesRead = co_await wrapper->read(buffer, 5);
    KJ_ASSERT(bytesRead == 10);  // Should read all available data

    KJ_ASSERT(buffer.asPtr().first(bytesRead) == testData);

    // Next read should return nothing
    bytesRead = co_await wrapper->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("ReadableStreamSourceWrapper release") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableStreamSource(kj::mv(fakeOwn));

  class TestWrapper: public ReadableStreamSourceWrapper {
   public:
    TestWrapper(kj::Own<ReadableStreamSource> inner): ReadableStreamSourceWrapper(kj::mv(inner)) {}
  };
  auto wrapper = kj::heap<TestWrapper>(kj::mv(source));

  source = wrapper->release();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 15> buffer;

    // Using the wrapper shuld fail.
    try {
      co_await wrapper->read(buffer, 1);
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("inner != nullptr"));
    }

    KJ_ASSERT(KJ_ASSERT_NONNULL(source->tryGetLength(rpc::StreamEncoding::IDENTITY)) == 10);
    KJ_ASSERT(source->tryGetLength(rpc::StreamEncoding::GZIP) == kj::none);

    // Read at least 5 bytes, at most 15.
    auto bytesRead = co_await source->read(buffer, 5);
    KJ_ASSERT(bytesRead == 10);  // Should read all available data

    KJ_ASSERT(buffer.asPtr().first(bytesRead) == testData);

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

// ======================================================================================
// Factory Function Tests

KJ_TEST("newClosedReadableStreamSource") {
  TestFixture fixture;
  auto source = newClosedReadableStreamSource();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::byte buffer[10];
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("newErroredReadableStreamSource") {
  TestFixture fixture;
  auto exception = KJ_EXCEPTION(FAILED, "test error");
  auto source = newErroredReadableStreamSource(kj::cp(exception));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::byte buffer[10];
    try {
      co_await source->read(buffer, 1);
      KJ_FAIL_REQUIRE("was expected to throw");
    } catch (...) {
      auto caught = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(caught.getDescription().contains("test error"));
    }
  });
}

KJ_TEST("newReadableStreamSourceFromBytes (copy)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5};
  auto source = newReadableStreamSourceFromBytes(kj::ArrayPtr<const kj::byte>(testData, 5));
  testData[0] = 42;  // Modify original to ensure copy was made.

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 5);
    KJ_ASSERT(buffer[0] == 1);  // Original data

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("newReadableStreamSourceFromBytes (owned)") {
  TestFixture fixture;
  auto ownedData = kj::heapArray<kj::byte>(5);
  ownedData.asPtr().fill(0);
  auto ptr = ownedData.asPtr();
  auto source = newReadableStreamSourceFromBytes(ptr, kj::heap(kj::mv(ownedData)));
  ptr[0] = 42;  // Modify original to ensure copy was made.

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 5);
    KJ_ASSERT(buffer[0] == 42);  // Modified data

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("newReadableStreamSourceFromDelegate") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5};
  size_t position = 0;

  auto producer = [&](kj::ArrayPtr<kj::byte> buffer, size_t minBytes) -> kj::Promise<size_t> {
    if (position >= 5) {
      return size_t(0);  // EOF
    }

    size_t available = 5 - position;
    size_t toRead = kj::min(available, buffer.size());

    if (toRead > 0) {
      memcpy(buffer.begin(), testData + position, toRead);
      position += toRead;
    }

    return toRead;
  };

  auto source = newReadableStreamSourceFromDelegate(kj::mv(producer), 5);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 5);
    KJ_ASSERT(buffer.asPtr().first(bytesRead) == kj::ArrayPtr<const kj::byte>(testData, 5));

    // Next read should return nothing
    bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("newReadableStreamSourceFromDelegate (not enough bytes)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5};
  size_t position = 0;

  auto producer = [&](kj::ArrayPtr<kj::byte> buffer, size_t minBytes) -> kj::Promise<size_t> {
    if (position >= 5) {
      return size_t(0);  // EOF
    }

    size_t available = 5 - position;
    size_t toRead = kj::min(available, buffer.size());

    if (toRead > 0) {
      memcpy(buffer.begin(), testData + position, toRead);
      position += toRead;
    }

    return toRead;
  };

  auto source = newReadableStreamSourceFromDelegate(kj::mv(producer), 10);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 5> buffer;
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 5);
    KJ_ASSERT(buffer.asPtr().first(bytesRead) == kj::ArrayPtr<const kj::byte>(testData, 5));

    // Next read should fail since producer did not produce the expected number of bytes
    try {
      co_await source->read(buffer, 1);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("ended stream early"));
    }
  });
}

// ======================================================================================
// Gzip encoding

KJ_TEST("Gzip encoded stream") {
  TestFixture fixture;
  static constexpr kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73,
    44, 73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto inner = newMemoryInputStream(data);
  auto source = newEncodedReadableStreamSource(rpc::StreamEncoding::GZIP, kj::mv(inner));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    // Should decompress on read all...
    auto allBytes = co_await source->readAllBytes(kj::maxValue);
    KJ_ASSERT(allBytes == "some data to gzip"_kjb);
  });
}

KJ_TEST("Gzip encoded stream (pumpTo)") {
  TestFixture fixture;
  static constexpr kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73,
    44, 73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto inner = newMemoryInputStream(data);
  auto source = newEncodedReadableStreamSource(rpc::StreamEncoding::GZIP, kj::mv(inner));

  MockWritableStreamSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, true));
  });

  KJ_ASSERT(sink.writtenData == "some data to gzip"_kjb);
}

KJ_TEST("Gzip encoded stream (pumpTo same encoding)") {
  TestFixture fixture;
  static const kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73, 44,
    73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto in = newMemoryInputStream(data);
  auto source = newEncodedReadableStreamSource(rpc::StreamEncoding::GZIP, kj::mv(in));

  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableStreamSink(rpc::StreamEncoding::GZIP, kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(*sink, true));
  });

  // The data should pass through unchanged.
  KJ_ASSERT(inner.data == data);
}

KJ_TEST("Gzip encoded stream (pumpTo different encoding)") {
  TestFixture fixture;
  static const kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73, 44,
    73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto in = newMemoryInputStream(data);
  auto source = newEncodedReadableStreamSource(rpc::StreamEncoding::GZIP, kj::mv(in));

  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableStreamSink(rpc::StreamEncoding::BROTLI, kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(*sink, true));
  });

  // The data shuld be brotli compressed.
  static const kj::byte expected[] = {
    5, 8, 128, 115, 111, 109, 101, 32, 100, 97, 116, 97, 32, 116, 111, 32, 103, 122, 105, 112, 3};
  KJ_ASSERT(inner.data == expected);
}

}  // namespace
}  // namespace workerd::api::streams
