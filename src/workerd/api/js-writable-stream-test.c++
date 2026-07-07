// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-writable-stream.h>
#include <workerd/api/streams/identity-transform-stream.h>
#include <workerd/jsg/type-wrapper.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// The full wrap/unwrap template bodies are only instantiated once these types appear in
// JSG-visible signatures; until then, at least pin the SelfConvertible signatures.
static_assert(jsg::SelfConvertible<JsWritableStream>);
static_assert(jsg::SelfConvertible<JsReadableWritablePair>);

constexpr uint64_t kLimit = 1024 * 1024;
constexpr kj::StringPtr kData = "hello world"_kj;

// The user-visible message produced when performing a lock-checked operation on a stream that is
// locked to a writer. This must match the message produced by WritableStream exactly.
constexpr kj::StringPtr kWriterLockedError =
    "This WritableStream is currently locked to a writer."_kj;

// A WritableStreamSink recording its interactions into externally-owned state.
class RecordingSink final: public WritableStreamSink {
 public:
  RecordingSink(kj::Vector<kj::byte>& data, bool& ended, bool& aborted)
      : data(data),
        ended(ended),
        aborted(aborted) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    data.addAll(buffer);
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto& piece: pieces) {
      data.addAll(piece);
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> end() override {
    ended = true;
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {
    aborted = true;
  }

 private:
  kj::Vector<kj::byte>& data;
  bool& ended;
  bool& aborted;
};

// Test state bundling the externally-owned sink observations. Declared outside runInIoContext
// (which runs synchronously to completion) so the sink's references remain valid for the whole
// test.
struct SinkState {
  kj::Vector<kj::byte> written;
  bool ended = false;
  bool aborted = false;

  kj::Own<RecordingSink> makeSink() {
    return kj::heap<RecordingSink>(written, ended, aborted);
  }
};

KJ_TEST("JsWritableStream null state") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsWritableStream stream;
    KJ_EXPECT(stream.isNull());
    KJ_EXPECT(!stream.isLocked(js));
    KJ_EXPECT(!stream.isClosedOrClosing(js));
    KJ_EXPECT(stream.addRef(js).isNull());

    // setPendingClosure() and forceAbort() are teardown-path operations that tolerate null.
    stream.setPendingClosure(js);
    return env.context.awaitJs(js, stream.forceAbort(js, kj::none));
  });
}

KJ_TEST("JsWritableStream create wraps a native sink; forceClose ends it") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(!stream.isLocked(js));
    KJ_EXPECT(!stream.isClosedOrClosing(js));

    auto promise = stream.forceClose(js).then(js,
        JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream),
            (jsg::Lock& js) { KJ_EXPECT(stream.isClosedOrClosing(js)); }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST("JsWritableStream forceAbort aborts the sink") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    return env.context.awaitJs(js, stream.forceAbort(js, js.error("connection lost")));
  });
  KJ_EXPECT(state.aborted);
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsWritableStream flush resolves on an idle stream") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    return env.context.awaitJs(js, stream.flush(js));
  });
  // flush() completes pending writes but does not end the stream.
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsWritableStream flush rejects when a writer is held; forceFlush succeeds") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // Pre-lock the stream by attaching a writer to the underlying WritableStream before adopting
    // it into the abstraction.
    auto ws = js.alloc<WritableStream>(env.context, state.makeSink(), kj::none);
    auto writer = ws->getWriter(js);
    JsWritableStream stream(kj::mv(ws));
    KJ_EXPECT(stream.isLocked(js));

    auto promise = stream.flush(js)
                       .then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected flush() of a writer-locked stream to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(
          e.getDescription() == kj::str("jsg.TypeError: ", kWriterLockedError), e.getDescription());
    }).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js) {
                         // forceFlush() bypasses the writer lock.
                         return stream.forceFlush(js);
                       }));
    return env.context.awaitJs(js, kj::mv(promise)).attach(kj::mv(writer));
  });
}

KJ_TEST("JsWritableStream forceAbort succeeds despite a held writer") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto ws = js.alloc<WritableStream>(env.context, state.makeSink(), kj::none);
    auto writer = ws->getWriter(js);
    JsWritableStream stream(kj::mv(ws));
    KJ_EXPECT(stream.isLocked(js));

    return env.context.awaitJs(js, stream.forceAbort(js, kj::none)).attach(kj::mv(writer));
  });
  KJ_EXPECT(state.aborted);
}

KJ_TEST("JsWritableStream forceClose succeeds despite a held writer") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto ws = js.alloc<WritableStream>(env.context, state.makeSink(), kj::none);
    auto writer = ws->getWriter(js);
    JsWritableStream stream(kj::mv(ws));
    KJ_EXPECT(stream.isLocked(js));

    return env.context.awaitJs(js, stream.forceClose(js)).attach(kj::mv(writer));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream detach neutralizes the stream without ending the sink") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    stream.detach(js);

    // The stream is left permanently locked and closed to further writes...
    KJ_EXPECT(stream.isLocked(js));
    KJ_EXPECT(stream.isClosedOrClosing(js));
  });
  // ...but detach is a takeover, not a close: the sink is dropped without end().
  KJ_EXPECT(!state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST("JsWritableStream detach throws when a writer is held") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto ws = js.alloc<WritableStream>(env.context, state.makeSink(), kj::none);
    auto writer = ws->getWriter(js);
    JsWritableStream stream(kj::mv(ws));

    js.tryCatch([&] {
      stream.detach(js);
      KJ_FAIL_REQUIRE("expected detach() of a writer-locked stream to throw");
    }, [&](jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains(kWriterLockedError), e.getDescription());
    });
  });
}

KJ_TEST("JsWritableStream detach of a closed stream throws") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise =
        stream.forceClose(js).then(js,
            JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js) {
              KJ_EXPECT(stream.isClosedOrClosing(js));
              js.tryCatch([&] {
                stream.detach(js);
                KJ_FAIL_REQUIRE("expected detach() of a closed stream to throw");
              }, [&](jsg::Value exception) {
                auto e = js.exceptionToKj(kj::mv(exception));
                KJ_EXPECT(e.getDescription().contains("This WritableStream is closed."),
                    e.getDescription());
              });
            }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsWritableStream addRef shares the underlying stream") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto ref = stream.addRef(js);
    KJ_EXPECT(!ref.isNull());
    KJ_EXPECT(!ref.isClosedOrClosing(js));

    // Closing through the addRef closes the original: both wrap the same stream.
    auto promise = ref.forceClose(js).then(js,
        JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream),
            (jsg::Lock& js) { KJ_EXPECT(stream.isClosedOrClosing(js)); }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream setPendingClosure is safe on live and null streams") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsWritableStream().setPendingClosure(js);  // null: no-op

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    stream.setPendingClosure(js);
    KJ_EXPECT(!stream.isNull());
  });
}

// Build a JsReadableWritablePair from the two ends of an IdentityTransformStream, the way the
// two-tier unwrap's brand-first path would.
JsReadableWritablePair makeIdentityPair(jsg::Lock& js) {
  auto transform = IdentityTransformStream::constructor(js);
  return JsReadableWritablePair{
    .readable = JsReadableStream(transform->getReadable()),
    .writable = JsWritableStream(transform->getWritable()),
  };
}

KJ_TEST("JsReadableStream pipeTo pipes into a JsWritableStream and closes it") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = source.pipeTo(js, destination);
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsReadableStream pipeTo with preventClose leaves the destination open") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = source.pipeTo(js, destination, PipeToOptions{.preventClose = true});
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsReadableStream pipeTo rejects when the source is locked") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // detach() leaves the original as a locked husk, which is a convenient way to produce a
    // locked stream without a reader.
    JsReadableStream source(js, kj::str(kData));
    auto detached = source.detach(js);
    KJ_EXPECT(source.isLocked(js));

    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = source.pipeTo(js, destination).then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() from a locked source to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("currently locked to a reader"), e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsReadableStream pipeTo rejects when the destination is locked") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto ws = js.alloc<WritableStream>(env.context, state.makeSink(), kj::none);
    auto writer = ws->getWriter(js);
    JsWritableStream destination(kj::mv(ws));

    auto promise = source.pipeTo(js, destination).then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() into a locked destination to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      // Note: unlike every other occurrence of this message, ReadableStream::pipeTo's
      // destination-locked rejection has no trailing period. That text is load-bearing.
      KJ_EXPECT(e.getDescription() ==
              "jsg.TypeError: This WritableStream is currently locked to a writer"_kj,
          e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise)).attach(kj::mv(writer));
  });
}

KJ_TEST("JsReadableStream pipeThrough pipes through an identity transform") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto result = source.pipeThrough(js, makeIdentityPair(js));
    KJ_EXPECT(!result.isNull());
    // The source is now locked into the pipe.
    KJ_EXPECT(source.isLocked(js));

    auto promise = result.text(js, kLimit).then(js, [](jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream pipeThrough composes with pipeTo") {
  // The readable returned by pipeThrough() feeds directly into further pipeline calls:
  // source.pipeThrough(transform).pipeTo(destination). (Chaining two IdentityTransformStreams
  // back to back is NOT covered here: the legacy C++ implementation rejects direct
  // inter-IdentityTransformStream pipes -- "Inter-TransformStream ReadableStream.pipeTo() is
  // not implemented", identity-transform-stream.c++ -- a pre-existing implementation
  // limitation, identical in JavaScript, and not this abstraction's behavior to pin.)
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = source.pipeThrough(js, makeIdentityPair(js)).pipeTo(js, destination);
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsReadableStream pipeThrough throws synchronously when the source is locked") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));
    auto detached = source.detach(js);
    KJ_EXPECT(source.isLocked(js));

    js.tryCatch([&] {
      source.pipeThrough(js, makeIdentityPair(js));
      KJ_FAIL_REQUIRE("expected pipeThrough() from a locked source to throw");
    }, [&](jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("currently locked to a reader"), e.getDescription());
    });
  });
}

KJ_TEST("JsReadableStream pipeThrough throws synchronously when the transform writable is locked") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsReadableStream source(js, kj::str(kData));

    auto transform = IdentityTransformStream::constructor(js);
    auto writable = transform->getWritable();
    auto writer = writable->getWriter(js);
    JsReadableWritablePair pair{
      .readable = JsReadableStream(transform->getReadable()),
      .writable = JsWritableStream(kj::mv(writable)),
    };

    js.tryCatch([&] {
      source.pipeThrough(js, kj::mv(pair));
      KJ_FAIL_REQUIRE("expected pipeThrough() into a locked transform writable to throw");
    }, [&](jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      // pipeThrough's synchronous destination-locked message carries the trailing period,
      // unlike pipeTo's rejection.
      KJ_EXPECT(e.getDescription().contains(kWriterLockedError), e.getDescription());
    });
  });
}

KJ_TEST("JsWritableStream create honors the closure waitable") {
  // Note: create()'s maybeHighWaterMark is likewise a pass-through to the internal controller;
  // its backpressure behavior is not observable through the abstraction's narrow API and is
  // pinned by streams/internal-test.c++ and the socket tests.
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto prp = js.newPromiseAndResolver<void>();
    auto stream = JsWritableStream::create(
        js, env.context, state.makeSink(), kj::none, kj::none, kj::mv(prp.promise));

    auto closePromise = stream.forceClose(js);
    // The closure waitable has not resolved, so the close must not have completed (the internal
    // controller's closeImpl is gated on the waitable and cannot have run yet).
    KJ_EXPECT(!state.ended);

    prp.resolver.resolve(js);
    return env.context.awaitJs(js, kj::mv(closePromise));
  });
  KJ_EXPECT(state.ended);
}

}  // namespace
}  // namespace workerd::api
