#include <workerd/api/basics.h>
#include <workerd/api/streams/identity-transform-stream.h>
#include <workerd/api/streams/internal.h>
#include <workerd/api/streams/readable.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/streams/writable.h>
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
      KJ_ASSERT(handle.isUint8Array());
      jsg::JsBufferSource source(handle);
      if (util::Autogate::isEnabled(util::AutogateKey::UPDATED_AUTO_ALLOCATE_CHUNK_SIZE)) {
        // With 16KB buffer, the entire 10KB stream fits in one read.
        KJ_ASSERT(streamLength == source.size());
      } else {
        KJ_ASSERT(4 * 1024 == source.size());
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

      auto buffer = jsg::JsUint8Array::create(js, test.bufferSize);
      return env.context.awaitJs(js, byobReader->read(js, buffer, {}).then(js,
                  JSG_VISITABLE_LAMBDA(
                      (test, reader = byobReader.addRef(), stream = stream.addRef()),
                      (reader, stream), (jsg::Lock& js, ReadResult readResult) {
        KJ_ASSERT(!readResult.done);

        auto& value = KJ_REQUIRE_NONNULL(readResult.value);
        auto handle = value.getHandle(js);
        auto view = KJ_REQUIRE_NONNULL(handle.tryCast<jsg::JsUint8Array>());
        KJ_ASSERT(kj::min(test.streamLength, test.bufferSize) == view.size());
        KJ_ASSERT(test.bufferSize == view.getBuffer().size());
      })));
      return kj::READY_NOW;
    });
  }
}

KJ_TEST("PumpToReader regression") {
  // If the promise holding the PumpToReader is dropped while the inner
  // write to the sink is pending, the PumpToReader can free the sink.
  // In some cases, this means that the sink can error because shutdownWrite
  // is called while there is still a pending write promise. This test verifies
  // that PumpToReader cancels any pending write promise when it is destroyed.

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
      auto ab = jsg::JsArrayBuffer::create(js, 10);
      c->enqueue(js, ab);
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

// Returns true if the controller's queue still contains a Pipe event whose
// source PipeController reference is now dangling. This is the post-condition
// the pipeLoop UAF class leaves behind: `source.release()` (or its peers) was
// called but `queue.pop_front()` was not, so a downstream handlePromise
// continuation would dereference `request.source()` through stale storage.
//
// Lives in test code (rather than as a method on the controller) so the
// production class doesn't grow a test-only accessor.
static bool hasPhantomPipeInQueue(WritableStream& writable) {
  auto& controller = writable.getController();
  return kj::downcast<WritableStreamInternalController>(controller).isPiping();
}

KJ_TEST("Phantom Pipe in queue after AbortSignal — checkSignal preventAbort "
        "path (AUTOVULN-CLOUDFLARE-WORKERD-261)") {
  // pipeTo(JS-backed source, internal-backed sink) with a signal that fires
  // during pull(). checkSignal's preventAbort branch must pop the Pipe from
  // the queue so handlePromise.success bails on queue.empty() instead of
  // dereferencing a stale sourceRef in internal.c++

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  struct {
    bool successRan = false;
    bool failureRan = false;
    bool phantomPipe = false;
  } result;

  testFixture.runInIoContext([&result](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);

    auto abortController = js.alloc<AbortController>(js);
    auto signal = abortController->getSignal();
    AbortController* acPtr = &*abortController;

    auto rs = ReadableStream::constructor(js,
        UnderlyingSource{
          .start = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
      return js.resolvedPromise();
    },
          .pull = [acPtr](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
      acPtr->abort(js, kj::none);
      return js.resolvedPromise();
    },
        },
        kj::none);

    auto its = IdentityTransformStream::constructor(js, kj::none);
    auto writable = its->getWritable();

    PipeToOptions opts;
    opts.preventAbort = true;
    opts.signal = kj::mv(signal);

    auto pipePromise = rs->pipeTo(js, writable.addRef(), kj::mv(opts));

    return env.context.awaitJs(js,
        pipePromise.then(js,
            JSG_VISITABLE_LAMBDA(
                (&result, writableRef = writable.addRef(), ac = abortController.addRef(),
                    its = its.addRef(), rsRef = rs.addRef()),
                (writableRef, ac, its, rsRef),
                (jsg::Lock& js) {
                  result.successRan = true;
                  result.phantomPipe = hasPhantomPipeInQueue(*writableRef);
                }),
            JSG_VISITABLE_LAMBDA(
                (&result, writableRef = writable.addRef(), ac = kj::mv(abortController),
                    its = its.addRef(), rsRef = rs.addRef()),
                (writableRef, ac, its, rsRef), (jsg::Lock& js, jsg::Value reason) {
                  result.failureRan = true;
                  result.phantomPipe = hasPhantomPipeInQueue(*writableRef);
                })));
  });

  KJ_ASSERT(result.failureRan, "pipe should have rejected");
  KJ_ASSERT(!result.successRan, "pipe should not have resolved");
  KJ_ASSERT(!result.phantomPipe,
      "Phantom Pipe left in queue after AbortSignal — checkSignal must "
      "pop_front before sourceRef.release");
}

// A sink that accepts writes immediately and discards the data. Lets pipe
// tests exercise pipeLoop's iterative source-state checks without the IDS
// readable-side backpressure stalling the loop.
struct DiscardingSink final: public WritableStreamSink {
  kj::Promise<void> write(kj::ArrayPtr<const byte>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>>) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }
  void abort(kj::Exception) override {}
};

KJ_TEST("Source error mid-pipe — pipeLoop tryGetErrored branch") {
  // start() enqueues one chunk so HWM is satisfied and pull() is NOT called
  // eagerly at construction. pipeTo then enters pipeLoop; iter 1 reads the
  // start chunk, writes it to the DiscardingSink (which accepts synchronously),
  // and iterates. iter 2 demands more data → pull() runs → pull errors the
  // source. iter 3 hits the source.tryGetErrored branch.
  //
  // We deliberately do NOT capture the source jsg::Ref in the .then
  // continuations so the source's PipeLocked storage actually goes through
  // heap free before the pipe's async continuations run. That gives ASAN a
  // clean heap-use-after-free pre-fix in internal.c++.

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  struct {
    bool successRan = false;
    bool failureRan = false;
    kj::String failureMessage;
  } result;

  testFixture.runInIoContext([&result](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);

    auto rs = ReadableStream::constructor(js,
        UnderlyingSource{
          .start = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
      return js.resolvedPromise();
    },
          .pull = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->error(js, js.error("source-errored"));
      return js.resolvedPromise();
    },
        },
        kj::none);

    // WritableStream wrapping a DiscardingSink: an internal-backed writable
    // (so the WritableStreamInternalController code path applies) but whose
    // writes complete synchronously (so the pipe loop can iterate without
    // backpressure).
    auto writable = js.alloc<WritableStream>(
        env.context, kj::Own<WritableStreamSink>(kj::heap<DiscardingSink>()), kj::none);

    PipeToOptions opts;
    opts.preventAbort = true;

    auto pipePromise = rs->pipeTo(js, writable.addRef(), kj::mv(opts));

    return env.context.awaitJs(js,
        pipePromise.then(js,
            JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                (jsg::Lock& js) { result.successRan = true; }),
            JSG_VISITABLE_LAMBDA(
                (&result, rsRef = rs.addRef()), (rsRef), (jsg::Lock& js, jsg::Value reason) {
                  result.failureRan = true;
                  result.failureMessage = kj::str(reason.getHandle(js));
                })));
  });

  KJ_ASSERT(result.failureRan, "pipe should reject with the source error");
  KJ_ASSERT(!result.successRan, "pipe should not resolve");
  KJ_ASSERT(result.failureMessage.contains("source-errored"),
      "pipe rejection should carry the source error reason; got: ", result.failureMessage);
}

// A sink whose write() rejects with a KJ exception. Used to trigger the
// parent-errored pipeLoop branch: pipeLoop writes a chunk, the write
// rejects, the write-failure lambda calls doError on the parent and
// recurses into pipeLoop, whose next iteration finds
// parent.state == StreamStates::Errored (sites C/D).
struct FailingSink final: public WritableStreamSink {
  kj::Promise<void> write(kj::ArrayPtr<const byte>) override {
    return KJ_EXCEPTION(FAILED, "sink-write-failed");
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>>) override {
    return KJ_EXCEPTION(FAILED, "sink-write-failed");
  }
  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }
  void abort(kj::Exception) override {}
};

KJ_TEST("Parent errored during pipe — pipeLoop parent.state Errored "
        "branch, !preventCancel (site C)") {
  // start() enqueues a chunk. pipeLoop iter 1 reads it and writes to the
  // FailingSink, which rejects. The write-failure lambda calls doError on
  // the parent and recurses. pipeLoop iter 2 finds the parent Errored and
  // takes the !preventCancel branch: releases the source with the error
  // reason and returns rejectedPromise.

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  struct {
    bool successRan = false;
    bool failureRan = false;
  } result;

  {
    KJ_EXPECT_LOG(ERROR, "sink-write-failed");
    testFixture.runInIoContext([&result](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = jsg::Lock::from(env.isolate);

      auto rs = ReadableStream::constructor(js,
          UnderlyingSource{
            .start = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
        auto& c = KJ_REQUIRE_NONNULL(
            controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
        c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
        return js.resolvedPromise();
      },
          },
          kj::none);

      auto writable = js.alloc<WritableStream>(
          env.context, kj::Own<WritableStreamSink>(kj::heap<FailingSink>()), kj::none);

      PipeToOptions opts;
      // preventCancel = false (default) exercises site C.

      auto pipePromise = rs->pipeTo(js, writable.addRef(), kj::mv(opts));

      return env.context.awaitJs(js,
          pipePromise.then(js,
              JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                  (jsg::Lock& js) { result.successRan = true; }),
              JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                  (jsg::Lock& js, jsg::Value reason) { result.failureRan = true; })));
    });
  }

  KJ_ASSERT(result.failureRan, "pipe should reject with the sink error");
  KJ_ASSERT(!result.successRan, "pipe should not resolve");
}

KJ_TEST("Parent errored during pipe — pipeLoop parent.state Errored "
        "branch, preventCancel (site D)") {
  // Same as site C but with preventCancel:true. pipeLoop releases the
  // source without an error reason and returns resolvedPromise.

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  struct {
    bool successRan = false;
    bool failureRan = false;
  } result;

  {
    KJ_EXPECT_LOG(ERROR, "sink-write-failed");
    testFixture.runInIoContext([&result](const TestFixture::Environment& env) -> kj::Promise<void> {
      auto& js = jsg::Lock::from(env.isolate);

      auto rs = ReadableStream::constructor(js,
          UnderlyingSource{
            .start = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
        auto& c = KJ_REQUIRE_NONNULL(
            controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
        c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
        return js.resolvedPromise();
      },
          },
          kj::none);

      auto writable = js.alloc<WritableStream>(
          env.context, kj::Own<WritableStreamSink>(kj::heap<FailingSink>()), kj::none);

      PipeToOptions opts;
      opts.preventCancel = true;

      auto pipePromise = rs->pipeTo(js, writable.addRef(), kj::mv(opts));

      return env.context.awaitJs(js,
          pipePromise.then(js,
              JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                  (jsg::Lock& js) { result.successRan = true; }),
              JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                  (jsg::Lock& js, jsg::Value reason) { result.failureRan = true; })));
    });
  }

  // With preventCancel, the pipe promise may resolve or reject depending
  // on internal error propagation — we just care that it settles without
  // crashing (i.e. the poisoned vtable isn't hit).
  KJ_ASSERT(result.successRan || result.failureRan, "pipe should settle");
}

KJ_TEST("Source closed mid-pipe — pipeLoop source.isClosed branch, "
        "preventClose (site F)") {
  // start() enqueues a chunk. pull() closes the source. pipeLoop iter 1
  // reads the chunk, writes to DiscardingSink, and iterates. pipeLoop
  // iter 2 reads done=true (early bail). handlePromise.success runs and
  // finds the source already closed. With preventClose, the writable
  // stays open.

  capnp::MallocMessageBuilder flagsBuilder;
  auto featureFlags = flagsBuilder.initRoot<CompatibilityFlags>();
  featureFlags.setStreamsJavaScriptControllers(true);
  TestFixture testFixture({.featureFlags = featureFlags.asReader()});

  struct {
    bool successRan = false;
    bool failureRan = false;
  } result;

  testFixture.runInIoContext([&result](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = jsg::Lock::from(env.isolate);

    auto rs = ReadableStream::constructor(js,
        UnderlyingSource{
          .start = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->enqueue(js, jsg::JsArrayBuffer::create(js, 4));
      return js.resolvedPromise();
    },
          .pull = [](jsg::Lock& js, auto controller) -> jsg::Promise<void> {
      auto& c = KJ_REQUIRE_NONNULL(
          controller.template tryGet<jsg::Ref<ReadableStreamDefaultController>>());
      c->close(js);
      return js.resolvedPromise();
    },
        },
        kj::none);

    auto writable = js.alloc<WritableStream>(
        env.context, kj::Own<WritableStreamSink>(kj::heap<DiscardingSink>()), kj::none);

    PipeToOptions opts;
    opts.preventClose = true;

    auto pipePromise = rs->pipeTo(js, writable.addRef(), kj::mv(opts));

    return env.context.awaitJs(js,
        pipePromise.then(js,
            JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                (jsg::Lock& js) { result.successRan = true; }),
            JSG_VISITABLE_LAMBDA((&result, rsRef = rs.addRef()), (rsRef),
                (jsg::Lock& js, jsg::Value reason) { result.failureRan = true; })));
  });

  KJ_ASSERT(result.successRan || result.failureRan, "pipe should settle");
}

}  // namespace
}  // namespace workerd::api
