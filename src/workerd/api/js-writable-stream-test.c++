// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-streams-bridge.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/api/streams/identity-transform-stream.h>
#include <workerd/io/per-isolate-bootstrap.h>
#include <workerd/jsg/type-wrapper.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

// The full wrap/unwrap template bodies are only instantiated once these types appear in
// JSG-visible signatures; until then, at least pin the SelfConvertible signatures.
static_assert(jsg::SelfConvertible<JsWritableStream>);
static_assert(jsg::SelfConvertible<JsReadableWritablePair>);

constexpr uint64_t kLimit = 1024 * 1024;
constexpr kj::StringPtr kData = "hello world"_kj;
constexpr kj::StringPtr kMoreData = " and goodbye"_kj;

// A WritableStreamSink recording its interactions into externally-owned state.
class RecordingSink final: public WritableStreamSink {
 public:
  RecordingSink(kj::Vector<kj::byte>& data, bool& ended, bool& aborted, bool& destroyed)
      : data(data),
        ended(ended),
        aborted(aborted),
        destroyed(destroyed) {}

  ~RecordingSink() noexcept(false) {
    destroyed = true;
  }

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
  bool& destroyed;
};

// Test state bundling the externally-owned sink observations. Declared outside runInIoContext
// (which runs synchronously to completion) so the sink's references remain valid for the whole
// test.
struct SinkState {
  kj::Vector<kj::byte> written;
  bool ended = false;
  bool aborted = false;
  bool destroyed = false;

  kj::Own<RecordingSink> makeSink() {
    return kj::heap<RecordingSink>(written, ended, aborted, destroyed);
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
    // A null stream is backed by neither arm.
    KJ_EXPECT(stream.tryGetLegacy(js) == kj::none);
    KJ_EXPECT(stream.tryGetTs(js) == kj::none);

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

    auto promise = stream.forceClose(js).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      KJ_EXPECT(stream.isClosedOrClosing(js));
    });
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
                       .then(js, [](jsg::Lock& js) mutable {
      KJ_FAIL_REQUIRE("expected flush() of a writer-locked stream to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("locked"), e.getDescription());
    }).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      // forceFlush() bypasses the writer lock.
      return stream.forceFlush(js);
    });
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

    // ...and the sink is released immediately (a takeover leaves nothing behind to hold
    // the taken-over connection), not at some later GC.
    KJ_EXPECT(state.destroyed);
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

    JSG_TRY(js) {
      stream.detach(js);
      KJ_FAIL_REQUIRE("expected detach() of a writer-locked stream to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("locked"), e.getDescription());
    };
  });
}

KJ_TEST("JsWritableStream detach of a closed stream throws") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = stream.forceClose(js).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      KJ_EXPECT(stream.isClosedOrClosing(js));
      JSG_TRY(js) {
        stream.detach(js);
        KJ_FAIL_REQUIRE("expected detach() of a closed stream to throw");
      }
      JSG_CATCH(exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(
            e.getDescription().contains("This WritableStream is closed."), e.getDescription());
      };
    });
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
    auto promise = ref.forceClose(js).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      KJ_EXPECT(stream.isClosedOrClosing(js));
    });
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
    // pipeTo() self-retains the source; the destination must still outlive the pipe because the
    // pipe's write loop runs inside the writable controller.
    auto promise =
        source.pipeTo(js, destination).then(js, [destination = kj::mv(destination)](jsg::Lock& js) {
    });
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
    auto promise = source.pipeTo(js, destination, PipeToOptions{.preventClose = true})
                       .then(js, [destination = kj::mv(destination)](jsg::Lock& js) {});
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
      KJ_EXPECT(e.getDescription().contains("locked to a reader"), e.getDescription());
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
      KJ_EXPECT(e.getDescription().contains("locked"), e.getDescription());
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
    auto piped = source.pipeThrough(js, makeIdentityPair(js));
    // pipeTo() self-retains the piped source; pipeThrough() self-retains the original source
    // via JSG_THIS; only the destination needs explicit keepalive.
    auto promise =
        piped.pipeTo(js, destination).then(js, [destination = kj::mv(destination)](jsg::Lock& js) {
    });
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

    JSG_TRY(js) {
      source.pipeThrough(js, makeIdentityPair(js));
      KJ_FAIL_REQUIRE("expected pipeThrough() from a locked source to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("locked to a reader"), e.getDescription());
    };
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

    JSG_TRY(js) {
      source.pipeThrough(js, kj::mv(pair));
      KJ_FAIL_REQUIRE("expected pipeThrough() into a locked transform writable to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      // pipeThrough's synchronous destination-locked message carries the trailing period,
      // unlike pipeTo's rejection.
      KJ_EXPECT(e.getDescription().contains("locked"), e.getDescription());
    };
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

// =======================================================================================
// TypeScript-backed streams

// Builds a TestFixture in which the TypeScript streams implementation is active: the
// typescript_implemented_streams compat flag plus the per-isolate bootstrap autogate.
TestFixture makeTsStreamsFixture() {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  return TestFixture({
    .featureFlags = flags.asReader(),
    .autogates = kj::arr("per-isolate-javascript-bootstrap"_kj),
  });
}

// A ReadableStreamSource serving fixed content with a known length (for pipe tests).
class ContentSource final: public ReadableStreamSource {
 public:
  ContentSource(kj::StringPtr data): data(data) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto amount = kj::min(maxBytes, data.size() - offset);
    kj::arrayPtr(static_cast<kj::byte*>(buffer), amount)
        .copyFrom(data.slice(offset, offset + amount).asBytes());
    offset += amount;
    return amount;
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return data.size() - offset;
    }
    return kj::none;
  }

 private:
  kj::StringPtr data;
  size_t offset = 0;
};

// A WritableStreamSink whose writes complete only when the externally-owned gate is
// fulfilled. Used to observe operations parked behind an in-flight write.
class GatedSink final: public WritableStreamSink {
 public:
  GatedSink(kj::Vector<kj::byte>& data, kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>>& gate)
      : data(data),
        gate(gate) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    data.addAll(buffer);
    auto paf = kj::newPromiseAndFulfiller<void>();
    gate = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto& piece: pieces) {
      data.addAll(piece);
    }
    auto paf = kj::newPromiseAndFulfiller<void>();
    gate = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }

  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {}

 private:
  kj::Vector<kj::byte>& data;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>>& gate;
};

// A ContentSource variant recording whether cancel() reached the underlying source (for
// preventCancel pipe tests).
class CancelRecordingSource final: public ReadableStreamSource {
 public:
  CancelRecordingSource(kj::StringPtr data, bool& cancelled): data(data), cancelled(cancelled) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto amount = kj::min(maxBytes, data.size() - offset);
    kj::arrayPtr(static_cast<kj::byte*>(buffer), amount)
        .copyFrom(data.slice(offset, offset + amount).asBytes());
    offset += amount;
    return amount;
  }

  void cancel(kj::Exception reason) override {
    cancelled = true;
  }

 private:
  kj::StringPtr data;
  bool& cancelled;
  size_t offset = 0;
};

// A WritableStreamSink whose writes always fail (for pipe failure-path tests).
class FailingSink final: public WritableStreamSink {
 public:
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return KJ_EXCEPTION(FAILED, "jsg.Error: write failed");
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    return KJ_EXCEPTION(FAILED, "jsg.Error: write failed");
  }

  kj::Promise<void> end() override {
    return KJ_EXCEPTION(FAILED, "jsg.Error: end failed");
  }

  void abort(kj::Exception reason) override {}
};

KJ_TEST("WritableStreamNativeSink instances carry the kNativeSink marker") {
  TestFixture testFixture;
  SinkState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // Wrap an instance via the Lock-acquired TypeHandler -- the same path create()'s
    // TypeScript arm uses -- then verify the contract's detection shape: an own data
    // property keyed by the kNativeSink API-registry symbol whose value is the symbol
    // itself.
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<WritableStreamNativeSink>>());
    auto handle = handler.wrap(
        js, js.alloc<WritableStreamNativeSink>(env.context, state.makeSink(), kj::none, kj::none));
    auto obj = KJ_ASSERT_NONNULL(jsg::JsValue(handle).tryCast<jsg::JsObject>());

    auto symbol = jsg::JsValue(
        v8::Symbol::ForApi(js.v8Isolate, jsg::v8StrIntern(js.v8Isolate, "kNativeSink")));
    KJ_EXPECT(obj.has(js, symbol, jsg::JsObject::HasOption::OWN));
    KJ_EXPECT(obj.get(js, symbol) == symbol);
  });
}

KJ_TEST("JsWritableStream create under the flag produces a TypeScript-backed stream") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(stream.tryGetLegacy(js) == kj::none);
    KJ_EXPECT(stream.tryGetTs(js) != kj::none);
    KJ_EXPECT(!stream.isLocked(js));
    KJ_EXPECT(!stream.isClosedOrClosing(js));

    // forceClose drives the native sink's close hook; the query flips through the TS arm.
    auto promise = stream.forceClose(js).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      KJ_EXPECT(stream.isClosedOrClosing(js));
      // The forcible close is idempotent, matching the legacy controller.
      return stream.forceClose(js);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST("JsWritableStream forceAbort aborts a TypeScript-backed stream's sink") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise = stream.forceAbort(js, js.error("connection lost"))
                       .then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      // Aborting an already-errored stream is a no-op (resolved), matching the legacy arm.
      return stream.forceAbort(js, kj::none);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.aborted);
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsWritableStream flush TS arm: locked rejection, terminal-state rejections") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);

    // flush() on an idle stream resolves.
    auto promise = stream.flush(js).then(js, [stream = stream.addRef(js)](jsg::Lock& js) mutable {
      // Acquire a writer through JS; the lock-checked flush must reject with the exact
      // legacy text.
      auto handle = KJ_ASSERT_NONNULL(stream.tryGetTs(js));
      auto writer = KJ_ASSERT_NONNULL(
          webstreams::invokeMethod(js, handle, "getWriter"_kj).tryCast<jsg::JsObject>());
      KJ_EXPECT(stream.isLocked(js));

      auto locked = stream.flush(js).then(js, [](jsg::Lock& js) {
        KJ_FAIL_REQUIRE("expected flush() of a writer-locked stream to reject");
      }, [](jsg::Lock& js, jsg::Value exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(
            e.getDescription().contains("This WritableStream is currently locked to a writer."),
            e.getDescription());
      });

      // forceFlush() bypasses the writer lock.
      return locked
          .then(js,
              [stream = stream.addRef(js)](jsg::Lock& js) mutable { return stream.forceFlush(js); })
          .then(
              js, [stream = stream.addRef(js), writer = writer.addRef(js)](jsg::Lock& js) mutable {
        // Release the writer so the closed-stream flush below exercises the terminal-state
        // rejection rather than the locked one (the composed flush checks the lock first).
        auto releaseResult KJ_UNUSED =
            webstreams::invokeMethod(js, writer.getHandle(js), "releaseLock"_kj);
        return stream.forceClose(js);
      }).then(js, [stream = stream.addRef(js)](jsg::Lock& js) mutable {
        // flush of a closed stream rejects with the legacy text.
        return stream.flush(js).then(js, [](jsg::Lock& js) {
          KJ_FAIL_REQUIRE("expected flush() of a closed stream to reject");
        }, [](jsg::Lock& js, jsg::Value exception) {
          auto e = js.exceptionToKj(kj::mv(exception));
          KJ_EXPECT(e.getDescription().contains("This WritableStream has been closed."),
              e.getDescription());
        });
      });
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream flush TS arm parks behind an in-flight write") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> written;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> gate;
  // Observed by continuations that run after the runInIoContext body has returned, so it
  // must live at test scope (the body's stack frame is gone by then).
  bool flushed = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream =
        JsWritableStream::create(js, env.context, kj::heap<GatedSink>(written, gate), kj::none);
    auto handle = KJ_ASSERT_NONNULL(stream.tryGetTs(js));
    auto writer = KJ_ASSERT_NONNULL(
        webstreams::invokeMethod(js, handle, "getWriter"_kj).tryCast<jsg::JsObject>());
    auto writeResult = webstreams::invokeMethod(
        js, writer, "write"_kj, jsg::JsUint8Array::create(js, kData.asBytes()));
    KJ_ASSERT_NONNULL(writeResult.tryCast<jsg::JsPromise>());

    // The flush is positional: its marker is queued behind the (gated) write.
    auto flushPromise =
        stream.forceFlush(js).then(js, [&flushed](jsg::Lock& js) { flushed = true; });

    // A KJ roundtrip drains the microtask queue, so by the continuation the write has
    // reached the (gated) sink while the flush is still parked.
    auto sequence = env.context
                        .awaitIo(js, kj::Promise<void>(kj::READY_NOW),
                            [&written, &gate, &flushed](jsg::Lock& js) {
      KJ_EXPECT(written.asPtr() == kData.asBytes());
      KJ_EXPECT(!flushed);
      KJ_ASSERT_NONNULL(gate)->fulfill();
    })
                        .then(js,
                            [flushPromise = kj::mv(flushPromise), writer = writer.addRef(js),
                                stream = kj::mv(stream)](
                                jsg::Lock& js) mutable { return kj::mv(flushPromise); });
    auto done = sequence.then(js, [&flushed](jsg::Lock& js) { KJ_EXPECT(flushed); });
    return env.context.awaitJs(js, kj::mv(done));
  });
  KJ_EXPECT(written.asPtr() == kData.asBytes());
}

KJ_TEST("WritableStreamNativeSink write hook: strings and non-byte chunks") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto handle = KJ_ASSERT_NONNULL(stream.tryGetTs(js));
    auto writer = KJ_ASSERT_NONNULL(
        webstreams::invokeMethod(js, handle, "getWriter"_kj).tryCast<jsg::JsObject>());

    // Strings are written as UTF-8 (the legacy controller's ergonomic extension).
    auto writeResult = webstreams::invokeMethod(js, writer, "write"_kj, js.str(kData));
    auto writePromise = KJ_ASSERT_NONNULL(writeResult.tryCast<jsg::JsPromise>());
    auto promise = js.toVoidPromise(writePromise)
                       .then(js,
                           [&state, writer = writer.addRef(js), stream = kj::mv(stream)](
                               jsg::Lock& js) mutable {
      KJ_EXPECT(state.written.asPtr() == kData.asBytes());

      // A non-byte chunk rejects the write with the legacy byte-types error.
      auto badResult =
          webstreams::invokeMethod(js, writer.getHandle(js), "write"_kj, jsg::JsValue(js.obj()));
      auto badPromise = KJ_ASSERT_NONNULL(badResult.tryCast<jsg::JsPromise>());
      return js.toVoidPromise(badPromise).then(js, [](jsg::Lock& js) {
        KJ_FAIL_REQUIRE("expected a non-byte write to reject");
      }, [](jsg::Lock& js, jsg::Value exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(
            e.getDescription().contains("only supports writing byte types"), e.getDescription());
      });
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsWritableStream detach TS arm neutralizes and enforces preconditions") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // detach() neutralizes: the stream is left permanently locked, without touching the
    // sink (a takeover, not a close).
    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    stream.detach(js);
    KJ_EXPECT(stream.isLocked(js));

    // The sink is released immediately (legacy parity): the controller's stored hook
    // algorithms still reference the WritableStreamNativeSink wrapper until the stream
    // is GC'd, but the C++ sink it owned must not survive the takeover.
    KJ_EXPECT(state.destroyed);

    // detach() of a locked stream throws the exact legacy text (including the period).
    auto locked = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto handle = KJ_ASSERT_NONNULL(locked.tryGetTs(js));
    auto writer KJ_UNUSED = webstreams::invokeMethod(js, handle, "getWriter"_kj);
    JSG_TRY(js) {
      locked.detach(js);
      KJ_FAIL_REQUIRE("expected detach() of a writer-locked stream to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("This WritableStream is currently locked to a writer."),
          e.getDescription());
    };
  });
  KJ_EXPECT(!state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST("JsWritableStream detach TS arm: closed and errored streams throw") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  SinkState errorState;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // NOTE: both streams are created and both terminal transitions initiated HERE, in the
    // body: the TestFixture only guarantees the IoContext is current for the body's own
    // turn (and the awaitIo machinery's continuations), and create()/the native sink
    // hooks require it. The detach precondition checks chained below are pure JS dispatch
    // and are safe in any later microtask drain.
    auto closed = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto errored = JsWritableStream::create(js, env.context, errorState.makeSink(), kj::none);
    auto closePromise = closed.forceClose(js);
    auto abortPromise = errored.forceAbort(js, js.error("boom"));

    auto promise = closePromise
                       .then(js,
                           [closed = kj::mv(closed)](jsg::Lock& js) mutable {
      JSG_TRY(js) {
        closed.detach(js);
        KJ_FAIL_REQUIRE("expected detach() of a closed stream to throw");
      }
      JSG_CATCH(exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(
            e.getDescription().contains("This WritableStream is closed."), e.getDescription());
      };
    })
                       .then(js, [abortPromise = kj::mv(abortPromise)](jsg::Lock& js) mutable {
      return kj::mv(abortPromise);
    }).then(js, [errored = kj::mv(errored)](jsg::Lock& js) mutable {
      // detach() of an errored stream throws the stored error.
      JSG_TRY(js) {
        errored.detach(js);
        KJ_FAIL_REQUIRE("expected detach() of an errored stream to throw");
      }
      JSG_CATCH(exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(e.getDescription().contains("boom"), e.getDescription());
      };
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(errorState.aborted);
}

KJ_TEST("JsWritableStream addRef shares a TypeScript-backed stream") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto ref = stream.addRef(js);
    KJ_EXPECT(!ref.isNull());

    // Both handles reference the same JS object...
    auto handle1 = KJ_ASSERT_NONNULL(stream.tryGetTs(js));
    auto handle2 = KJ_ASSERT_NONNULL(ref.tryGetTs(js));
    KJ_EXPECT(jsg::JsValue(handle1) == jsg::JsValue(handle2));

    // ...so state changes through one are visible through the other.
    auto promise = ref.forceClose(js).then(js, [stream = kj::mv(stream)](jsg::Lock& js) mutable {
      KJ_EXPECT(stream.isClosedOrClosing(js));
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream setPendingClosure gates writes but not the teardown operations") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    stream.setPendingClosure(js);
    // Pending closure is separate state, not closed-or-closing.
    KJ_EXPECT(!stream.isClosedOrClosing(js));

    // New writes fail fast with the legacy text (legacy internal-controller parity: only
    // write() is gated)...
    auto handle = KJ_ASSERT_NONNULL(stream.tryGetTs(js));
    auto writer = KJ_ASSERT_NONNULL(
        webstreams::invokeMethod(js, handle, "getWriter"_kj).tryCast<jsg::JsObject>());
    auto writePromise = js.toPromise(v8::Local<v8::Value>(KJ_ASSERT_NONNULL(JSG_TRY_CAST_PROMISE(
        webstreams::invokeMethod(js, writer, "write"_kj, js.str("data"_kj))))));

    auto promise =
        writePromise
            .then(js, [](jsg::Lock& js, jsg::Value) -> void {
      KJ_FAIL_REQUIRE("expected write() after setPendingClosure to reject");
    }, [](jsg::Lock& js, jsg::Value exception) -> void {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(
          e.getDescription().contains("This WritableStream belongs to an object that is closing"),
          e.getDescription());
    }).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock & js) mutable {
              // ...while the teardown's own operations stay open: forceFlush and forceClose
              // proceed (they bypass the writer lock and the pending-closure gate by design).
              auto flushed = stream.forceFlush(js);
              return flushed.then(js,
                  JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream),
                      (jsg::Lock & js) mutable { return stream.forceClose(js); }));
            }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream create TS arm honors the closure waitable") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto prp = js.newPromiseAndResolver<void>();
    auto stream = JsWritableStream::create(
        js, env.context, state.makeSink(), kj::none, kj::none, kj::mv(prp.promise));

    auto closePromise = stream.forceClose(js);
    // The closure waitable has not resolved, so the close must not have completed (the
    // native sink's close hook is gated on the waitable and cannot have run yet).
    KJ_EXPECT(!state.ended);

    prp.resolver.resolve(js);
    return env.context.awaitJs(js, kj::mv(closePromise));
  });
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsWritableStream create TS arm: closure waitable rejection skips the sink end") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto prp = js.newPromiseAndResolver<void>();
    auto stream = JsWritableStream::create(
        js, env.context, state.makeSink(), kj::none, kj::none, kj::mv(prp.promise));

    auto closePromise = stream.forceClose(js);
    // A rejected closure waitable resolves the close WITHOUT
    // ending the sink (the failure is reported through the owning object's own promises).
    prp.resolver.reject(js, js.error("connect failed"));
    return env.context.awaitJs(js, kj::mv(closePromise));
  });
  KJ_EXPECT(!state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST("JsWritableStream::tryUnwrapTs adopts TypeScript streams and rejects impostors") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
    auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
    auto constructor =
        KJ_ASSERT_NONNULL(exportsObj.get(js, "WritableStream"_kj).tryCast<jsg::JsFunction>());
    auto streamObj = constructor.newInstance(js, jsg::JsValue(js.obj()));

    // A genuine TypeScript stream unwraps; unwrap ADOPTS the same underlying stream.
    auto unwrapped = KJ_ASSERT_NONNULL(JsWritableStream::tryUnwrapTs(js, jsg::JsValue(streamObj)));
    KJ_EXPECT(!unwrapped.isNull());
    KJ_EXPECT(!unwrapped.isLocked(js));
    auto writer KJ_UNUSED = webstreams::invokeMethod(js, streamObj, "getWriter"_kj);
    KJ_EXPECT(unwrapped.isLocked(js));

    // Unwrap has no locked precondition (legacy parity): a locked stream still unwraps.
    auto unwrappedWhileLocked =
        KJ_ASSERT_NONNULL(JsWritableStream::tryUnwrapTs(js, jsg::JsValue(streamObj)));
    KJ_EXPECT(unwrappedWhileLocked.isLocked(js));

    // Impostors do not unwrap: plain objects, primitives, and JSG resource objects that
    // are not streams all fail the brand check.
    KJ_EXPECT(JsWritableStream::tryUnwrapTs(js, jsg::JsValue(js.obj())) == kj::none);
    KJ_EXPECT(JsWritableStream::tryUnwrapTs(js, js.str("stream"_kj)) == kj::none);
    KJ_EXPECT(JsWritableStream::tryUnwrapTs(js, js.num(42.0)) == kj::none);
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<WritableStreamNativeSink>>());
    auto sinkObj = jsg::JsValue(handler.wrap(
        js, js.alloc<WritableStreamNativeSink>(env.context, state.makeSink(), kj::none, kj::none)));
    KJ_EXPECT(JsWritableStream::tryUnwrapTs(js, sinkObj) == kj::none);
  });
}

KJ_TEST("JsWritableStream::tryUnwrapTs is inert without the compat flag") {
  TestFixture testFixture;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    // Without the flag there is no TypeScript implementation and no bootstrap export;
    // the guard must return none rather than reaching for the absent export.
    KJ_EXPECT(JsWritableStream::tryUnwrapTs(js, jsg::JsValue(js.obj())) == kj::none);
  });
}

KJ_TEST("JsReadableStream pipeTo runs a native+native TypeScript pipe at the C++ layer") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // Both ends are TS-backed native streams, so the TS pipeTo's dispatch extracts both
    // and runs the pump through WritableStreamNativeSink::pipeFrom.
    auto source = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise =
        source.pipeTo(js, destination).then(js, [destination = kj::mv(destination)](jsg::Lock& js) {
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(state.ended);
  KJ_EXPECT(!state.aborted);
}

KJ_TEST(
    "JsReadableStream pipeTo preventClose leaves the destination usable (socket concatenation)") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    auto source2 = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kMoreData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);

    // A prevent* option routes the pipe through the JS pump: extraction would consume
    // both endpoints permanently, but the legacy pipe contract leaves the un-prevented
    // endpoint usable after the pipe settles.
    auto promise = source.pipeTo(js, destination, PipeToOptions{.preventClose = true})
                       .then(js,
                           [destination = destination.addRef(js), source2 = kj::mv(source2)](
                               jsg::Lock& js) mutable {
      // The destination is unlocked and still writable after the pipe...
      KJ_EXPECT(!destination.isLocked(js));
      KJ_EXPECT(!destination.isClosedOrClosing(js));
      // ...so a second pipe can append onto the same sink (the socket-concatenation
      // pattern). Without options this one takes the native+native fast path and ends
      // the sink.
      return source2.pipeTo(js, destination);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  auto expected = kj::str(kData, kMoreData);
  KJ_EXPECT(state.written.asPtr() == expected.asBytes());
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsReadableStream pipeTo preventCancel leaves the source usable after a failed pipe") {
  auto fixture = makeTsStreamsFixture();
  bool cancelled = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = JsReadableStream::create(
        js, env.context, kj::heap<CancelRecordingSource>(kData, cancelled));
    auto destination = JsWritableStream::create(js, env.context, kj::heap<FailingSink>(), kj::none);

    // A prevent* option routes the pipe through the JS pump; the failing destination
    // rejects the pipe, and preventCancel leaves the source untouched: unlocked and
    // uncancelled.
    auto promise = source.pipeTo(js, destination, PipeToOptions{.preventCancel = true})
                       .then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() into a failing sink to reject");
    }, [source = source.addRef(js)](jsg::Lock& js, jsg::Value exception) mutable {
      KJ_EXPECT(!source.isLocked(js));
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(!cancelled);
}

KJ_TEST("JsReadableStream pipeTo without preventCancel cancels the source on a failed pipe") {
  auto fixture = makeTsStreamsFixture();
  bool cancelled = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = JsReadableStream::create(
        js, env.context, kj::heap<CancelRecordingSource>(kData, cancelled));
    auto destination = JsWritableStream::create(js, env.context, kj::heap<FailingSink>(), kj::none);

    // Control leg: with no options this is the native+native fast path; on failure the
    // pump cancels the source and the pipe rejects with the write error.
    auto promise = source.pipeTo(js, destination).then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() into a failing sink to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("write failed"), e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(cancelled);
}

KJ_TEST("JsReadableStream pipeTo validates options before consuming the endpoints") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // A bogus signal cannot be expressed through the typed C++ PipeToOptions, so build
    // the native-backed TS stream the same way create()'s TypeScript arm does -- keeping
    // the raw handle -- and invoke its pipeTo the way user code would.
    auto& sourceHandler =
        KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto sourceObj = jsg::JsValue(sourceHandler.wrap(
        js, js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData))));
    auto constructor = webstreams::getCppExport(js, "ReadableStream");
    auto sourceHandle = constructor.newInstance(js, sourceObj);
    auto source = JsReadableStream(js, sourceHandle.addRef(js));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto destHandle = KJ_ASSERT_NONNULL(destination.tryGetTs(js));
    auto badOptions = js.obj();
    badOptions.set(js, "signal"_kj, js.num(42));
    auto rejected = KJ_ASSERT_NONNULL(
        webstreams::invokeMethod(js, sourceHandle, "pipeTo"_kj, destHandle, badOptions)
            .tryCast<jsg::JsPromise>());

    auto promise = js.toVoidPromise(rejected).then(js,
        [](jsg::Lock& js) -> jsg::Promise<void> {
      KJ_FAIL_REQUIRE("expected pipeTo() with a non-AbortSignal signal to reject");
    },
        [source = source.addRef(js), destination = destination.addRef(js)](
            jsg::Lock& js, jsg::Value exception) mutable -> jsg::Promise<void> {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(
          e.getDescription().contains("options.signal must be an AbortSignal"), e.getDescription());
      // Validation ran BEFORE extraction: both endpoints are untouched (not locked, not
      // disturbed) and the pipe can be retried successfully.
      KJ_EXPECT(!source.isLocked(js));
      KJ_EXPECT(!source.isDisturbed(js));
      KJ_EXPECT(!destination.isLocked(js));
      return source.pipeTo(js, destination);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(state.ended);
}

KJ_TEST("JsReadableStream pipeTo into an errored destination rejects with the stored error") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);

    // Error the destination, then pipe into it. The fast-path gate requires a 'writable'
    // destination, so this routes to the JS pump, which rejects with the destination's
    // STORED error (spec behavior) rather than a generic closed-stream TypeError.
    auto promise = destination.forceAbort(js, js.error("boom"))
                       .then(js,
                           [source = kj::mv(source), destination = destination.addRef(js)](
                               jsg::Lock& js) mutable {
      return source.pipeTo(js, destination);
    }).then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() into an errored destination to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("boom"), e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.aborted);
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsReadableStream pipeTo native+native with a pre-aborted signal shuts both ends down") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto abortController = AbortController::constructor(js);
    abortController->abort(js, kj::none);

    auto promise =
        source.pipeTo(js, destination, PipeToOptions{.signal = abortController->getSignal()})
            .then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pipeTo() with a pre-aborted signal to reject");
    }, [destination = kj::mv(destination)](jsg::Lock& js, jsg::Value exception) {});
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.size() == 0);
  KJ_EXPECT(state.aborted);
  KJ_EXPECT(!state.ended);
}

KJ_TEST("JsReadableStream pipeTo drives a queued TypeScript source into a native sink") {
  auto fixture = makeTsStreamsFixture();
  SinkState state;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // A plain JS underlying source (the queued backend) piped into a native-backed
    // writable: no extraction fast path, so the TS JS pump drives the native sink's
    // write hook chunk by chunk -- the socket-response shape.
    auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
    auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
    auto constructor =
        KJ_ASSERT_NONNULL(exportsObj.get(js, "ReadableStream"_kj).tryCast<jsg::JsFunction>());
    auto underlying = js.obj();
    underlying.set(js, "start"_kj,
        jsg::JsValue(js.wrapSimpleFunction(
            js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
      auto controller = KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsObject>());
      auto enqueue = KJ_ASSERT_NONNULL(controller.get(js, "enqueue"_kj).tryCast<jsg::JsFunction>());
      enqueue.call(js, controller, jsg::JsUint8Array::create(js, kData.asBytes()));
      auto close = KJ_ASSERT_NONNULL(controller.get(js, "close"_kj).tryCast<jsg::JsFunction>());
      close.call(js, controller);
    })));
    auto source =
        JsReadableStream(js, constructor.newInstance(js, jsg::JsValue(underlying)).addRef(js));

    auto destination = JsWritableStream::create(js, env.context, state.makeSink(), kj::none);
    auto promise =
        source.pipeTo(js, destination).then(js, [destination = kj::mv(destination)](jsg::Lock& js) {
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(state.written.asPtr() == kData.asBytes());
  KJ_EXPECT(state.ended);
}

}  // namespace
}  // namespace workerd::api
