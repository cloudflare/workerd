#include <workerd/api/streams/readable.h>
#include <workerd/api/streams/standard.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <kj/test.h>

namespace workerd::api {

namespace {

class FakeStreamSource final: public ReadableStreamSource {
 public:
  FakeStreamSource(size_t length): length(length) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return kj::evalNow([this, maxBytes, buffer] {
      auto amount = kj::min(maxBytes, length);
      memset(buffer, 0, amount);
      length -= amount;
      return amount;
    });
  }

 private:
  size_t length;
};

KJ_TEST("Streams tee stack overflow regression") {
  // Verify that deeply nested tee() chains don't cause a stack overflow.
  // This is a regression test for a fix that removed deep recursion from tee().
  static constexpr size_t teeDepth = 200 * 1024 / sizeof(void*);
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = jsg::Lock::from(env.isolate);
    ReadableStream s(env.context, kj::heap<FakeStreamSource>(10 * 1024 * 1024));
    auto readableStreams = s.tee(js);
    for (size_t i = 0; i < teeDepth; i++) {
      readableStreams = readableStreams[0]->tee(js);
    }
  });
}

KJ_TEST("Reading from default reader") {
  static constexpr size_t streamLength = 10 * 1024;
  TestFixture testFixture;

  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);
    auto stream = js.alloc<ReadableStream>(env.context, kj::heap<FakeStreamSource>(streamLength));
    auto reader = stream->getReader(js, {});
    KJ_REQUIRE(reader.is<jsg::Ref<ReadableStreamDefaultReader>>());
    auto& defaultReader = reader.get<jsg::Ref<ReadableStreamDefaultReader>>();

    return env.context.awaitJs(js, defaultReader->read(js).then(js,
            JSG_VISITABLE_LAMBDA((reader = defaultReader.addRef(), stream = stream.addRef()),
                (reader, stream), (jsg::Lock& js, ReadResult readResult) {
      KJ_ASSERT(!readResult.done);
      auto& value = KJ_REQUIRE_NONNULL(readResult.value);
      auto handle = value.getHandle(js);
      auto u8 = KJ_ASSERT_NONNULL(handle.tryCast<jsg::JsUint8Array>());
      if (util::Autogate::isEnabled(util::AutogateKey::UPDATED_AUTO_ALLOCATE_CHUNK_SIZE)) {
        // With 16KB buffer, the entire 10KB stream fits in one read.
        KJ_ASSERT(streamLength == u8.size());
      } else {
        KJ_ASSERT(4 * 1024 == u8.size());
      }
    })));
  });
}

KJ_TEST("Reading from byob reader") {
  TestFixture testFixture;

  struct TestData {
    size_t streamLength = 10 * 1024;
    size_t bufferSize;
    bool expectDone;
  };

  TestData tests[] = {
    {.streamLength = 10 * 1024, .bufferSize = 100},
    {.streamLength = 10 * 1024, .bufferSize = 100 * 1024},
    {.streamLength = 10, .bufferSize = 100},
    {.streamLength = 1024, .bufferSize = 1024},
  };

  for (auto test: tests) {
    testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = jsg::Lock::from(env.isolate);
      auto stream =
          js.alloc<ReadableStream>(env.context, kj::heap<FakeStreamSource>(test.streamLength));
      ReadableStream::GetReaderOptions getReaderOptons = {.mode = kj::str("byob")};
      auto reader = stream->getReader(js, kj::mv(getReaderOptons));
      KJ_REQUIRE(reader.is<jsg::Ref<ReadableStreamBYOBReader>>());
      auto& byobReader = reader.get<jsg::Ref<ReadableStreamBYOBReader>>();

      auto buffer = v8::Uint8Array::New(
          v8::ArrayBuffer::New(js.v8Isolate, test.bufferSize), 0, test.bufferSize);

      return env.context.awaitJs(js, byobReader->read(js, buffer, {}).then(js,
                  JSG_VISITABLE_LAMBDA(
                      (test, reader = byobReader.addRef(), stream = stream.addRef()),
                      (reader, stream), (jsg::Lock& js, ReadResult readResult) {
        KJ_ASSERT(!readResult.done);

        auto& value = KJ_REQUIRE_NONNULL(readResult.value);
        auto handle = value.getHandle(js);
        KJ_ASSERT(handle.isUint8Array());
        v8::Local<v8::Uint8Array> u8 = KJ_ASSERT_NONNULL(handle.tryCast<jsg::JsUint8Array>());
        KJ_ASSERT(kj::min(test.streamLength, test.bufferSize) == u8->ByteLength());
        KJ_ASSERT(test.bufferSize == u8->Buffer()->ByteLength());
      })));
      return kj::READY_NOW;
    });
  }
}

KJ_TEST("ReadableStream pumpTo pending write cancellation regression") {
  // If the promise holding pumpTo's implementation is dropped while the inner
  // write to the sink is pending, the sink can be freed. In some cases, this
  // means that the sink can error because shutdownWrite is called while there
  // is still a pending write promise. This test verifies that destruction of
  // the pump operation cancels any pending write promise.

  struct TestSink final: public WritableStreamSink {
    kj::TwoWayPipe pipe;
    kj::PromiseFulfillerPair<void> paf;
    kj::Vector<kj::String>& events;
    TestSink(kj::Vector<kj::String>& events)
        : pipe(kj::newTwoWayPipe()),
          paf(kj::newPromiseAndFulfiller<void>()),
          events(events) {}

    ~TestSink() {
      events.add(kj::str("sink was destroyed"));
      pipe.ends[0]->shutdownWrite();
    }

    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      events.add(kj::str("got the write"));

      paf.fulfiller->fulfill();

      return pipe.ends[0]->write(buffer).attach(
          kj::defer([this] { events.add(kj::str("write promise was dropped")); }));
    }

    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      events.add(kj::str("got the write"));
      paf.fulfiller->fulfill();
      // Concatenate pieces into a single buffer for the pipe write.
      kj::Vector<byte> data;
      for (auto& piece: pieces) {
        data.addAll(piece);
      }
      auto arr = data.releaseAsArray();
      return pipe.ends[0]->write(arr).attach(
          kj::mv(arr), kj::defer([this] { events.add(kj::str("write promise was dropped")); }));
    }

    kj::Promise<void> end() override {
      return kj::READY_NOW;
    }

    void abort(kj::Exception reason) override {}
  };

  kj::Vector<kj::String> events;
  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);
    auto stream = ReadableStream::constructor(js,
        UnderlyingSource{.start =
                             [](jsg::Lock& js, auto controller) {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->enqueue(js, jsg::JsValue(v8::ArrayBuffer::New(js.v8Isolate, 10)));
      c->close(js);
      return js.resolvedPromise();
    }},
        kj::none);

    auto sink = kj::heap<TestSink>(events);
    auto writePromise = kj::mv(sink->paf.promise);
    auto promise = stream->pumpTo(js, kj::mv(sink), true);

    return writePromise.attach(kj::mv(promise));
  });

  KJ_ASSERT(events.size() == 3);
  KJ_ASSERT(events[0] == "got the write");
  KJ_ASSERT(events[1] == "write promise was dropped");
  KJ_ASSERT(events[2] == "sink was destroyed");
}

KJ_TEST("ReadableStream pumpTo cancels the JS source when dropped mid-stream") {
  // Regression test for https://github.com/cloudflare/workerd/issues/6832.
  //
  // When the pump is dropped while the JS ReadableStream source is suspended awaiting more
  // data (e.g. the client disconnected and the HTTP layer dropped the response-body pump),
  // the underlying source's cancel() algorithm must still run. Before the fix, dropping the
  // pump coroutine only released the reader lock and the JS source ran to natural completion.

  struct TestSink final: public WritableStreamSink {
    kj::Own<kj::PromiseFulfiller<void>> gotFirstWrite;
    bool fired = false;
    TestSink(kj::Own<kj::PromiseFulfiller<void>> gotFirstWrite)
        : gotFirstWrite(kj::mv(gotFirstWrite)) {}

    void signal() {
      if (!fired) {
        fired = true;
        gotFirstWrite->fulfill();
      }
    }
    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      signal();
      return kj::READY_NOW;
    }
    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      signal();
      return kj::READY_NOW;
    }
    kj::Promise<void> end() override {
      return kj::READY_NOW;
    }
    void abort(kj::Exception reason) override {}
  };

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  // Declared in the test scope (outliving the runInIoContext lambda frame) because the JS
  // cancel() callback captures them by reference and runs asynchronously, after the pump is
  // dropped.
  bool cancelCalled = false;
  auto cancelObserved = kj::newPromiseAndFulfiller<void>();

  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);

    auto firstWrite = kj::newPromiseAndFulfiller<void>();

    auto stream = ReadableStream::constructor(js,
        UnderlyingSource{
          .start =
              [](jsg::Lock& js, auto controller) {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      // Enqueue one chunk so the pump makes progress, but don't close the stream.
      c->enqueue(js, jsg::JsValue(v8::ArrayBuffer::New(js.v8Isolate, 10)));
      return js.resolvedPromise();
    },
          .pull =
              [](jsg::Lock& js, auto controller) {
      // Never enqueue more, so once the first chunk is drained the pump's next read suspends.
      return js.resolvedPromise();
    },
          .cancel =
              [&cancelCalled, &cancelObserved](jsg::Lock& js, jsg::JsValue) -> jsg::Promise<void> {
      cancelCalled = true;
      if (cancelObserved.fulfiller->isWaiting()) {
        cancelObserved.fulfiller->fulfill();
      }
      return js.resolvedPromise();
    },
        },
        kj::none);

    auto sink = kj::heap<TestSink>(kj::mv(firstWrite.fulfiller));
    auto pump = stream->pumpTo(js, kj::mv(sink), true);

    // Once the first chunk has been written the source is suspended on its next read. Drop the
    // pump (simulating the disconnect), then wait for the source's cancel() to run.
    return firstWrite.promise.then(
        [pump = kj::mv(pump), cancelPromise = kj::mv(cancelObserved.promise)]() mutable {
      { auto dropped = kj::mv(pump); }
      return kj::mv(cancelPromise);
    });
  });

  KJ_ASSERT(cancelCalled);
}

}  // namespace
}  // namespace workerd::api
