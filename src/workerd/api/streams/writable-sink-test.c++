#include "writable-sink.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>

#include <kj/async-io.h>
#include <kj/compat/gzip.h>
#include <kj/test.h>

namespace workerd::api::streams {
namespace {

// Mock WritableSink for testing wrapper functionality
class MockWritableSink final: public WritableSink {
 public:
  MockWritableSink() = default;
  ~MockWritableSink() = default;

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    writeCallCount++;
    lastWriteSize = buffer.size();
    totalBytesWritten += buffer.size();

    if (shouldFailWrite) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    // Copy data for verification
    writtenData.addAll(buffer);
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    multiWriteCallCount++;
    size_t totalSize = 0;

    for (auto piece: pieces) {
      totalSize += piece.size();
      writtenData.addAll(piece);
    }

    totalBytesWritten += totalSize;
    lastWriteSize = totalSize;

    if (shouldFailWrite) {
      KJ_FAIL_REQUIRE("Expected failure");
    }

    co_return;
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
    abortReason = kj::mv(reason);
  }

  rpc::StreamEncoding disownEncodingResponsibility() override {
    disownCallCount++;
    auto prev = encoding;
    encoding = rpc::StreamEncoding::IDENTITY;
    return prev;
  }

  rpc::StreamEncoding getEncoding() override {
    getEncodingCallCount++;
    return encoding;
  }

  // Test state accessors
  uint32_t writeCallCount = 0;
  uint32_t multiWriteCallCount = 0;
  uint32_t endCallCount = 0;
  uint32_t abortCallCount = 0;
  uint32_t disownCallCount = 0;
  uint32_t getEncodingCallCount = 0;

  size_t lastWriteSize = 0;
  size_t totalBytesWritten = 0;
  bool isEnded = false;
  kj::Maybe<kj::Exception> abortReason;

  kj::Vector<kj::byte> writtenData;
  rpc::StreamEncoding encoding = rpc::StreamEncoding::IDENTITY;

  // Control behavior for testing
  bool shouldFailWrite = false;
  bool shouldFailEnd = false;
};

// Test memory-based AsyncOutputStream for factory function tests
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

struct MockEndable final: public EndableAsyncOutputStream {
  bool isEnded = false;
  kj::Vector<kj::byte> data;

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    data.addAll(buffer);
    co_return;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      data.addAll(piece);
    }
    co_return;
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> end() override {
    isEnded = true;
    co_return;
  }
};

// ======================================================================================
// Core WritableSink Interface Tests

KJ_TEST("WritableSink basic write operations") {
  TestFixture fixture;
  MockWritableSink sink;
  kj::byte testData[] = {1, 2, 3, 4, 5};

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    // Test single buffer write
    kj::ArrayPtr<const kj::byte> buffer(testData, 5);

    co_await sink.write(buffer);
    co_await sink.end();
  });

  KJ_ASSERT(sink.writeCallCount == 1);
  KJ_ASSERT(sink.lastWriteSize == 5);
  KJ_ASSERT(sink.totalBytesWritten == 5);
  KJ_ASSERT(sink.writtenData.size() == 5);
  KJ_ASSERT(sink.writtenData == testData);
  KJ_ASSERT(sink.isEnded);
}

KJ_TEST("WritableSink multi-piece write operations") {
  TestFixture fixture;
  MockWritableSink sink;

  // Test multi-piece write
  kj::byte data1[] = {1, 2, 3};
  kj::byte data2[] = {4, 5};
  kj::byte data3[] = {6, 7, 8, 9};

  kj::ArrayPtr<const kj::byte> pieces[] = {kj::ArrayPtr<const kj::byte>(data1, 3),
    kj::ArrayPtr<const kj::byte>(data2, 2), kj::ArrayPtr<const kj::byte>(data3, 4)};

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await sink.write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>>(pieces, 3));
  });

  KJ_ASSERT(sink.multiWriteCallCount == 1);
  KJ_ASSERT(sink.lastWriteSize == 9);
  KJ_ASSERT(sink.totalBytesWritten == 9);
  KJ_ASSERT(sink.writtenData.size() == 9);

  // Verify data order
  kj::byte expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  KJ_ASSERT(sink.writtenData == kj::ArrayPtr<const kj::byte>(expected, 9));
}

KJ_TEST("WritableSink end operation") {
  TestFixture fixture;
  MockWritableSink sink;

  fixture.runInIoContext(
      [&](const auto& environment) -> kj::Promise<void> { co_await sink.end(); });

  KJ_ASSERT(sink.endCallCount == 1);
  KJ_ASSERT(sink.isEnded);
}

KJ_TEST("WritableSink abort operation") {
  MockWritableSink sink;

  sink.abort(KJ_EXCEPTION(DISCONNECTED, "Abort reason"));

  KJ_ASSERT(sink.abortCallCount == 1);
  KJ_ASSERT(sink.abortReason != kj::none);
}

KJ_TEST("WritableSink encoding operations") {
  MockWritableSink sink;

  auto encoding = sink.getEncoding();
  KJ_ASSERT(encoding == rpc::StreamEncoding::IDENTITY);
  KJ_ASSERT(sink.getEncodingCallCount == 1);

  auto disownedEncoding = sink.disownEncodingResponsibility();
  KJ_ASSERT(disownedEncoding == rpc::StreamEncoding::IDENTITY);
  KJ_ASSERT(sink.disownCallCount == 1);
}

// ======================================================================================
// WritableSinkWrapper Tests

KJ_TEST("WritableSinkWrapper write/end delegation") {
  TestFixture fixture;
  auto innerSink = kj::heap<MockWritableSink>();
  auto& sink = *innerSink;
  class TestWrapper: public WritableSinkWrapper {
   public:
    TestWrapper(kj::Own<WritableSink> inner): WritableSinkWrapper(kj::mv(inner)) {}
  };
  auto wrapper = kj::heap<TestWrapper>(kj::mv(innerSink));
  kj::byte testData[] = {1, 2, 3};

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await wrapper->write(kj::ArrayPtr<const kj::byte>(testData, 3));
    co_await wrapper->end();
  });

  KJ_ASSERT(sink.writeCallCount == 1);
  KJ_ASSERT(sink.lastWriteSize == 3);
  KJ_ASSERT(sink.totalBytesWritten == 3);
  KJ_ASSERT(sink.writtenData.size() == 3);
  KJ_ASSERT(sink.writtenData == kj::arrayPtr(testData, sizeof(testData)));
  KJ_ASSERT(sink.endCallCount == 1);
  KJ_ASSERT(sink.isEnded);
}

KJ_TEST("WritableSinkWrapper abort delegation") {
  TestFixture fixture;
  auto innerSink = kj::heap<MockWritableSink>();
  auto& sink = *innerSink;
  class TestWrapper: public WritableSinkWrapper {
   public:
    TestWrapper(kj::Own<WritableSink> inner): WritableSinkWrapper(kj::mv(inner)) {}
  };
  auto wrapper = kj::heap<TestWrapper>(kj::mv(innerSink));
  wrapper->abort(KJ_EXCEPTION(FAILED, "test abort"));
  KJ_ASSERT(sink.abortCallCount == 1);
  KJ_ASSERT(sink.abortReason != kj::none);
}

KJ_TEST("WritableSinkWrapper encoding delegation") {
  TestFixture fixture;
  auto innerSink = kj::heap<MockWritableSink>();
  auto& sink = *innerSink;
  sink.encoding = rpc::StreamEncoding::GZIP;
  class TestWrapper: public WritableSinkWrapper {
   public:
    TestWrapper(kj::Own<WritableSink> inner): WritableSinkWrapper(kj::mv(inner)) {}
  };
  auto wrapper = kj::heap<TestWrapper>(kj::mv(innerSink));

  auto encoding = wrapper->getEncoding();
  KJ_ASSERT(encoding == rpc::StreamEncoding::GZIP);
  KJ_ASSERT(sink.getEncodingCallCount == 1);
  auto disowned = wrapper->disownEncodingResponsibility();
  KJ_ASSERT(disowned == rpc::StreamEncoding::GZIP);
  KJ_ASSERT(wrapper->getEncoding() == rpc::StreamEncoding::IDENTITY);
  KJ_ASSERT(sink.disownCallCount == 1);
}

KJ_TEST("WritableSinkWrapper release functionality") {
  auto innerSink = kj::heap<MockWritableSink>();
  auto innerPtr = innerSink.get();

  class TestWrapper: public WritableSinkWrapper {
   public:
    TestWrapper(kj::Own<WritableSink> inner): WritableSinkWrapper(kj::mv(inner)) {}
  };

  auto wrapper = kj::heap<TestWrapper>(kj::mv(innerSink));

  // Release the inner sink
  auto released = wrapper->release();
  KJ_ASSERT(released.get() == innerPtr);

  // Wrapper should no longer be usable
  try {
    wrapper->abort(KJ_EXCEPTION(FAILED, "test"));
    KJ_FAIL_REQUIRE("Expected exception on using released wrapper");
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_EXPECT(exception.getDescription().contains("inner != nullptr"));
  }
}

// ======================================================================================
// Factory Function Tests

KJ_TEST("newWritableSink with AsyncOutputStream") {
  TestFixture fixture;
  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newWritableSink(kj::mv(fakeOwn));
  kj::byte testData[] = {1, 2, 3, 4, 5};

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    // Test writing data
    co_await sink->write(kj::ArrayPtr<const kj::byte>(testData, 5));
    co_await sink->end();
  });

  KJ_ASSERT(inner.data.size() == 5);
  KJ_ASSERT(inner.data == kj::arrayPtr(testData, sizeof(testData)));
}

KJ_TEST("newClosedWritableSink (write)") {
  TestFixture fixture;
  auto sink = newClosedWritableSink();
  kj::byte testData[] = {1, 2, 3};

  try {
    fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
      // Test write on closed sink
      co_await sink->write(kj::ArrayPtr<const kj::byte>(testData, 3));
    });
    KJ_FAIL_REQUIRE("should have failed");
  } catch (...) {
    auto ex = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(ex.getDescription().contains("closed stream"));
  }

  // Should not throw, write should be a no-op
}

KJ_TEST("newClosedWritableSink (end)") {
  TestFixture fixture;
  auto sink = newClosedWritableSink();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    // Test write on closed sink
    co_await sink->end();
  });

  // Should not throw, write should be a no-op
}

KJ_TEST("newClosedWritableSink (aborted)") {
  TestFixture fixture;
  auto exception = KJ_EXCEPTION(FAILED, "test error");
  auto sink = newClosedWritableSink();
  sink->abort(kj::cp(exception));

  try {
    fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
      // Test write on closed sink
      co_await sink->end();
    });
    KJ_FAIL_REQUIRE("should have failed");
  } catch (...) {
    auto caught = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(caught.getDescription() == exception.getDescription());
  }
}

KJ_TEST("newErroredWritableSink (write)") {
  TestFixture fixture;
  auto exception = KJ_EXCEPTION(FAILED, "test error");
  auto sink = newErroredWritableSink(kj::cp(exception));

  try {
    fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
      // Test write on errored sink
      co_await sink->write(kj::ArrayPtr<const kj::byte>());
    });
    KJ_FAIL_REQUIRE("should have failed");
  } catch (...) {
    auto caught = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(caught.getDescription() == exception.getDescription());
  }
}

KJ_TEST("newErroredWritableSink (end)") {
  TestFixture fixture;
  auto exception = KJ_EXCEPTION(FAILED, "test error");
  auto sink = newErroredWritableSink(kj::cp(exception));

  try {
    fixture.runInIoContext(
        [&](const auto& environment) -> kj::Promise<void> { co_await sink->end(); });
    KJ_FAIL_REQUIRE("should have failed");
  } catch (...) {
    auto caught = kj::getCaughtExceptionAsKj();
    KJ_ASSERT(caught.getDescription() == exception.getDescription());
  }
}

KJ_TEST("newNullWritableSink") {
  TestFixture fixture;
  auto sink = newNullWritableSink();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    // Test writing data
    co_await sink->write(kj::ArrayPtr<const kj::byte>());
    co_await sink->write(kj::ArrayPtr<const kj::byte>());
    co_await sink->end();
  });
}

// ======================================================================================
// Error Handling Tests

KJ_TEST("WritableSink error propagation") {
  TestFixture fixture;
  MemoryAsyncOutputStream inner;
  inner.writeShouldError = true;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newWritableSink(kj::mv(fakeOwn));
  kj::byte testData[] = {1, 2, 3, 4, 5};

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    try {
      co_await sink->write(kj::ArrayPtr<const kj::byte>(testData, 5));
      KJ_FAIL_REQUIRE("shuld have failed");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("Expected failure"));
    }
    // The write should have been recorded.
    KJ_ASSERT(inner.data.size() == 5);
    // Let's make sure the error state is persistent.
    try {
      co_await sink->write(kj::ArrayPtr<const kj::byte>(testData, 5));
      KJ_FAIL_REQUIRE("shuld have failed");
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(exception.getDescription().contains("Expected failure"));
    }
    // No additional data should have been recorded.
    KJ_ASSERT(inner.data.size() == 5);
  });
}

// ======================================================================================
// Encoding Tests

KJ_TEST("WritableSink encoding responsibility transfer") {
  TestFixture fixture;
  MemoryAsyncOutputStream inner;
  inner.writeShouldError = true;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newWritableSink(kj::mv(fakeOwn));
  KJ_ASSERT(sink->getEncoding() == rpc::StreamEncoding::IDENTITY);
  KJ_ASSERT(sink->disownEncodingResponsibility() == rpc::StreamEncoding::IDENTITY);
  KJ_ASSERT(sink->getEncoding() == rpc::StreamEncoding::IDENTITY);
}

// ======================================================================================
// Encoding-Aware WritableSink Implementation

KJ_TEST("Gzip-encoding sink") {
  auto ctx = kj::setupAsyncIo();
  TestFixture fixture({
    .waitScope = ctx.waitScope,
  });
  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableSink(rpc::StreamEncoding::GZIP, kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await sink->write("some data to gzip"_kjb);
    co_await sink->end();

    auto mem = newMemoryInputStream(inner.data);
    kj::GzipAsyncInputStream gunzip(*mem);
    auto data = co_await gunzip.readAllText(kj::maxValue);
    KJ_ASSERT(data == "some data to gzip"_kj);
  });

  auto memInput = newMemoryInputStream(inner.data);
  kj::GzipAsyncInputStream gunzip(*memInput);
  auto decompressed = gunzip.readAllBytes().wait(ctx.waitScope);

  KJ_ASSERT(decompressed.asBytes() == "some data to gzip"_kjb);
}

KJ_TEST("Gzip-encoding sink (identity)") {
  TestFixture fixture;
  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newEncodedWritableSink(rpc::StreamEncoding::GZIP, kj::mv(fakeOwn));

  static const kj::byte check[] = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3, 43, 206, 207, 77, 85, 72, 73,
    44, 73, 84, 40, 201, 87, 72, 175, 202, 44, 0, 0, 40, 58, 113, 128, 17, 0, 0, 0};

  // When encoding is disowned, the data should be passed through unmodified.
  sink->disownEncodingResponsibility();

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await sink->write(check);
    co_await sink->end();
  });

  KJ_ASSERT(inner.data == kj::arrayPtr(check, sizeof(check)));
}

// ======================================================================================
// IoContext-aware WritableSinkWrapper Tests

KJ_TEST("IoContext aware wrapper") {
  TestFixture fixture;
  MemoryAsyncOutputStream inner;
  auto fakeOwn = kj::Own<MemoryAsyncOutputStream>(&inner, kj::NullDisposer::instance);
  auto sink = newWritableSink(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    IoContext& ioContext = environment.context;
    auto wrapper = newIoContextWrappedWritableSink(ioContext, kj::mv(sink));
    co_await wrapper->write("some data"_kjb);
    co_await wrapper->end();
  });

  KJ_ASSERT(inner.data == "some data"_kjb);
}

// ======================================================================================
// EndableAsyncOutputStream Tests

KJ_TEST("EndableAsyncOutputStream") {
  TestFixture fixture;
  MockEndable inner;
  auto fakeOwn = kj::Own<MockEndable>(&inner, kj::NullDisposer::instance);
  auto sink = newWritableSink(kj::mv(fakeOwn));

  fixture.runInIoContext([&](const auto& environment) -> kj::Promise<void> {
    co_await sink->write("some data"_kjb);
    co_await sink->end();
  });

  KJ_ASSERT(inner.data == "some data"_kjb);
  KJ_ASSERT(inner.isEnded);
}

}  // namespace
}  // namespace workerd::api::streams
