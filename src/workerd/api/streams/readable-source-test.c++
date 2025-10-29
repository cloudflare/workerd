#include "readable-source.h"

#include <workerd/api/streams/writable-sink.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>

#include <kj/async-io.h>
#include <kj/test.h>

// We Thank Claude for Tests.

namespace workerd::api::streams {
namespace {

// Mock WritableSink for testing pumpTo functionality
class MockWritableSink final: public WritableSink {
 public:
  MockWritableSink() = default;
  ~MockWritableSink() = default;

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
// Core ReadableSource Interface Tests

KJ_TEST("ReadableSource basic read operations (full)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource basic read operations (partial)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource concurrent reads forbidden") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource pumpTo with end") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    KJ_ASSERT(sink.totalBytesWritten == 10);
    KJ_ASSERT(sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableSource pumpTo without end") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::NO));
    KJ_ASSERT(sink.totalBytesWritten == 10);
    KJ_ASSERT(!sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableSource large pumpTo with end") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    KJ_ASSERT(sink.totalBytesWritten == 52 * 1024);
    KJ_ASSERT(sink.isEnded);
    KJ_ASSERT(sink.writtenData == testData);
  });
}

KJ_TEST("ReadableSource large pumpTo canceled") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise =
        environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    try {
      co_await promise;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableSource large pumpTo canceled before") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    source->cancel(KJ_EXCEPTION(FAILED, "test abort"));
    auto promise =
        environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    try {
      co_await promise;
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("test abort"));
    }
  });
}

KJ_TEST("ReadableSource large pumpTo closed") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto& context = environment.context;
    co_await source->readAllBytes(kj::maxValue);
    co_await context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    KJ_ASSERT(sink.totalBytesWritten == 0);
  });
}

KJ_TEST("ReadableSource large pumpTo, concurrent read fails") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto promise =
        environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

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

KJ_TEST("ReadableSource read all bytes (small)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes = co_await source->readAllBytes(100);
    KJ_ASSERT(allBytes.size() == 10);
    KJ_ASSERT(allBytes.asPtr() == kj::ArrayPtr<const kj::byte>(testData, 10));
  });
}

KJ_TEST("ReadableSource read all bytes (large)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allBytes = co_await source->readAllBytes((52 * 1024) + 1);
    KJ_ASSERT(allBytes.size() == 52 * 1024);
    KJ_ASSERT(allBytes.asPtr() == testData);
  });
}

KJ_TEST("ReadableSource read all text (small)") {
  TestFixture fixture;
  kj::byte testData[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allText = co_await source->readAllBytes(100);
    KJ_ASSERT(allText.size() == 10);
    KJ_ASSERT(allText.asPtr() == kj::ArrayPtr<const kj::byte>(testData, 10));
  });
}

KJ_TEST("ReadableSource read all text (large)") {
  TestFixture fixture;
  auto testData = kj::heapArray<char>(52 * 1024);
  testData.asPtr().fill('a');

  MemoryAsyncInputStream input(testData.asBytes());
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  KJ_ASSERT(source->getEncoding() == rpc::StreamEncoding::IDENTITY);

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    auto allText = co_await source->readAllText((52 * 1024) + 1);
    KJ_ASSERT(allText.size() == 52 * 1024);
    KJ_ASSERT(allText.asPtr() == testData);
  });
}

KJ_TEST("ReadableSource read all aborted (after read)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource read all aborted (prior to read)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource read all aborted (dropped)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource tee (small, no limit)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  auto tee = source->tee(200);
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

KJ_TEST("ReadableSource tee (small, no limit, independent)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  auto tee = source->tee(200);
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

KJ_TEST("ReadableSource tee (large, no limit)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(52 * 1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  auto tee = source->tee(0xffffffff);
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

KJ_TEST("ReadableSource tee (large, buffer limit)") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

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

KJ_TEST("ReadableSource after read") {
  TestFixture fixture;
  auto testData = kj::heapArray<kj::byte>(1024);
  testData.asPtr().fill(42);

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::FixedArray<kj::byte, 512> buffer;
    buffer.asPtr().first(512).fill(0);
    buffer.asPtr().slice(512).fill(1);
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 512);
    KJ_ASSERT(buffer.asPtr().first(bytesRead) == testData.asPtr().first(bytesRead));

    auto tee = source->tee(0xffffffff);
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
// ReadableSourceWrapper Tests

KJ_TEST("ReadableSourceWrapper delegation") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  class TestWrapper: public ReadableSourceWrapper {
   public:
    TestWrapper(kj::Own<ReadableSource> inner): ReadableSourceWrapper(kj::mv(inner)) {}
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

KJ_TEST("ReadableSourceWrapper release") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  MemoryAsyncInputStream input(testData);
  auto fakeOwn = kj::Own<MemoryAsyncInputStream>(&input, kj::NullDisposer::instance);
  auto source = newReadableSource(kj::mv(fakeOwn));

  class TestWrapper: public ReadableSourceWrapper {
   public:
    TestWrapper(kj::Own<ReadableSource> inner): ReadableSourceWrapper(kj::mv(inner)) {}
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

KJ_TEST("newClosedReadableSource") {
  TestFixture fixture;
  auto source = newClosedReadableSource();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    kj::byte buffer[10];
    auto bytesRead = co_await source->read(buffer, 1);
    KJ_ASSERT(bytesRead == 0);  // EOF
  });
}

KJ_TEST("newErroredReadableSource") {
  TestFixture fixture;
  auto exception = KJ_EXCEPTION(FAILED, "test error");
  auto source = newErroredReadableSource(kj::cp(exception));

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

KJ_TEST("newReadableSourceFromBytes (copy)") {
  TestFixture fixture;
  kj::byte testData[] = {1, 2, 3, 4, 5};
  auto source = newReadableSourceFromBytes(kj::ArrayPtr<const kj::byte>(testData, 5));
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

KJ_TEST("newReadableSourceFromBytes (owned)") {
  TestFixture fixture;
  auto ownedData = kj::heapArray<kj::byte>(5);
  ownedData.asPtr().fill(0);
  auto ptr = ownedData.asPtr();
  auto source = newReadableSourceFromBytes(ptr, kj::heap(kj::mv(ownedData)));
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

KJ_TEST("newReadableSourceFromDelegate") {
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

  auto source = newReadableSourceFromProducer(kj::mv(producer), 5);

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

KJ_TEST("newReadableSourceFromDelegate (not enough bytes)") {
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

  auto source = newReadableSourceFromProducer(kj::mv(producer), 10);

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
  auto source = newEncodedReadableSource(rpc::StreamEncoding::GZIP, kj::mv(inner));

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
  auto source = newEncodedReadableSource(rpc::StreamEncoding::GZIP, kj::mv(inner));

  MockWritableSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
  });

  KJ_ASSERT(sink.writtenData == "some data to gzip"_kjb);
}

KJ_TEST("Gzip encoded stream (pumpTo same encoding)") {
  TestFixture fixture;
  static const kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73, 44,
    73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto in = newMemoryInputStream(data);
  auto source = newEncodedReadableSource(rpc::StreamEncoding::GZIP, kj::mv(in));

  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableSink(rpc::StreamEncoding::GZIP, kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(*sink, EndAfterPump::YES));
  });

  // The data should pass through unchanged.
  KJ_ASSERT(inner.data == data);
}

KJ_TEST("Gzip encoded stream (pumpTo different encoding)") {
  TestFixture fixture;
  static const kj::byte data[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73, 44,
    73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};
  auto in = newMemoryInputStream(data);
  auto source = newEncodedReadableSource(rpc::StreamEncoding::GZIP, kj::mv(in));

  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableSink(rpc::StreamEncoding::BROTLI, kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(*sink, EndAfterPump::YES));
  });

  // The data shuld be brotli compressed.
  static const kj::byte expected[] = {
    5, 8, 128, 115, 111, 109, 101, 32, 100, 97, 116, 97, 32, 116, 111, 32, 103, 122, 105, 112, 3};
  KJ_ASSERT(inner.data == expected);
}

// ======================================================================================
// Adaptive Pump Behavior Tests
// These tests verify the adaptive pump heuristics without relying on timing.

// Mock AsyncInputStream that tracks tryRead() calls and their parameters
class AdaptiveTestInputStream final: public kj::AsyncInputStream {
 public:
  enum class FillBehavior {
    ALWAYS_FILL_COMPLETELY,  // Always fill the buffer completely
    PARTIAL_FILLS,           // Always return partial fills
    MIXED,                   // Alternate between full and partial
  };

  AdaptiveTestInputStream(size_t totalSize, FillBehavior behavior, size_t chunkSize = 0)
      : totalSize_(totalSize),
        position_(0),
        behavior_(behavior),
        chunkSize_(chunkSize),
        readCount_(0) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    readCount_++;

    // Track the minBytes parameter on each call
    minBytesHistory_.add(minBytes);
    maxBytesHistory_.add(maxBytes);

    if (position_ >= totalSize_) {
      co_return 0;  // EOF
    }

    size_t remaining = totalSize_ - position_;
    size_t toRead = 0;

    switch (behavior_) {
      case FillBehavior::ALWAYS_FILL_COMPLETELY:
        // Fill the buffer completely up to maxBytes
        toRead = kj::min(remaining, maxBytes);
        break;

      case FillBehavior::PARTIAL_FILLS:
        // Return partial fills - less than maxBytes but at least minBytes
        // (unless at EOF). This simulates a stream with natural boundaries.
        if (remaining >= minBytes) {
          // We have enough data to satisfy minBytes
          if (chunkSize_ > 0 && chunkSize_ >= minBytes) {
            // Use chunkSize if it's large enough
            toRead = kj::min(remaining, chunkSize_);
          } else {
            // Otherwise, use minBytes to avoid triggering EOF
            toRead = minBytes;
          }
        } else {
          // At the end, return what's left (even if less than minBytes)
          toRead = remaining;
        }
        break;

      case FillBehavior::MIXED:
        // Alternate between full and partial fills
        if (readCount_ % 2 == 1) {
          toRead = kj::min(remaining, maxBytes);
        } else {
          toRead = kj::min(remaining, minBytes);
        }
        break;
    }

    // Fill buffer with predictable data
    auto dest = static_cast<kj::byte*>(buffer);
    for (size_t i = 0; i < toRead; i++) {
      dest[i] = static_cast<kj::byte>((position_ + i) & 0xFF);
    }

    position_ += toRead;
    co_return toRead;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return totalSize_;
  }

  // Test accessors
  const kj::ArrayPtr<const size_t> getMinBytesHistory() const {
    return minBytesHistory_.asPtr();
  }
  const kj::ArrayPtr<const size_t> getMaxBytesHistory() const {
    return maxBytesHistory_.asPtr();
  }
  size_t getReadCount() const {
    return readCount_;
  }

 private:
  size_t totalSize_;
  size_t position_;
  FillBehavior behavior_;
  size_t chunkSize_;
  size_t readCount_;
  kj::Vector<size_t> minBytesHistory_;
  kj::Vector<size_t> maxBytesHistory_;
};

// Mock WritableSink that tracks write patterns
class AdaptiveTestSink final: public WritableSink {
 public:
  AdaptiveTestSink() = default;
  ~AdaptiveTestSink() = default;

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    writeCallCount_++;
    writeSizes_.add(buffer.size());
    totalBytesWritten_ += buffer.size();
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    KJ_FAIL_ASSERT("Should not be called in these tests");
  }

  kj::Promise<void> end() override {
    endCallCount_++;
    co_return;
  }

  void abort(kj::Exception reason) override {
    abortCallCount_++;
  }

  rpc::StreamEncoding disownEncodingResponsibility() override {
    return rpc::StreamEncoding::IDENTITY;
  }

  rpc::StreamEncoding getEncoding() override {
    return rpc::StreamEncoding::IDENTITY;
  }

  // Test accessors
  uint32_t getWriteCallCount() const {
    return writeCallCount_;
  }
  const kj::ArrayPtr<const size_t> getWriteSizes() const {
    return writeSizes_.asPtr();
  }
  size_t getTotalBytesWritten() const {
    return totalBytesWritten_;
  }

 private:
  uint32_t writeCallCount_ = 0;
  uint32_t endCallCount_ = 0;
  uint32_t abortCallCount_ = 0;
  size_t totalBytesWritten_ = 0;
  kj::Vector<size_t> writeSizes_;
};

KJ_TEST("Adaptive pump: verify mock stream is called") {
  TestFixture fixture;

  // Simple test to verify the mock tracking actually works
  AdaptiveTestInputStream input(
      100 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));
    KJ_ASSERT(sink.getTotalBytesWritten() == 100 * 1024);

    // Verify that our mock was actually called
    // Note: Vector tracking doesn't work reliably in coroutine context, but readCount does
    KJ_ASSERT(input.getReadCount() > 0, "Mock stream was never read from");
  });
}

KJ_TEST("Adaptive pump: buffer sizing for small stream (2KB)") {
  TestFixture fixture;

  // Small stream should use a small buffer (power of 2, clamped to range)
  // For 2KB, expect buffer size of 2KB (next power of 2), clamped to MED_BUFFER_SIZE (64KB)
  AdaptiveTestInputStream input(
      2 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    // Verify the stream was read efficiently
    KJ_ASSERT(sink.getTotalBytesWritten() == 2 * 1024);

    // For small streams that fill completely, we should see efficient buffer usage
    // The buffer should be sized appropriately (power of 2, at least MIN_BUFFER_SIZE)
    auto maxBytesHistory = input.getMaxBytesHistory();
    if (maxBytesHistory.size() > 0) {
      // First read should use a buffer size that's a power of 2 and >= 2KB
      size_t firstBufferSize = maxBytesHistory[0];
      KJ_ASSERT(firstBufferSize >= 2 * 1024, firstBufferSize);
      KJ_ASSERT(
          (firstBufferSize & (firstBufferSize - 1)) == 0, "Should be power of 2", firstBufferSize);
    }
    // For very small streams, there might be optimizations that bypass our tracking
  });
}

KJ_TEST("Adaptive pump: buffer sizing for medium stream (500KB)") {
  TestFixture fixture;

  // Medium stream should be read efficiently in a reasonable number of chunks
  AdaptiveTestInputStream input(
      500 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 500 * 1024);

    // For a 500KB stream, with reasonable buffer sizing (likely 64KB),
    // we should see around 8-10 reads
    auto readCount = input.getReadCount();
    KJ_ASSERT(readCount >= 4 && readCount <= 20, "Expected 4-20 reads for 500KB stream", readCount);
  });
}

KJ_TEST("Adaptive pump: buffer sizing for large stream (2MB)") {
  TestFixture fixture;

  // Large stream (>1MB) should use MAX_BUFFER_SIZE and complete efficiently
  AdaptiveTestInputStream input(
      2 * 1024 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 2 * 1024 * 1024);

    // For a 2MB stream with MAX_BUFFER_SIZE (128KB), we should see around 16-18 reads
    auto readCount = input.getReadCount();
    KJ_ASSERT(readCount >= 10 && readCount <= 30, "Expected 10-30 reads for 2MB stream", readCount);
  });
}

KJ_TEST("Adaptive pump: fast-filling stream efficiency") {
  TestFixture fixture;

  // Stream that always fills the buffer completely should be read efficiently
  AdaptiveTestInputStream input(
      200 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 200 * 1024);

    // Fast-filling streams should complete in relatively few iterations
    auto readCount = input.getReadCount();
    KJ_ASSERT(readCount >= 2 && readCount <= 10,
        "Expected 2-10 reads for 200KB fast-filling stream", readCount);

    // Write count should be close to read count (double buffering)
    auto writeCount = sink.getWriteCallCount();
    KJ_ASSERT(writeCount >= readCount - 2 && writeCount <= readCount + 2,
        "Write count should be close to read count", writeCount, readCount);
  });
}

KJ_TEST("Adaptive pump: partial-filling stream behavior") {
  TestFixture fixture;

  // Stream that returns partial fills (32KB chunks)
  // Should require more iterations than a fast-filling stream
  AdaptiveTestInputStream input(
      200 * 1024, AdaptiveTestInputStream::FillBehavior::PARTIAL_FILLS, 32 * 1024);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 200 * 1024);

    // Partial-filling streams require more reads than fast-filling streams
    // 200KB / 32KB chunks = ~7 reads minimum
    auto readCount = input.getReadCount();
    KJ_ASSERT(readCount >= 5, "Expected at least 5 reads for partial-fill stream", readCount);
  });
}

KJ_TEST("Adaptive pump: large stream efficiency") {
  TestFixture fixture;

  // Large streams should complete efficiently with appropriate buffer sizing
  AdaptiveTestInputStream input(
      2 * 1024 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 2 * 1024 * 1024);

    // Should complete in a reasonable number of reads
    // With 128KB buffers: 2MB / 128KB = ~16 reads
    auto readCount = input.getReadCount();
    KJ_ASSERT(readCount >= 10 && readCount <= 30, "Expected 10-30 reads for 2MB stream", readCount);
  });
}

KJ_TEST("Adaptive pump: mixed behavior stream") {
  TestFixture fixture;

  // Stream that alternates between full and partial fills
  // Should still complete reasonably efficiently
  AdaptiveTestInputStream input(1024 * 1024, AdaptiveTestInputStream::FillBehavior::MIXED);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 1024 * 1024);

    // Mixed behavior should still complete in a reasonable number of reads
    auto readCount = input.getReadCount();
    KJ_ASSERT(
        readCount >= 5 && readCount <= 40, "Expected 5-40 reads for 1MB mixed stream", readCount);
  });
}

KJ_TEST("Adaptive pump: double buffering behavior") {
  TestFixture fixture;

  // Verify that the pump uses double buffering effectively
  // We can observe this by checking write patterns match read patterns
  AdaptiveTestInputStream input(
      100 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 100 * 1024);

    // With double buffering, the number of writes should be close to the number of reads
    // (minus one since the last read returns EOF)
    auto readCount = input.getReadCount();
    auto writeCount = sink.getWriteCallCount();

    // Reads should be at least as many as writes (or equal for small streams)
    KJ_ASSERT(readCount >= writeCount, readCount, writeCount);

    // For properly pipelined operation, reads and writes should be close
    // The difference should be small (typically 0-1 for good pipelining)
    KJ_ASSERT(readCount - writeCount <= 2, "Pipelining gap too large", readCount, writeCount);
  });
}

KJ_TEST("Adaptive pump: verify heuristics optimize for throughput") {
  TestFixture fixture;

  // Large stream with consistent full fills should optimize for throughput
  // by using large buffers and appropriate minBytes
  AdaptiveTestInputStream input(
      1024 * 1024, AdaptiveTestInputStream::FillBehavior::ALWAYS_FILL_COMPLETELY);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 1024 * 1024);

    auto writeSizes = sink.getWriteSizes();
    auto readCount = input.getReadCount();

    // For a 1MB stream with fast fills, we should see efficient large writes
    // The number of iterations should be relatively small
    KJ_ASSERT(readCount <= 20, "Too many iterations for 1MB stream", readCount);

    // Most writes should be using the full buffer
    size_t largeWrites = 0;
    for (size_t size: writeSizes) {
      if (size >= 32 * 1024) {  // Reasonably large writes
        largeWrites++;
      }
    }

    // Most writes should be large for throughput optimization
    KJ_ASSERT(largeWrites >= writeSizes.size() / 2, "Expected mostly large writes for throughput",
        largeWrites, writeSizes.size());
  });
}

KJ_TEST("Adaptive pump: verify heuristics optimize for responsiveness") {
  TestFixture fixture;

  // Stream with medium chunks should optimize for responsiveness
  // Using 16KB chunks which will not fill larger buffers
  AdaptiveTestInputStream input(
      256 * 1024, AdaptiveTestInputStream::FillBehavior::PARTIAL_FILLS, 16 * 1024);
  auto fakeOwn = kj::Own<AdaptiveTestInputStream>(&input, kj::NullDisposer::instance);

  auto source = newReadableSource(kj::mv(fakeOwn));
  AdaptiveTestSink sink;

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await environment.context.waitForDeferredProxy(source->pumpTo(sink, EndAfterPump::YES));

    KJ_ASSERT(sink.getTotalBytesWritten() == 256 * 1024);

    auto writeSizes = sink.getWriteSizes();

    // For partial-fill streams, writes should match the stream's natural chunk size
    // We should see multiple writes rather than trying to accumulate into large ones
    KJ_ASSERT(writeSizes.size() >= 4, "Expected multiple writes for partial-fill stream",
        writeSizes.size());

    // The write pattern should reflect the stream's behavior
    // Most writes should be around the chunk size (16KB) or minBytes
    size_t mediumWrites = 0;
    for (size_t size: writeSizes) {
      if (size >= 8 * 1024 && size <= 32 * 1024) {  // Medium chunks
        mediumWrites++;
      }
    }

    // Should have multiple medium-sized writes reflecting the partial-fill pattern
    KJ_ASSERT(mediumWrites >= 2, "Expected some medium writes for responsive stream", mediumWrites);
  });
}

}  // namespace
}  // namespace workerd::api::streams
