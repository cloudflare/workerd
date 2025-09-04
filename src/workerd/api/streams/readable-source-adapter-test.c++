#include "readable-source-adapter.h"
#include "standard.h"

#include <workerd/api/system-streams.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/own-util.h>
#include <workerd/util/stream-utils.h>

namespace workerd::api::streams {

namespace {

struct RecordingSource final: public ReadableStreamSource {
  size_t readCalled = 0;
  kj::Maybe<kj::Exception> canceledCalled;

  kj::Promise<size_t> tryRead(void*, size_t minBytes, size_t maxBytes) override {
    readCalled++;
    co_return 0;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding) override {
    static const uint64_t length = 42;
    return length;
  }

  void cancel(kj::Exception ex) override {
    canceledCalled = kj::mv(ex);
  }
};

struct NeverDoneSource final: public ReadableStreamSource {
  size_t readCalled = 0;
  kj::Maybe<kj::Exception> canceledCalled;

  kj::Promise<size_t> tryRead(void* ptr, size_t minBytes, size_t maxBytes) override {
    KJ_IF_SOME(exc, canceledCalled) {
      return kj::cp(exc);
    }
    kj::ArrayPtr<kj::byte> buffer(static_cast<kj::byte*>(ptr), maxBytes);
    buffer.fill('a');
    return maxBytes;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding) override {
    return kj::none;
  }

  void cancel(kj::Exception ex) override {
    canceledCalled = kj::mv(ex);
  }
};

struct MinimalReadSource final: public ReadableStreamSource {
  size_t readCalled = 0;
  kj::Maybe<kj::Exception> canceledCalled;

  kj::Promise<size_t> tryRead(void* ptr, size_t minBytes, size_t maxBytes) override {
    KJ_IF_SOME(exc, canceledCalled) {
      return kj::cp(exc);
    }
    kj::ArrayPtr<kj::byte> buffer(static_cast<kj::byte*>(ptr), minBytes);
    buffer.fill('a');
    return minBytes;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding) override {
    return kj::none;
  }

  void cancel(kj::Exception ex) override {
    canceledCalled = kj::mv(ex);
  }
};

struct FiniteReadSource final: public ReadableStreamSource {
  size_t readCalled = 0;
  size_t maxReads;
  kj::Maybe<kj::Exception> canceledCalled;

  FiniteReadSource(size_t maxReads): maxReads(maxReads) {}

  kj::Promise<size_t> tryRead(void* ptr, size_t minBytes, size_t maxBytes) override {
    if (readCalled >= maxReads) {
      co_return 0;
    }
    readCalled++;
    kj::ArrayPtr<kj::byte> buffer(static_cast<kj::byte*>(ptr), minBytes);
    buffer.fill('a');
    co_return minBytes;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding) override {
    return kj::none;
  }

  void cancel(kj::Exception ex) override {
    canceledCalled = kj::mv(ex);
  }
};

}  // namespace

KJ_TEST("Test successful construction with valid ReadableStreamSource") {
  TestFixture fixture;
  RecordingSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    return kj::READY_NOW;
  });

  // In the default case, when the adapter drops the source without any
  // errors/cancelations, the source shoult not be canceled, only dropped.
  KJ_ASSERT(source.canceledCalled == kj::none);
}

KJ_TEST("Adapter shutdown with no reads") {
  TestFixture fixture;
  RecordingSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    adapter->shutdown(env.js);
    adapter->shutdown(env.js);  // second call is no-op

    // Read after shutdown should be resolved immediate
    auto read = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, 10)),
        });
    KJ_ASSERT(read.getState(env.js) ==
            jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult>::State::FULFILLED,
        "Read after shutdown should be resolved immediately");

    KJ_ASSERT(adapter->isClosed(), "Adapter shoud be closed after shutdown()");
    KJ_ASSERT(adapter->isCanceled() == kj::none, "Adapter should not be canceled after shutdown()");

    return kj::READY_NOW;
  });

  KJ_ASSERT(source.canceledCalled == kj::none);
}

KJ_TEST("Adapter cancel with no reads") {
  TestFixture fixture;
  RecordingSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    adapter->cancel(env.js, env.js.error("boom"));

    auto read = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, 10)),
        });
    KJ_ASSERT(read.getState(env.js) ==
            jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult>::State::REJECTED,
        "Read after shutdown should be rejected immediately");

    adapter->shutdown(env.js);  // shutdown after cancel is no-op

    KJ_ASSERT(!adapter->isClosed(), "Adapter shoud be canceled, not closed");
    auto& ex = KJ_ASSERT_NONNULL(adapter->isCanceled());
    KJ_ASSERT(ex.getDescription().contains("boom"),
        "Adapter should be in canceled state with provided exception");

    return kj::READY_NOW;
  });

  auto& ex = KJ_ASSERT_NONNULL(source.canceledCalled);
  KJ_ASSERT(
      ex.getDescription().contains("boom"), "Source should be canceled with provided exception");
}

KJ_TEST("Adapter cancel (kj::Exception) with no reads") {
  TestFixture fixture;
  RecordingSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    adapter->cancel(KJ_EXCEPTION(FAILED, "boom"));

    KJ_ASSERT(!adapter->isClosed(), "Adapter shoud be canceled, not closed");
    auto& ex = KJ_ASSERT_NONNULL(adapter->isCanceled());
    KJ_ASSERT(ex.getDescription().contains("boom"),
        "Adapter should be in canceled state with provided exception");

    return kj::READY_NOW;
  });

  auto& ex = KJ_ASSERT_NONNULL(source.canceledCalled);
  KJ_ASSERT(
      ex.getDescription().contains("boom"), "Source should be canceled with provided exception");
}

KJ_TEST("Adapter with single read (ArrayBuffer)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, bufferSize);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                      .minBytes = 5,
                    })
                .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 10, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsArrayBuffer());
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with single read (Uint8Array)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;
    auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                      .minBytes = 5,
                    })
                .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 10, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsUint8Array());
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with single read (Int32Array)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 16;
    auto backing = jsg::BackingStore::alloc<v8::Int32Array>(env.js, bufferSize);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                      .minBytes = 5,
                    })
                .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 16, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaaaaaaaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsInt32Array());
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with single large read (ArrayBuffer)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 16 * 1024;
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, bufferSize);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                      .minBytes = 5,
                    })
                .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 16 * 1024, "Read buffer should be full size");

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsArrayBuffer());
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with single small read (ArrayBuffer)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 1;
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, bufferSize);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                      .minBytes = 5,
                    })
                .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 1, "Read buffer should be full size");

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsArrayBuffer());
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with minimal reads (Uint8Array)") {
  TestFixture fixture;
  MinimalReadSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;
    auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize);

    auto promise = adapter
                       ->read(env.js,
                           ReadableStreamSourceJsAdapter::ReadOptions{
                             .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                             .minBytes = 3,
                           })
                       .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 3, "Read buffer should be three bytes");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsUint8Array());
    });

    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with minimal reads (Uint32Array)") {
  TestFixture fixture;
  MinimalReadSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 16;
    auto backing = jsg::BackingStore::alloc<v8::Uint32Array>(env.js, bufferSize);

    auto promise = adapter
                       ->read(env.js,
                           ReadableStreamSourceJsAdapter::ReadOptions{
                             .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                             .minBytes = 3,  // Impl with round up to 4
                           })
                       .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 4, "Read buffer should be four bytes");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsUint32Array());
    });

    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with over large min reads (Uint32Array)") {
  TestFixture fixture;
  MinimalReadSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 16;
    auto backing = jsg::BackingStore::alloc<v8::Uint32Array>(env.js, bufferSize);

    auto promise = adapter
                       ->read(env.js,
                           ReadableStreamSourceJsAdapter::ReadOptions{
                             .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                             .minBytes = 24,  // Impl with round up to 4
                           })
                       .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 16, "Read buffer should be four bytes");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaaaaaaaa"_kjb);

      // BufferSource should be an ArrayBuffer
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsUint32Array());
    });

    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with over large min reads (Uint32Array)") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullSource = newNullInputStream();
    auto source = newSystemStream(kj::mv(nullSource), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(source));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 1;
    auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, bufferSize);

    auto promise = adapter
                       ->read(env.js,
                           ReadableStreamSourceJsAdapter::ReadOptions{
                             .buffer = jsg::BufferSource(env.js, kj::mv(backing)),
                           })
                       .then(env.js, [](jsg::Lock& js, auto result) {
      KJ_ASSERT(result.done, "Stream should be done");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 0, "Read buffer should be 0 bytes");
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsArrayBuffer());
    });

    return env.context.awaitJs(env.js, kj::mv(promise)).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with multiple reads (Uint8Array)") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;

    auto read1 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read2 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read3 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });

    return env.context
        .awaitJs(env.js,
            read1
                .then(env.js,
                    [read2 = kj::mv(read2)](jsg::Lock& js, auto result) mutable {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 10, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaa"_kjb);
      return kj::mv(read2);
    })
                .then(env.js, [read3 = kj::mv(read3)](jsg::Lock& js, auto result) mutable {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 10, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaa"_kjb);
      return kj::mv(read3);
    }).then(env.js, [](jsg::Lock& js, auto result) mutable {
      KJ_ASSERT(!result.done, "Stream should not be done yet");
      KJ_ASSERT(result.buffer.asArrayPtr().size() == 10, "Read buffer should be full size");
      KJ_ASSERT(result.buffer.asArrayPtr() == "aaaaaaaaaa"_kjb);
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Adapter with multiple reads shutdown") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;

    auto read1 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read2 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read3 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });

    adapter->shutdown(env.js);

    return env.context
        .awaitJs(env.js,
            read1
                .then(env.js,
                    [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<ReadableStreamSourceJsAdapter::ReadResult>(
          js.error("Should not have completed read after shutdown"));
    },
                    [read2 = kj::mv(read2)](jsg::Lock& js, jsg::Value exception) mutable
                    -> jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> {
      return kj::mv(read2);
    })
                .then(env.js,
                    [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<ReadableStreamSourceJsAdapter::ReadResult>(
          js.error("Should not have completed read after shutdown"));
    },
                    [read3 = kj::mv(read3)](jsg::Lock& js, jsg::Value exception) mutable
                    -> jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> {
      return kj::mv(read3);
    }).then(env.js, [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<void>(js.error("Should not have completed read after shutdown"));
    }, [](jsg::Lock& js, jsg::Value exception) mutable -> jsg::Promise<void> {
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });

  KJ_ASSERT(source.canceledCalled == kj::none, "Source should not be canceled after shutdown");
}

KJ_TEST("Adapter with multiple reads cancel") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    KJ_ASSERT(!adapter->isClosed(), "Adapter should not be closed upon construction");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Adapter should not be canceled upon construction");

    const size_t bufferSize = 10;

    auto read1 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read2 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });
    auto read3 = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(
              env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, bufferSize)),
        });

    adapter->cancel(env.js, env.js.error("boom"));
    adapter->cancel(env.js, env.js.error("bang"));

    return env.context
        .awaitJs(env.js,
            read1
                .then(env.js,
                    [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<ReadableStreamSourceJsAdapter::ReadResult>(
          js.error("Should not have completed read after shutdown"));
    },
                    [read2 = kj::mv(read2)](jsg::Lock& js, jsg::Value exception) mutable
                    -> jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> {
      auto handle = exception.getHandle(js);
      KJ_ASSERT(kj::str(handle).contains("boom"),
          "Read should have been rejected with cancelation error");
      return kj::mv(read2);
    })
                .then(env.js,
                    [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<ReadableStreamSourceJsAdapter::ReadResult>(
          js.error("Should not have completed read after shutdown"));
    },
                    [read3 = kj::mv(read3)](jsg::Lock& js, jsg::Value exception) mutable
                    -> jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult> {
      auto handle = exception.getHandle(js);
      KJ_ASSERT(kj::str(handle).contains("boom"),
          "Read should have been rejected with cancelation error");
      return kj::mv(read3);
    }).then(env.js, [](jsg::Lock& js, auto result) {
      return js.rejectedPromise<void>(js.error("Should not have completed read after shutdown"));
    }, [](jsg::Lock& js, jsg::Value exception) mutable -> jsg::Promise<void> {
      auto handle = exception.getHandle(js);
      KJ_ASSERT(kj::str(handle).contains("boom"),
          "Read should have been rejected with cancelation error");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });

  auto& ex = KJ_ASSERT_NONNULL(source.canceledCalled);
  KJ_ASSERT(
      ex.getDescription().contains("boom"), "Source should be canceled with provided exception");
}

KJ_TEST("Adapter close after read") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    auto read = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, 10)),
        });

    auto closePromise = adapter->close(env.js);

    return env.context
        .awaitJs(env.js,
            closePromise.then(
                env.js, [&adapter = *adapter, read = kj::mv(read)](jsg::Lock& js) mutable {
      KJ_ASSERT(adapter.isClosed(), "Adapter should be closed after close()");
      KJ_ASSERT(adapter.isCanceled() == kj::none, "Adapter should not be canceled after close()");

      KJ_ASSERT(read.getState(js) ==
              jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult>::State::FULFILLED,
          "Read should have completed successfully before close()");
    })).attach(kj::mv(adapter));
  });

  KJ_ASSERT(source.canceledCalled == kj::none, "Source should not be canceled after close");
}

KJ_TEST("Adapter close") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));
    auto closePromise = adapter->close(env.js);

    // reads after close should be resoved immediately.
    auto read = adapter->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = jsg::BufferSource(env.js, jsg::BackingStore::alloc<v8::Uint8Array>(env.js, 10)),
        });
    KJ_ASSERT(read.getState(env.js) ==
            jsg::Promise<ReadableStreamSourceJsAdapter::ReadResult>::State::FULFILLED,
        "Read after close should be fullfilled immediately");

    return env.context
        .awaitJs(env.js, closePromise.then(env.js, [&adapter = *adapter](jsg::Lock& js) {
      KJ_ASSERT(adapter.isClosed(), "Adapter should be closed after close()");
      KJ_ASSERT(adapter.isCanceled() == kj::none, "Adapter should not be canceled after close()");
    })).attach(kj::mv(adapter));
  });

  KJ_ASSERT(source.canceledCalled == kj::none, "Source should not be canceled after close");
}

KJ_TEST("Adapter close superseded by cancel") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    auto closePromise = adapter->close(env.js);

    adapter->cancel(env.js, env.js.error("boom"));

    return env.context
        .awaitJs(env.js, closePromise.then(env.js, [](jsg::Lock& js) {
      return js.rejectedPromise<void>(js.error("Should not have completed close after cancel"));
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto handle = exception.getHandle(js);
      KJ_ASSERT(kj::str(handle).contains("boom"),
          "Close should have been rejected with cancelation error");
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("After read BackingStore maintains identity") {
  TestFixture fixture;
  NeverDoneSource source;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    std::unique_ptr<v8::BackingStore> backing =
        v8::ArrayBuffer::NewBackingStore(env.js.v8Isolate, 10);
    auto* backingPtr = backing.get();
    v8::Local<v8::ArrayBuffer> originalArrayBuffer =
        v8::ArrayBuffer::New(env.js.v8Isolate, kj::mv(backing));
    jsg::BufferSource source(env.js, originalArrayBuffer);

    return env.context
        .awaitJs(env.js,
            adapter
                ->read(env.js,
                    ReadableStreamSourceJsAdapter::ReadOptions{
                      .buffer = jsg::BufferSource(env.js, originalArrayBuffer),
                      .minBytes = 5,
                    })
                .then(env.js, [backingPtr](jsg::Lock& js, auto result) {
      auto handle = result.buffer.getHandle(js);
      KJ_ASSERT(handle->IsArrayBuffer());
      auto backing = handle.template As<v8::ArrayBuffer>()->GetBackingStore();
      KJ_ASSERT(backing.get() == backingPtr);
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Read all text") {
  TestFixture fixture;
  FiniteReadSource source(2);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    return env.context
        .awaitJs(env.js,
            adapter->readAllText(env.js).then(
                env.js, [&adapter = *adapter](jsg::Lock& js, jsg::JsRef<jsg::JsString> result) {
      auto str = result.getHandle(js).toString(js);
      KJ_ASSERT(str.size() == 8192);
      KJ_ASSERT(adapter.isClosed(), "Adapter should be closed after readAllText()");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Read all bytes") {
  TestFixture fixture;
  FiniteReadSource source(2);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    return env.context
        .awaitJs(env.js,
            adapter->readAllBytes(env.js).then(
                env.js, [&adapter = *adapter](jsg::Lock& js, jsg::BufferSource result) {
      KJ_ASSERT(result.size() == 8192);
      KJ_ASSERT(adapter.isClosed(), "Adapter should be closed after readAllText()");
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Read all text (limit)") {
  TestFixture fixture;
  FiniteReadSource source(2);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    return env.context
        .awaitJs(env.js,
            adapter->readAllText(env.js, 100)
                .then(env.js,
                    [](jsg::Lock& js, jsg::JsRef<jsg::JsString> result) -> jsg::Promise<void> {
      KJ_FAIL_ASSERT("Should not have completed readAllText within limit");
    }, [&adapter = *adapter](jsg::Lock& js, jsg::Value exception) {
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("Read all bytes (limit)") {
  TestFixture fixture;
  FiniteReadSource source(2);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    kj::Own<ReadableStreamSource> fake(&source, kj::NullDisposer::instance);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(fake));

    return env.context
        .awaitJs(env.js,
            adapter->readAllBytes(env.js, 100)
                .then(env.js, [](jsg::Lock&, auto) -> jsg::Promise<void> {
      KJ_FAIL_ASSERT("Should not have completed readAllBytes within limit");
    }, [&adapter = *adapter](jsg::Lock& js, jsg::Value exception) {
      return js.resolvedPromise();
    })).attach(kj::mv(adapter));
  });
}

KJ_TEST("tryGetLength") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto nullSource = newNullInputStream();
    auto source = newSystemStream(kj::mv(nullSource), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(source));
    auto length = KJ_ASSERT_NONNULL(adapter->tryGetLength(StreamEncoding::IDENTITY));
    KJ_ASSERT(length == 0, "Length of empty stream should be 0");

    adapter->shutdown(env.js);

    KJ_ASSERT(adapter->tryGetLength(StreamEncoding::IDENTITY) == kj::none,
        "Length after shutdown should be none");

    return kj::READY_NOW;
  });
}

KJ_TEST("tee successful") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto dataSource = newMemoryInputStream("hello world"_kjb);
    auto source = newSystemStream(kj::mv(dataSource), StreamEncoding::IDENTITY, env.context);
    auto adapter = kj::heap<ReadableStreamSourceJsAdapter>(env.js, env.context, kj::mv(source));

    auto [branch1, branch2] = KJ_ASSERT_NONNULL(adapter->tryTee(env.js));

    KJ_ASSERT(adapter->isClosed(), "Original adapter should be closed after tee");
    KJ_ASSERT(
        adapter->isCanceled() == kj::none, "Original adapter should not be canceled after tee");

    KJ_ASSERT(!branch1->isClosed(), "Branch1 should not be closed after tee");
    KJ_ASSERT(branch1->isCanceled() == kj::none, "Branch1 should not be canceled after tee");

    KJ_ASSERT(!branch2->isClosed(), "Branch2 should not be closed after tee");
    KJ_ASSERT(branch2->isCanceled() == kj::none, "Branch2 should not be canceled after tee");

    auto backing1 = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 11);
    auto buffer1 = jsg::BufferSource(env.js, kj::mv(backing1));
    auto read1 = branch1->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = kj::mv(buffer1),
        });
    auto backing2 = jsg::BackingStore::alloc<v8::ArrayBuffer>(env.js, 11);
    auto buffer2 = jsg::BufferSource(env.js, kj::mv(backing2));
    auto read2 = branch2->read(env.js,
        ReadableStreamSourceJsAdapter::ReadOptions{
          .buffer = kj::mv(buffer2),
        });

    return env.context
        .awaitJs(env.js,
            kj::mv(read1)
                .then(env.js, [read2 = kj::mv(read2)](jsg::Lock& js, auto result1) mutable {
      KJ_ASSERT(!result1.done, "Stream should not be done yet");
      KJ_ASSERT(result1.buffer.asArrayPtr().size() == 11);
      KJ_ASSERT(result1.buffer.asArrayPtr() == "hello world"_kjb);
      return kj::mv(read2);
    }).then(env.js, [](jsg::Lock& js, auto result2) {
      KJ_ASSERT(!result2.done, "Stream should not be done yet");
      KJ_ASSERT(result2.buffer.asArrayPtr().size() == 11);
      KJ_ASSERT(result2.buffer.asArrayPtr() == "hello world"_kjb);
      return js.resolvedPromise();
    })).attach(kj::mv(branch1), kj::mv(branch2));
  });
}

// ===========================================================================================

namespace {
static size_t countStatic = 0;
jsg::Ref<ReadableStream> createFiniteBytesReadableStream(
    jsg::Lock& js, size_t chunkSize = 1024, size_t* count = nullptr) {
  if (count == nullptr) {
    countStatic = 0;
    count = &countStatic;
  }
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .pull =
            [chunkSize, count](jsg::Lock& js, auto controller) {
    auto c = kj::mv(
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>()));
    auto& counter = *count;
    if (counter++ < 10) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
      jsg::BufferSource buffer(js, kj::mv(backing));
      buffer.asArrayPtr().fill(96 + counter);  // fill with 'a'...'j'
      c->enqueue(js, buffer.getHandle(js));
    }
    if (counter == 10) {
      c->close(js);
    }
    return js.resolvedPromise();
  },
        .expectedLength = 10 * chunkSize,
      },
      StreamQueuingStrategy{
        .highWaterMark = 0,
      });
}

jsg::Ref<ReadableStream> createFiniteByobReadableStream(jsg::Lock& js, size_t chunkSize = 1024) {
  return ReadableStream::constructor(js,
      UnderlyingSource{
        .type = kj::str("bytes"),
        .pull =
            [chunkSize](jsg::Lock& js, auto controller) {
    auto c = kj::mv(
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableByteStreamController>>()));
    static int count = 0;
    if (count++ < 10) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, chunkSize);
      jsg::BufferSource buffer(js, kj::mv(backing));
      c->enqueue(js, kj::mv(buffer));
    }
    if (count == 10) {
      c->close(js);
    }
    return js.resolvedPromise();
  },
        .expectedLength = 10 * chunkSize,
      },
      kj::none);
}

jsg::Ref<ReadableStream> createErroredStream(jsg::Lock& js) {
  return ReadableStream::constructor(js,
      UnderlyingSource{.start =
                           [](jsg::Lock& js, auto controller) {
    auto c = kj::mv(
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>()));
    c->error(js, js.error("boom"));
    return js.resolvedPromise();
  }},
      kj::none);
}

jsg::Ref<ReadableStream> createClosedStream(jsg::Lock& js) {
  return ReadableStream::constructor(js,
      UnderlyingSource{.start =
                           [](jsg::Lock& js, auto controller) {
    auto c = kj::mv(
        KJ_ASSERT_NONNULL(controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>()));
    c->close(js);
    return js.resolvedPromise();
  }},
      kj::none);
}

struct RecordingSink final: public WritableStreamSink {
  kj::Vector<kj::byte> data;
  bool ended = false;
  kj::Maybe<kj::Exception> aborted;

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    data.addAll(buffer.begin(), buffer.end());
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    for (auto piece: pieces) {
      data.addAll(piece.begin(), piece.end());
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> end() override {
    ended = true;
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {
    aborted = kj::mv(reason);
  }
};

struct ErrorSink final: public WritableStreamSink {
  kj::Maybe<kj::Exception> aborted;

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; Write failed");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; Write failed");
  }

  kj::Promise<void> end() override {
    return KJ_EXCEPTION(FAILED, "worker_do_not_log; End failed");
  }

  void abort(kj::Exception reason) override {
    aborted = kj::mv(reason);
  }
};
}  // namespace

KJ_TEST("KjAdapter constructor with valid normal ReadableStream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 16 * 1024);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    // The size is known because we provided expectedLength in the source.
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->tryGetLength(StreamEncoding::IDENTITY)), 16 * 1024);

    // The preferred encoding is always IDENTITY
    KJ_ASSERT(adapter->getPreferredEncoding() == StreamEncoding::IDENTITY);

    // Teeing is unsupported so always returns kj::none
    KJ_ASSERT(adapter->tryTee(0) == kj::none);

    return kj::READY_NOW;
  });
}

KJ_TEST("KjAdapter constructor with valid byob ReadableStream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteByobReadableStream(env.js, 16 * 1024);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    // The size is known because we provided expectedLength in the source.
    KJ_ASSERT(KJ_ASSERT_NONNULL(adapter->tryGetLength(StreamEncoding::IDENTITY)), 16 * 1024);

    // The preferred encoding is always IDENTITY
    KJ_ASSERT(adapter->getPreferredEncoding() == StreamEncoding::IDENTITY);

    // Teeing is unsupported so always returns kj::none
    KJ_ASSERT(adapter->tryTee(0) == kj::none);

    return kj::READY_NOW;
  });
}

KJ_TEST("KjAdapter constructor with valid ReadableStream manual cancel") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 16 * 1024);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    adapter->cancel(KJ_EXCEPTION(FAILED, "Manual cancel"));

    KJ_ASSERT(stream->isLocked(), "Stream should remain locked after adapter cancel");

    KJ_ASSERT(adapter->tryGetLength(StreamEncoding::IDENTITY) == kj::none,
        "Length after cancel should be none");

    return kj::READY_NOW;
  });
}

KJ_TEST("KjAdapter constructor with locked/disturbed stream fails") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 16 * 1024);
    auto reader = stream->getReader(env.js, kj::none);
    try {
      kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
      KJ_FAIL_ASSERT("Should not be able to get adapter");
    } catch (...) {
      // Expected.
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription().contains("ReadableStream is locked"));
    }

    auto& r = KJ_ASSERT_NONNULL(reader.tryGet<jsg::Ref<ReadableStreamDefaultReader>>());
    r->read(env.js);
    r->releaseLock(env.js);
    KJ_ASSERT(stream->isDisturbed());

    // Disturbed streams are also fatal, even if not locked.
    KJ_ASSERT(stream->isDisturbed());

    try {
      kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
      KJ_FAIL_ASSERT("Should not be able to get adapter");
    } catch (...) {
      // Expected.
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription().contains("ReadableStream is disturbed"));
    }

    return kj::READY_NOW;
  });
}

KJ_TEST("KjAdapter tryRead with valid buffer and byte ranges") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 1024, &counter);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    auto buffer = kj::heapArray<kj::byte>(2049);

    return adapter->tryRead(buffer.begin(), 512, buffer.size())
        .then([buffer = kj::mv(buffer), &adapter = *adapter](size_t bytesRead) mutable {
      KJ_ASSERT(bytesRead >= 512 && bytesRead <= buffer.size());
      KJ_ASSERT(bytesRead == 2048);

      kj::FixedArray<kj::byte, 2048> expected;
      expected.asPtr().first(1024).fill(97);  // 'a'
      expected.asPtr().slice(1024).fill(98);  // 'b'
      KJ_ASSERT(buffer.asPtr().first(bytesRead) == expected.asPtr());

      // Perform another read...
      return adapter.tryRead(buffer.begin(), 1, buffer.size())
          .then([buffer = kj::mv(buffer)](size_t bytesRead) {
        KJ_ASSERT(bytesRead >= 1 && bytesRead <= buffer.size());
        KJ_ASSERT(bytesRead == 2048);

        kj::FixedArray<kj::byte, 2048> expected;
        expected.asPtr().first(1024).fill(99);   // 'c'
        expected.asPtr().slice(1024).fill(100);  // 'd'
        KJ_ASSERT(buffer.asPtr().first(bytesRead) == expected.asPtr());

        return kj::READY_NOW;
      });
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter tryRead with left over (source provides more than requested)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 1024, &counter);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    auto buffer = kj::heapArray<kj::byte>(1000);

    return adapter->tryRead(buffer.begin(), 1000, buffer.size())
        .then([buffer = kj::mv(buffer), &adapter = *adapter](size_t bytesRead) mutable {
      KJ_ASSERT(bytesRead >= 512 && bytesRead <= buffer.size());
      KJ_ASSERT(bytesRead == 1000);

      kj::FixedArray<kj::byte, 1000> expected;
      expected.asPtr().fill(97);  // 'a'
      KJ_ASSERT(buffer.asPtr().first(bytesRead) == expected.asPtr());

      // Perform another read...
      return adapter.tryRead(buffer.begin(), 1, buffer.size())
          .then([buffer = kj::mv(buffer)](size_t bytesRead) {
        // The next read should be only for the 24 remaining bytes leftover from the first chunk.
        KJ_ASSERT(bytesRead >= 1 && bytesRead <= buffer.size());
        KJ_ASSERT(bytesRead == 24);

        kj::FixedArray<kj::byte, 24> expected;
        expected.asPtr().fill(97);  // 'a'
        KJ_ASSERT(buffer.asPtr().first(bytesRead) == expected.asPtr());

        return kj::READY_NOW;
      });
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter tryRead with clamped minBytes (minBytes=0)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 5, &counter);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    auto buffer = kj::heapArray<kj::byte>(3);

    return adapter->tryRead(buffer.begin(), 0, buffer.size())
        .then([buffer = kj::mv(buffer), &adapter = *adapter](size_t bytesRead) mutable {
      // Should return exactly 1 byte, since minBytes is clamped to 1.
      KJ_ASSERT(bytesRead >= 1);
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter tryRead with clamped minBytes (minBytes > maxBytes)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 5, &counter);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    auto buffer = kj::heapArray<kj::byte>(3);

    return adapter->tryRead(buffer.begin(), 4, buffer.size())
        .then([buffer = kj::mv(buffer), &adapter = *adapter](size_t bytesRead) mutable {
      // Should return exactly 3 byte, since minBytes is clamped to 3.
      KJ_ASSERT(bytesRead == 3);
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter tryRead with zero length buffer") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 5, &counter);
    KJ_ASSERT(!stream->isLocked(), "Stream should not be locked before adapter construction");
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());
    KJ_ASSERT(stream->isLocked(), "Stream should be locked after adapter construction");

    auto buffer = kj::heapArray<kj::byte>(0);

    return adapter->tryRead(buffer.begin(), 1, buffer.size())
        .then([buffer = kj::mv(buffer), &adapter = *adapter](size_t bytesRead) mutable {
      // Should return exactly 0 byte
      KJ_ASSERT(bytesRead == 0);
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter forbid concurrent reads") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  // Constructs and drops without failures

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 5, &counter);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    auto buffer = kj::heapArray<kj::byte>(2);

    // Concurrent reads are not allowed.
    auto read1 = adapter->tryRead(buffer.begin(), 1, buffer.size());

    try {
      auto read2 KJ_UNUSED = adapter->tryRead(buffer.begin(), 1, buffer.size());
    } catch (...) {
      auto ex = kj::getCaughtExceptionAsKj();
      KJ_ASSERT(ex.getDescription().contains("Cannot have multiple concurrent reads"));
    }

    return kj::READY_NOW;
  });
}

KJ_TEST("KjAdapter cancel in-flight reads") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 5, &counter);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    auto buffer = kj::heapArray<kj::byte>(2);

    // Concurrent reads are not allowed.
    auto read1 = adapter->tryRead(buffer.begin(), 1, buffer.size());

    adapter->cancel(KJ_EXCEPTION(FAILED, "worker_do_not_log; Manual cancel"));

    return read1
        .then([](size_t) { KJ_FAIL_ASSERT("Should not have completed read after cancel"); },
            [](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("Manual cancel"));
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter read errored stream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createErroredStream(env.js);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    auto buffer = kj::heapArray<kj::byte>(2);

    // Concurrent reads are not allowed.
    auto read1 = adapter->tryRead(buffer.begin(), 1, buffer.size());

    return read1
        .then([](size_t) { KJ_FAIL_ASSERT("Should not have completed read after cancel"); },
            [&adapter = *adapter](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("boom"));
    })
        .then([&adapter = *adapter]() {
      // The adapter should be in the errored state now.
      kj::FixedArray<kj::byte, 1> buf;
      return adapter.tryRead(buf.begin(), 1, 1).then([](auto) {
        KJ_FAIL_ASSERT("Should not have completed read on errored adapter");
      }, [](kj::Exception exception) { KJ_ASSERT(exception.getDescription().contains("boom")); });
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter read closed stream") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createClosedStream(env.js);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    auto buffer = kj::heapArray<kj::byte>(2);

    auto read1 = adapter->tryRead(buffer.begin(), 1, buffer.size());

    return read1.then([](size_t size) { KJ_ASSERT(size == 0); }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter pumpTo") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  RecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 1024);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    return adapter->pumpTo(sink, true).attach(kj::mv(adapter));
  });

  kj::FixedArray<kj::byte, 10 * 1024> expected;
  expected.asPtr().first(1024).fill(97);          // 'a'
  expected.asPtr().slice(1024, 2048).fill(98);    // 'b'
  expected.asPtr().slice(2048, 3072).fill(99);    // 'c'
  expected.asPtr().slice(3072, 4096).fill(100);   // 'd'
  expected.asPtr().slice(4096, 5120).fill(101);   // 'e'
  expected.asPtr().slice(5120, 6144).fill(102);   // 'f'
  expected.asPtr().slice(6144, 7168).fill(103);   // 'g'
  expected.asPtr().slice(7168, 8192).fill(104);   // 'h'
  expected.asPtr().slice(8192, 9216).fill(105);   // 'i'
  expected.asPtr().slice(9216, 10240).fill(106);  // 'j'

  KJ_ASSERT(sink.data.size() == 10 * 1024);
  KJ_ASSERT(sink.data.asPtr() == expected.asPtr());
  KJ_ASSERT(sink.ended);
  KJ_ASSERT(sink.aborted == kj::none);
}

KJ_TEST("KjAdapter pumpTo (no end)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  RecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 1024);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    return adapter->pumpTo(sink, false).attach(kj::mv(adapter));
  });

  kj::FixedArray<kj::byte, 10 * 1024> expected;
  expected.asPtr().first(1024).fill(97);          // 'a'
  expected.asPtr().slice(1024, 2048).fill(98);    // 'b'
  expected.asPtr().slice(2048, 3072).fill(99);    // 'c'
  expected.asPtr().slice(3072, 4096).fill(100);   // 'd'
  expected.asPtr().slice(4096, 5120).fill(101);   // 'e'
  expected.asPtr().slice(5120, 6144).fill(102);   // 'f'
  expected.asPtr().slice(6144, 7168).fill(103);   // 'g'
  expected.asPtr().slice(7168, 8192).fill(104);   // 'h'
  expected.asPtr().slice(8192, 9216).fill(105);   // 'i'
  expected.asPtr().slice(9216, 10240).fill(106);  // 'j'

  KJ_ASSERT(sink.data.size() == 10 * 1024);
  KJ_ASSERT(sink.data.asPtr() == expected.asPtr());
  KJ_ASSERT(!sink.ended);
  KJ_ASSERT(sink.aborted == kj::none);
}

KJ_TEST("KjAdapter pumpTo (errored)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  RecordingSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createErroredStream(env.js);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    return env.context.waitForDeferredProxy(adapter->pumpTo(sink, false))
        .then([]() -> kj::Promise<void> {
      KJ_FAIL_ASSERT("Should not have completed pumpTo on errored stream");
    }, [](kj::Exception exception) {
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter pumpTo (error sink)") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  ErrorSink sink;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto stream = createFiniteBytesReadableStream(env.js, 1000);
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef());

    return env.context.waitForDeferredProxy(adapter->pumpTo(sink, false))
        .then([]() -> kj::Promise<void> {
      KJ_FAIL_ASSERT("Should not have completed pumpTo on errored stream");
    }, [](kj::Exception exception) {
      KJ_ASSERT(exception.getDescription().contains("Write failed"));
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter MinReadPolicy IMMEDIATE behavior") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Create a stream that returns data in small chunks to test the policy difference
    auto stream = ReadableStream::constructor(env.js,
        UnderlyingSource{
          .pull =
              [&counter](jsg::Lock& js, auto controller) {
      auto& c = KJ_ASSERT_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      if (counter < 8) {
        // Return 256 bytes per chunk, 8 chunks total (2048 bytes)
        auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 256);
        jsg::BufferSource buffer(js, kj::mv(backing));
        buffer.asArrayPtr().fill(97 + counter);  // 'a', 'b', 'c', etc.
        c->enqueue(js, buffer.getHandle(js));
        counter++;
      } else {
        c->close(js);
      }
      return js.resolvedPromise();
    },
          .expectedLength = 2048,
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Test IMMEDIATE policy - should return as soon as minBytes is satisfied
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef(),
        ReadableStreamSourceKjAdapter::Options{
          .minReadPolicy = ReadableStreamSourceKjAdapter::MinReadPolicy::IMMEDIATE});

    auto buffer = kj::heapArray<kj::byte>(2048);

    return adapter->tryRead(buffer.begin(), 512, buffer.size())
        .then([buffer = kj::mv(buffer)](size_t bytesRead) {
      // With IMMEDIATE policy, should return as soon as minBytes (512) is satisfied
      KJ_ASSERT(bytesRead == 512, "Should have read exactly minBytes");

      // Verify the data content matches expected pattern
      for (size_t i = 0; i < bytesRead; i++) {
        size_t chunkIndex = i / 256;
        KJ_ASSERT(buffer[i] == static_cast<kj::byte>(97 + chunkIndex),
            "Data should match expected pattern");
      }

      return kj::READY_NOW;
    }).attach(kj::mv(adapter));
  });
}

KJ_TEST("KjAdapter MinReadPolicy OPPORTUNISTIC behavior") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setStreamsJavaScriptControllers(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  size_t counter = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    // Create a stream that returns data in small chunks to test the policy difference
    auto stream = ReadableStream::constructor(env.js,
        UnderlyingSource{
          .pull =
              [&counter](jsg::Lock& js, auto controller) {
      auto c = kj::mv(KJ_ASSERT_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>()));

      if (counter < 8) {
        // Return 256 bytes per chunk, 8 chunks total (2048 bytes)
        auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 256);
        jsg::BufferSource buffer(js, kj::mv(backing));
        buffer.asArrayPtr().fill(97 + counter);  // 'a', 'b', 'c', etc.
        c->enqueue(js, buffer.getHandle(js));
        counter++;
      } else {
        c->close(js);
      }
      return js.resolvedPromise();
    },
          .expectedLength = 2048,
        },
        StreamQueuingStrategy{.highWaterMark = 0});

    // Test OPPORTUNISTIC policy - should try to fill buffer more completely
    auto adapter = kj::heap<ReadableStreamSourceKjAdapter>(env.js, env.context, stream.addRef(),
        ReadableStreamSourceKjAdapter::Options{
          .minReadPolicy = ReadableStreamSourceKjAdapter::MinReadPolicy::OPPORTUNISTIC});

    auto buffer = kj::heapArray<kj::byte>(2048);

    return adapter->tryRead(buffer.begin(), 512, buffer.size())
        .then([buffer = kj::mv(buffer)](size_t bytesRead) {
      // With OPPORTUNISTIC policy, should try to fill buffer more completely
      // when data is readily available
      KJ_ASSERT(bytesRead == 1792, "Should have read as much as possible up to maxBytes");

      // Verify the data content matches expected pattern
      for (size_t i = 0; i < bytesRead; i++) {
        size_t chunkIndex = i / 256;
        KJ_ASSERT(buffer[i] == static_cast<kj::byte>(97 + chunkIndex),
            "Data should match expected pattern");
      }

      return kj::READY_NOW;
    }).attach(kj::mv(adapter));
  });
}

}  // namespace workerd::api::streams
