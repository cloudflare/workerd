// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/io/per-isolate-bootstrap.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr uint64_t kLimit = 1024 * 1024;
constexpr kj::StringPtr kData = "hello world"_kj;

// The user-visible message produced when consuming an already-consumed stream. This must match
// the message historically produced by the Body mixin exactly.
constexpr kj::StringPtr kBodyUsedError =
    "Body has already been used. It can only be used once. Use tee() first if you need to "
    "read it twice."_kj;

// A ReadableStreamSource serving fixed content with a known length.
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

// A WritableStreamSink accumulating written bytes into externally-owned state.
class CollectingSink final: public WritableStreamSink {
 public:
  CollectingSink(kj::Vector<kj::byte>& data, bool& ended): data(data), ended(ended) {}

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

  void abort(kj::Exception reason) override {}

 private:
  kj::Vector<kj::byte>& data;
  bool& ended;
};

KJ_TEST("JsReadableStream null state") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsReadableStream stream;
    KJ_EXPECT(stream.isNull());
    KJ_EXPECT(!stream.isBufferBacked());
    KJ_EXPECT(!stream.isDisturbed(js));
    KJ_EXPECT(!stream.isLocked(js));
    KJ_EXPECT(stream.tryGetLength(js) == kj::none);
    KJ_EXPECT(stream.tryClone(js) == kj::none);
    KJ_EXPECT(stream.addRef(js).isNull());
  });
}

KJ_TEST("JsReadableStream null stream consumption yields empty results") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream;
    auto promise = stream.text(js, kLimit)
                       .then(js,
                           [](jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == ""_kj);
      return JsReadableStream().arrayBuffer(js, kLimit);
    })
                       .then(js,
                           [](jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> buffer) {
      KJ_EXPECT(buffer.getHandle(js).size() == 0);
      return JsReadableStream().bytes(js, kLimit);
    })
                       .then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsUint8Array> bytes) {
      KJ_EXPECT(bytes.getHandle(js).size() == 0);
      return JsReadableStream().blob(js, kLimit, kj::str("text/plain"));
    }).then(js, [](jsg::Lock& js, jsg::Ref<Blob> blob) {
      KJ_EXPECT(blob->getSize() == 0);
      KJ_EXPECT(blob->getType() == "text/plain"_kj);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream null stream json() rejects like parsing an empty string") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream;
    auto promise = stream.json(js, kLimit).then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsValue>) {
      KJ_FAIL_REQUIRE("expected json() on a null stream to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("SyntaxError"), e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream buffer-backed stream") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(stream.isBufferBacked());
    KJ_EXPECT(!stream.isDisturbed(js));
    KJ_EXPECT(!stream.isLocked(js));
    KJ_EXPECT(KJ_ASSERT_NONNULL(stream.tryGetLength(js)) == kData.size());

    auto promise = stream.text(js, kLimit).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
      // Consumption leaves the underlying stream disturbed...
      KJ_EXPECT(stream.isDisturbed(js));
      // ...but the JsReadableStream remains buffer-backed and therefore rewindable.
      KJ_EXPECT(stream.isBufferBacked());
    }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream consuming twice rejects with the historical Body error") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto promise = stream.text(js, kLimit)
                       .then(js,
                           JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream),
                               (jsg::Lock& js, kj::String) { return stream.text(js, kLimit); }))
                       .then(js, [](jsg::Lock& js, kj::String) {
      KJ_FAIL_REQUIRE("expected second consumption to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(
          e.getDescription() == kj::str("jsg.TypeError: ", kBodyUsedError), e.getDescription());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream consumption helpers return stream content") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream forArrayBuffer(js, kj::str(kData));
    JsReadableStream forBytes(js, kj::str(kData));
    auto promise = forArrayBuffer.arrayBuffer(js, kLimit)
                       .then(js,
                           JSG_VISITABLE_LAMBDA((forBytes = kj::mv(forBytes)), (forBytes),
                               (jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> buffer) {
                                 auto handle = buffer.getHandle(js);
                                 KJ_EXPECT(handle.asArrayPtr() == kData.asBytes());
                                 return forBytes.bytes(js, kLimit);
                               }))
                       .then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsUint8Array> bytes) {
      KJ_EXPECT(bytes.getHandle(js).size() == kData.size());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream blob carries the given content type") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto promise = stream.blob(js, kLimit, kj::str("text/plain;charset=UTF-8"))
                       .then(js, [](jsg::Lock& js, jsg::Ref<Blob> blob) {
      KJ_EXPECT(blob->getSize() == int(kData.size()));
      KJ_EXPECT(blob->getType() == "text/plain;charset=UTF-8"_kj);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream json parses stream content") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str("{\"hello\":\"world\"}"));
    auto promise =
        stream.json(js, kLimit).then(js, [](jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) {
      KJ_EXPECT(value.getHandle(js).isObject());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream addRef shares the underlying stream") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto ref = stream.addRef(js);
    KJ_EXPECT(ref.isBufferBacked());
    KJ_EXPECT(!ref.isDisturbed(js));

    // Consuming through the addRef disturbs the original: both wrap the same stream.
    auto promise = ref.text(js, kLimit).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
      KJ_EXPECT(stream.isDisturbed(js));
    }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream tryClone rewinds a consumed buffer-backed stream") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto promise =
        stream.text(js, kLimit)
            .then(js,
                JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream),
                    (jsg::Lock& js, kj::String) {
                      KJ_EXPECT(stream.isDisturbed(js));
                      // The clone is a fresh, independent, rewindable stream over the same bytes, even though
                      // the original has already been consumed. This is the property fetch() redirect handling
                      // relies upon.
                      auto clone = KJ_ASSERT_NONNULL(stream.tryClone(js));
                      KJ_EXPECT(clone.isBufferBacked());
                      KJ_EXPECT(!clone.isDisturbed(js));
                      return clone.text(js, kLimit);
                    }))
            .then(js, [](jsg::Lock& js, kj::String text) { KJ_EXPECT(text == kData); });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream tee carries the buffer and nullifies the original") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto tee = stream.tee(js);
    KJ_EXPECT(stream.isNull());
    KJ_EXPECT(tee.branch1.isBufferBacked());
    KJ_EXPECT(tee.branch2.isBufferBacked());

    auto promise = tee.branch1.text(js, kLimit)
                       .then(js,
                           JSG_VISITABLE_LAMBDA((branch2 = kj::mv(tee.branch2)), (branch2),
                               (jsg::Lock& js, kj::String text) {
                                 KJ_EXPECT(text == kData);
                                 return branch2.text(js, kLimit);
                               }))
                       .then(js, [](jsg::Lock& js, kj::String text) { KJ_EXPECT(text == kData); });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream detach takes over the stream, leaving a disturbed husk") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto detached = stream.detach(js);
    // The original still holds the (now locked and disturbed) husk...
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(stream.isDisturbed(js));
    KJ_EXPECT(stream.isLocked(js));
    // ...while the detached stream took over the state, including the retransmit buffer.
    KJ_EXPECT(detached.isBufferBacked());
    KJ_EXPECT(!detached.isDisturbed(js));

    auto promise = detached.text(js, kLimit).then(js, [](jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream detach of a consumed stream throws without IgnoreDisturbed") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto promise = stream.text(js, kLimit).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js, kj::String) {
      JSG_TRY(js) {
      stream.detach(js);  // defaults to IgnoreDisturbed::NO
      KJ_FAIL_REQUIRE("expected detach() of a consumed stream to throw");
      }
      JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("TypeError"), e.getDescription());
      };
    }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream detach accepts IgnoreDisturbed::YES") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    auto detached = stream.detach(js, IgnoreDisturbed::YES);
    KJ_EXPECT(!detached.isNull());
    KJ_EXPECT(stream.isDisturbed(js));
  });
}

KJ_TEST("JsReadableStream cancel disturbs the stream; null cancel is a no-op") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // Canceling a null stream is a no-op that yields a resolved promise.
    auto nullPromise = JsReadableStream().cancel(js, kj::none);

    JsReadableStream stream(js, kj::str(kData));
    auto promise =
        stream.cancel(js, js.error("lost interest")).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js) {
          // Cancellation counts as disturbing the stream.
          KJ_EXPECT(stream.isDisturbed(js));
        }));
    return env.context.awaitJs(js,
        nullPromise.then(
            js, [promise = kj::mv(promise)](jsg::Lock& js) mutable { return kj::mv(promise); }));
  });
}

KJ_TEST("JsReadableStream forceCancel cancels even when the stream is locked") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // detach() leaves the original as a locked (and disturbed) husk, which is a convenient way
    // to produce a locked stream without a reader.
    JsReadableStream stream(js, kj::str(kData));
    auto detached = stream.detach(js);
    KJ_EXPECT(stream.isLocked(js));

    // cancel() refuses to cancel a locked stream, exactly like ReadableStream.prototype.cancel.
    auto promise = stream.cancel(js, kj::none)
                       .then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected cancel() of a locked stream to reject");
    }, [](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("currently locked to a reader"), e.getDescription());
    }).then(js, JSG_VISITABLE_LAMBDA((stream = kj::mv(stream)), (stream), (jsg::Lock& js) {
                         // forceCancel() tears the stream down regardless.
                         return stream.forceCancel(js, kj::none);
                       }));
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream setPendingClosure is safe on live and null streams") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& js = env.js;

    JsReadableStream().setPendingClosure(js);  // null: no-op

    JsReadableStream stream(js, kj::str(kData));
    stream.setPendingClosure(js);
    KJ_EXPECT(!stream.isDisturbed(js));
  });
}

KJ_TEST("JsReadableStream onEof plumbs through to the underlying stream") {
  // Note: EOF signaling fires when the stream is consumed through reader-based reads (see the
  // signalEof() calls in ReadableStreamInternalController). The JsReadableStream consumption
  // helpers remove the underlying source and bypass that path, so end-to-end EOF behavior is
  // exercised by the socket tests instead. Here we just verify the plumbing: onEof() can be
  // registered and the stream remains fully usable afterwards.
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    auto eofPromise = stream.onEof(js);
    eofPromise.markAsHandled(js);
    auto promise = stream.text(js, kLimit).then(js, [](jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream create wraps a native source") {
  TestFixture testFixture;
  testFixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(!stream.isBufferBacked());
    KJ_EXPECT(KJ_ASSERT_NONNULL(stream.tryGetLength(js)) == kData.size());

    auto promise = stream.text(js, kLimit).then(js, [](jsg::Lock& js, kj::String text) {
      KJ_EXPECT(text == kData);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("JsReadableStream pumpTo drains into a sink and ends it") {
  TestFixture testFixture;
  kj::Vector<kj::byte> collected;
  bool ended = false;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(ended);
}

KJ_TEST("JsReadableStream pumpTo with EndStream::NO leaves the sink open") {
  TestFixture testFixture;
  kj::Vector<kj::byte> collected;
  bool ended = false;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    JsReadableStream stream(js, kj::str(kData));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::NO)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(!ended);
}

// =======================================================================================
// ReadableStreamNativeSource

// A ContentSource that additionally records cancellation into externally-owned state.
class CancelableContentSource final: public ReadableStreamSource {
 public:
  CancelableContentSource(kj::StringPtr data, kj::Maybe<kj::Exception>& canceled)
      : data(data),
        canceled(canceled) {}

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

  void cancel(kj::Exception reason) override {
    canceled = kj::mv(reason);
  }

 private:
  kj::StringPtr data;
  size_t offset = 0;
  kj::Maybe<kj::Exception>& canceled;
};

// A ReadableStreamSource whose reads always fail. Uses a tunneled JS exception so the
// failure surfaces to JavaScript with its message intact (a bare KJ exception would
// surface as an opaque "internal error").
class ErroringSource final: public ReadableStreamSource {
 public:
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return JSG_KJ_EXCEPTION(FAILED, Error, "test read failure");
  }
};

// A ContentSource with an optimized tee: each branch is an independent ContentSource over
// the remaining content (exercising ReadableStreamNativeSource::tee()'s tryTee fast
// path).
class TeeableContentSource final: public ReadableStreamSource {
 public:
  TeeableContentSource(kj::StringPtr data): data(data) {}

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

  kj::Maybe<Tee> tryTee(uint64_t limit) override {
    auto remaining = data.slice(offset);
    return Tee{
      .branches = {kj::heap<ContentSource>(remaining), kj::heap<ContentSource>(remaining)},
    };
  }

 private:
  kj::StringPtr data;
  size_t offset = 0;
};

// Records the conduit-side observations of a native source's deliveries.
struct MockControllerState {
  kj::Vector<kj::Array<kj::byte>> enqueued;
  bool closed = false;
  kj::Maybe<double> responded;
};

// Builds a mock of the TypeScript conduit's controller facade recording into `state`.
// `byobRequest` is null for default-read pulls, or a mock BYOB request object (see
// makeMockByobRequest).
jsg::JsObject makeMockController(
    jsg::Lock& js, MockControllerState& state, jsg::JsValue byobRequest) {
  auto obj = js.obj();
  obj.set(js, "byobRequest"_kj, byobRequest);
  obj.set(js, "enqueue"_kj,
      jsg::JsValue(js.wrapSimpleFunction(
          js.v8Context(), [&state](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto chunk = KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsUint8Array>());
    state.enqueued.add(kj::heapArray<kj::byte>(chunk.asArrayPtr()));
  })));
  obj.set(js, "close"_kj,
      jsg::JsValue(js.wrapSimpleFunction(
          js.v8Context(), [&state](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
    state.closed = true;
  })));
  return obj;
}

// Builds a mock of the conduit's BYOB request facade over the given view, recording
// respond() calls into `state`.
jsg::JsObject makeMockByobRequest(
    jsg::Lock& js, MockControllerState& state, jsg::JsUint8Array view, double atLeast) {
  auto obj = js.obj();
  obj.set(js, "view"_kj, view);
  obj.set(js, "atLeast"_kj, js.num(atLeast));
  obj.set(js, "respond"_kj,
      jsg::JsValue(js.wrapSimpleFunction(
          js.v8Context(), [&state](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
    state.responded = info[0].As<v8::Number>()->Value();
  })));
  return obj;
}

jsg::Ref<AbortSignal> freshSignal(jsg::Lock& js) {
  return AbortController::constructor(js)->getSignal();
}

// Reads a source's expectedLength (a JS bigint | undefined, per the conduit contract)
// back into C++ terms. Asserts the "unknown" representation is undefined specifically:
// null would pass a naive falsy check but is rejected by the conduit's validation.
kj::Maybe<uint64_t> expectedLengthOf(jsg::Lock& js, ReadableStreamNativeSource& source) {
  KJ_IF_SOME(bigint, source.getExpectedLength(js)) {
    return bigint.tryToUint64(js);
  }
  return kj::none;
}

KJ_TEST("ReadableStreamNativeSource default pull delivers chunks then closes at EOF") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto controller = makeMockController(js, state, js.null());

    auto promise = source->pull(js, controller, freshSignal(js))
                       .then(js,
                           [&state, source = source.addRef(), controller = controller.addRef(js)](
                               jsg::Lock& js) mutable {
      KJ_EXPECT(state.enqueued.size() == 1);
      KJ_EXPECT(state.enqueued[0].asPtr() == kData.asBytes());
      KJ_EXPECT(!state.closed);
      // The next pull observes EOF and closes (never enqueues an empty chunk).
      return source->pull(js, controller.getHandle(js), freshSignal(js));
    }).then(js, [&state](jsg::Lock& js) {
      KJ_EXPECT(state.enqueued.size() == 1);
      KJ_EXPECT(state.closed);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource BYOB pull fills the view; zero-byte EOF closes") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto view = jsg::JsUint8Array::create(js, static_cast<size_t>(64));
    auto byobRequest = makeMockByobRequest(js, state, view, 4);
    auto controller = makeMockController(js, state, byobRequest);

    auto promise = source->pull(js, controller, freshSignal(js))
                       .then(js,
                           [&state, source = source.addRef(), controller = controller.addRef(js),
                               view = view.addRef(js)](jsg::Lock& js) mutable {
      KJ_EXPECT(KJ_ASSERT_NONNULL(state.responded) == kData.size());
      auto filled = view.getHandle(js).asArrayPtr().first(kData.size());
      KJ_EXPECT(filled == kData.asBytes());
      KJ_EXPECT(!state.closed);
      state.responded = kj::none;
      // EOF with zero bytes must be signaled via close(), never respond(0).
      return source->pull(js, controller.getHandle(js), freshSignal(js));
    }).then(js, [&state](jsg::Lock& js) {
      KJ_EXPECT(state.responded == kj::none);
      KJ_EXPECT(state.closed);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource BYOB under-delivery responds then closes (fused EOF)") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    // The source can only ever produce 11 bytes, so a minimum of 20 cannot be satisfied:
    // the partial bytes commit via respond() and EOF is signaled explicitly via close()
    // in the same pull turn (the fused close-commit), and the source is done.
    auto view = jsg::JsUint8Array::create(js, static_cast<size_t>(64));
    auto byobRequest = makeMockByobRequest(js, state, view, 20);
    auto controller = makeMockController(js, state, byobRequest);

    auto promise = source->pull(js, controller, freshSignal(js))
                       .then(js,
                           [&state, source = source.addRef(), controller = controller.addRef(js)](
                               jsg::Lock& js) mutable {
      KJ_EXPECT(KJ_ASSERT_NONNULL(state.responded) == kData.size());
      KJ_EXPECT(state.closed);
      state.responded = kj::none;
      // The source released itself at the under-delivery; a further (contract-violating)
      // pull is tolerated as an inert no-op.
      return source->pull(js, controller.getHandle(js), freshSignal(js));
    }).then(js, [&state](jsg::Lock& js) {
      KJ_EXPECT(state.responded == kj::none);
      KJ_EXPECT(state.enqueued.size() == 0);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource abandoned default pull stashes and redelivers") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto controller = makeMockController(js, state, js.null());

    // Start a pull, then abort its signal before the read's completion is processed --
    // the consumer abandoned the read (e.g. releaseLock()). The bytes must not be
    // delivered OR dropped: they are retained for the next pull.
    auto abortController = AbortController::constructor(js);
    auto pullPromise = source->pull(js, controller, abortController->getSignal());
    abortController->abort(js, kj::none);

    auto promise = pullPromise
                       .then(js,
                           [&state, source = source.addRef(), controller = controller.addRef(js)](
                               jsg::Lock& js) mutable {
      KJ_EXPECT(state.enqueued.size() == 0);
      KJ_EXPECT(!state.closed);
      // The next pull redelivers the stashed bytes without touching the source.
      return source->pull(js, controller.getHandle(js), freshSignal(js));
    }).then(js, [&state](jsg::Lock& js) {
      KJ_EXPECT(state.enqueued.size() == 1);
      KJ_EXPECT(state.enqueued[0].asPtr() == kData.asBytes());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource abandoned BYOB pull stashes; redelivery is synchronous") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto view = jsg::JsUint8Array::create(js, static_cast<size_t>(64));
    auto byobRequest = makeMockByobRequest(js, state, view, 4);
    auto controller = makeMockController(js, state, byobRequest);

    auto abortController = AbortController::constructor(js);
    auto pullPromise = source->pull(js, controller, abortController->getSignal());
    abortController->abort(js, kj::none);

    auto promise = pullPromise.then(js,
        [&state, source = source.addRef(), controller = controller.addRef(js),
            view = view.addRef(js)](jsg::Lock& js) mutable {
      KJ_EXPECT(state.responded == kj::none);
      // The retained bytes satisfy the next read's minimum, so redelivery happens
      // synchronously from the stash (no I/O).
      auto promise = source->pull(js, controller.getHandle(js), freshSignal(js));
      KJ_EXPECT(KJ_ASSERT_NONNULL(state.responded) == kData.size());
      auto filled = view.getHandle(js).asArrayPtr().first(kData.size());
      KJ_EXPECT(filled == kData.asBytes());
      return kj::mv(promise);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource cancel propagates and later pulls are inert") {
  TestFixture testFixture;
  MockControllerState state;
  kj::Maybe<kj::Exception> canceled;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(
        env.context, kj::heap<CancelableContentSource>(kData, canceled));
    KJ_EXPECT(KJ_ASSERT_NONNULL(expectedLengthOf(js, *source)) == kData.size());

    source->cancel(js, js.error("lost interest"));
    auto& e = KJ_ASSERT_NONNULL(canceled);
    KJ_EXPECT(e.getDescription().contains("lost interest"), e.getDescription());

    // The source has been released: expectedLength is unknown and pulls are inert.
    KJ_EXPECT(expectedLengthOf(js, *source) == kj::none);
    auto controller = makeMockController(js, state, js.null());
    auto promise = source->pull(js, controller, freshSignal(js)).then(js, [&state](jsg::Lock& js) {
      KJ_EXPECT(state.enqueued.size() == 0);
      KJ_EXPECT(!state.closed);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource cancel during an in-flight pull defers the release") {
  TestFixture testFixture;
  MockControllerState state;
  kj::Maybe<kj::Exception> canceled;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(
        env.context, kj::heap<CancelableContentSource>(kData, canceled));
    auto controller = makeMockController(js, state, js.null());

    // Cancel while the pull's read is in flight: the source must not be destroyed out
    // from under its own read; the read's bytes are discarded at settlement.
    auto pullPromise = source->pull(js, controller, freshSignal(js));
    source->cancel(js, js.error("teardown"));
    KJ_EXPECT(canceled != kj::none);

    auto promise = pullPromise
                       .then(js,
                           [&state, source = source.addRef(), controller = controller.addRef(js)](
                               jsg::Lock& js) mutable {
      KJ_EXPECT(state.enqueued.size() == 0);
      KJ_EXPECT(!state.closed);
      // The deferred teardown completed at settlement; the source is now done.
      KJ_EXPECT(expectedLengthOf(js, *source) == kj::none);
      return source->pull(js, controller.getHandle(js), freshSignal(js));
    }).then(js, [&state](jsg::Lock& js) { KJ_EXPECT(state.enqueued.size() == 0); });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource expectedLength queries the source live") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    KJ_EXPECT(KJ_ASSERT_NONNULL(expectedLengthOf(js, *source)) == kData.size());

    auto controller = makeMockController(js, state, js.null());
    auto promise = source->pull(js, controller, freshSignal(js))
                       .then(js, [source = source.addRef()](jsg::Lock& js) mutable {
      // ContentSource reports its REMAINING length, and expectedLength is not cached, so
      // consuming the content is observable.
      KJ_EXPECT(KJ_ASSERT_NONNULL(expectedLengthOf(js, *source)) == 0);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource read failure rejects the pull") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ErroringSource>());
    auto controller = makeMockController(js, state, js.null());

    auto promise = source->pull(js, controller, freshSignal(js)).then(js, [](jsg::Lock& js) {
      KJ_FAIL_REQUIRE("expected pull() to reject");
    }, [&state](jsg::Lock& js, jsg::Value exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("test read failure"), e.getDescription());
      KJ_EXPECT(state.enqueued.size() == 0);
      KJ_EXPECT(!state.closed);
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource tee splits via the source's optimized tryTee") {
  TestFixture testFixture;
  MockControllerState state1;
  MockControllerState state2;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source =
        js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<TeeableContentSource>(kData));
    auto branches = source->tee(js);
    KJ_EXPECT(branches.size() == 2);
    // The original is consumed: its source is released and expectedLength is unknown.
    KJ_EXPECT(expectedLengthOf(js, *source) == kj::none);

    auto controller1 = makeMockController(js, state1, js.null());
    auto controller2 = makeMockController(js, state2, js.null());
    auto promise = branches[0]
                       ->pull(js, controller1, freshSignal(js))
                       .then(js,
                           [&state1, branch = branches[1].addRef(),
                               controller2 = controller2.addRef(js)](jsg::Lock& js) mutable {
      KJ_EXPECT(state1.enqueued.size() == 1);
      KJ_EXPECT(state1.enqueued[0].asPtr() == kData.asBytes());
      return branch->pull(js, controller2.getHandle(js), freshSignal(js));
    }).then(js, [&state2](jsg::Lock& js) {
      KJ_EXPECT(state2.enqueued.size() == 1);
      KJ_EXPECT(state2.enqueued[0].asPtr() == kData.asBytes());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource tee falls back to a generic kj tee") {
  TestFixture testFixture;
  MockControllerState state1;
  MockControllerState state2;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // ContentSource has no tryTee() override, so this exercises the kj::newTee fallback.
    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto branches = source->tee(js);
    KJ_EXPECT(branches.size() == 2);
    KJ_EXPECT(expectedLengthOf(js, *source) == kj::none);

    auto controller1 = makeMockController(js, state1, js.null());
    auto controller2 = makeMockController(js, state2, js.null());
    auto promise = branches[0]
                       ->pull(js, controller1, freshSignal(js))
                       .then(js,
                           [&state1, branch = branches[1].addRef(),
                               controller2 = controller2.addRef(js)](jsg::Lock& js) mutable {
      KJ_EXPECT(state1.enqueued.size() == 1);
      KJ_EXPECT(state1.enqueued[0].asPtr() == kData.asBytes());
      return branch->pull(js, controller2.getHandle(js), freshSignal(js));
    }).then(js, [&state2](jsg::Lock& js) {
      KJ_EXPECT(state2.enqueued.size() == 1);
      KJ_EXPECT(state2.enqueued[0].asPtr() == kData.asBytes());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource tee seeds both branches with stashed bytes") {
  TestFixture testFixture;
  MockControllerState state0;
  MockControllerState state1;
  MockControllerState state2;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source =
        js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<TeeableContentSource>(kData));

    // Abandon a pull so its bytes land in the stash.
    auto controller = makeMockController(js, state0, js.null());
    auto abortController = AbortController::constructor(js);
    auto pullPromise = source->pull(js, controller, abortController->getSignal());
    abortController->abort(js, kj::none);

    auto promise = pullPromise
                       .then(js, [&, source = source.addRef()](jsg::Lock& js) mutable {
      KJ_EXPECT(state0.enqueued.size() == 0);

      auto branches = source->tee(js);
      // The upstream is exhausted (the abandoned pull consumed everything); both branches
      // will produce exactly the inherited stash bytes, and their expectedLength accounts
      // for them (the conduit enforces expectedLength as an exact total, so this matters).
      KJ_EXPECT(KJ_ASSERT_NONNULL(expectedLengthOf(js, *branches[0])) == kData.size());
      KJ_EXPECT(KJ_ASSERT_NONNULL(expectedLengthOf(js, *branches[1])) == kData.size());

      auto controller1 = makeMockController(js, state1, js.null());
      auto controller2 = makeMockController(js, state2, js.null());
      auto p1 = branches[0]->pull(js, controller1, freshSignal(js));
      auto p2 = branches[1]->pull(js, controller2, freshSignal(js));
      return p1.then(js, [p2 = kj::mv(p2)](jsg::Lock& js) mutable { return kj::mv(p2); });
    }).then(js, [&](jsg::Lock& js) {
      KJ_EXPECT(state1.enqueued.size() == 1);
      KJ_EXPECT(state1.enqueued[0].asPtr() == kData.asBytes());
      KJ_EXPECT(state2.enqueued.size() == 1);
      KJ_EXPECT(state2.enqueued[0].asPtr() == kData.asBytes());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

KJ_TEST("ReadableStreamNativeSource tee guards: consumed source and in-flight pull") {
  TestFixture testFixture;
  MockControllerState state;
  kj::Maybe<kj::Exception> canceled;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // A consumed (canceled) source refuses to tee.
    auto consumed = js.alloc<ReadableStreamNativeSource>(
        env.context, kj::heap<CancelableContentSource>(kData, canceled));
    consumed->cancel(js, kj::none);
    JSG_TRY(js) {
      auto branches KJ_UNUSED = consumed->tee(js);
      KJ_FAIL_REQUIRE("expected tee() of a consumed source to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("already been consumed"), e.getDescription());
    };

    // A source with a read in flight refuses to tee (bytes the read produces after the
    // split would be lost).
    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto controller = makeMockController(js, state, js.null());
    auto pullPromise = source->pull(js, controller, freshSignal(js));
    JSG_TRY(js) {
      auto branches KJ_UNUSED = source->tee(js);
      KJ_FAIL_REQUIRE("expected tee() with a read in flight to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("while a read is in flight"), e.getDescription());
    };
    return env.context.awaitJs(js, kj::mv(pullPromise));
  });
}

KJ_TEST("ReadableStreamNativeSource instances carry the kNativeSource marker") {
  TestFixture testFixture;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    // Wrap an instance via the Lock-acquired TypeHandler -- the same path create()'s
    // TypeScript arm will use -- then verify the contract's detection shape: an own data
    // property keyed by the kNativeSource API-registry symbol whose value is the symbol
    // itself, re-acquirable by name at any time.
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto handle = handler.wrap(
        js, js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData)));
    auto obj = KJ_ASSERT_NONNULL(jsg::JsValue(handle).tryCast<jsg::JsObject>());

    auto symbol = jsg::JsValue(
        v8::Symbol::ForApi(js.v8Isolate, jsg::v8StrIntern(js.v8Isolate, "kNativeSource")));
    KJ_EXPECT(obj.has(js, symbol, jsg::JsObject::HasOption::OWN));
    KJ_EXPECT(obj.get(js, symbol) == symbol);
  });
}

// =======================================================================================
// pumpTo of TypeScript-backed streams

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

// Constructs a TypeScript ReadableStream over the given underlying source object (via
// the constructor exposed through the bootstrap's cpp_exports module) and adopts it as a
// TypeScript-backed JsReadableStream.
JsReadableStream makeTsStream(jsg::Lock& js, jsg::JsValue underlyingSource) {
  auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
  auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
  auto constructor =
      KJ_ASSERT_NONNULL(exportsObj.get(js, "ReadableStream"_kj).tryCast<jsg::JsFunction>());
  return JsReadableStream(js, constructor.newInstance(js, underlyingSource).addRef(js));
}

// Builds a plain-JS underlying source (the queued backend) that enqueues kData once as
// bytes and closes.
jsg::JsValue makeQueuedByteSource(jsg::Lock& js) {
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
  return jsg::JsValue(underlying);
}

KJ_TEST("JsReadableStream pumpTo drains a TypeScript-backed native stream") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // Under the flag, create() produces a TypeScript-backed stream over the native
    // source; pumpTo() must extract it and pump at the C++ layer.
    auto stream = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(ended);
}

KJ_TEST("JsReadableStream pumpTo of a TypeScript-backed stream honors EndStream::NO") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = JsReadableStream::create(js, env.context, kj::heap<ContentSource>(kData));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::NO)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(!ended);
}

KJ_TEST("JsReadableStream pumpTo emits stashed bytes before the source") {
  auto fixture = makeTsStreamsFixture();
  MockControllerState state;
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // Stash the whole content by abandoning a pull, then build a TypeScript stream over
    // the source. Extraction hands the stash to the pump as a prefix.
    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto controller = makeMockController(js, state, js.null());
    auto abortController = AbortController::constructor(js);
    auto pullPromise = source->pull(js, controller, abortController->getSignal());
    abortController->abort(js, kj::none);

    auto promise = pullPromise.then(js, [&, source = source.addRef()](jsg::Lock& js) mutable {
      auto& handler =
          KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
      auto stream = makeTsStream(js, jsg::JsValue(handler.wrap(js, kj::mv(source))));
      auto pump = stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
                      .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
      return IoContext::current().awaitIo(js, kj::mv(pump));
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(ended);
}

KJ_TEST("JsReadableStream pumpTo of an already-completed native stream just finishes") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // The source completes (via cancel) before the stream is even constructed;
    // extraction of a closed stream is legal and the pump simply ends.
    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    source->cancel(js, kj::none);
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto stream = makeTsStream(js, jsg::JsValue(handler.wrap(js, kj::mv(source))));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.size() == 0);
  KJ_EXPECT(ended);
}

KJ_TEST("JsReadableStream pumpTo drains a TypeScript-backed queued stream") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // A plain JS underlying source (no native marker): the queued backend, pumped via
    // the internal DrainingReader.
    auto stream = makeTsStream(js, makeQueuedByteSource(js));
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
        .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
  });
  KJ_EXPECT(collected.asPtr() == kData.asBytes());
  KJ_EXPECT(ended);
}

KJ_TEST("JsReadableStream pumpTo rejects when a queued stream produces non-byte chunks") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto underlying = js.obj();
    underlying.set(js, "start"_kj,
        jsg::JsValue(js.wrapSimpleFunction(
            js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info) {
      auto controller = KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsObject>());
      auto enqueue = KJ_ASSERT_NONNULL(controller.get(js, "enqueue"_kj).tryCast<jsg::JsFunction>());
      enqueue.call(js, controller, js.str("not bytes"_kj));
    })));

    auto stream = makeTsStream(js, underlying);
    return stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES)
        .then([](DeferredProxy<void> proxy) {
      return kj::mv(proxy.proxyTask);
    }).then([]() {
      KJ_FAIL_REQUIRE("expected the pump to reject");
    }, [](kj::Exception&& exception) {
      KJ_EXPECT(
          exception.getDescription().contains("did not return bytes"), exception.getDescription());
    });
  });
  KJ_EXPECT(!ended);
}

KJ_TEST("JsReadableStream pumpTo of a locked TypeScript-backed stream throws") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected;
  bool ended = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
    auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
    auto constructor =
        KJ_ASSERT_NONNULL(exportsObj.get(js, "ReadableStream"_kj).tryCast<jsg::JsFunction>());
    auto streamObj = constructor.newInstance(js, jsg::JsValue(js.obj()));
    auto stream = JsReadableStream(js, streamObj.addRef(js));

    // Lock the stream by acquiring the internal draining reader, then verify the
    // legacy-parity precondition.
    auto acquire = KJ_ASSERT_NONNULL(
        exportsObj.get(js, "acquireReadableStreamDrainingReader"_kj).tryCast<jsg::JsFunction>());
    auto reader KJ_UNUSED = acquire.call(js, js.undefined(), jsg::JsValue(streamObj));
    KJ_EXPECT(stream.isLocked(js));

    JSG_TRY(js) {
      auto pump KJ_UNUSED =
          stream.pumpTo(js, kj::heap<CollectingSink>(collected, ended), EndStream::YES);
      KJ_FAIL_REQUIRE("expected pumpTo() of a locked stream to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("locked to a reader"), e.getDescription());
    };
  });
}

KJ_TEST("ReadableStreamNativeSource rejects concurrent pulls") {
  TestFixture testFixture;
  MockControllerState state;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto source = js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData));
    auto controller = makeMockController(js, state, js.null());

    auto pullPromise = source->pull(js, controller, freshSignal(js));

    // The conduit structurally guarantees pull serialization; the guard is defensive and
    // must fail loudly.
    JSG_TRY(js) {
      auto ignored KJ_UNUSED = source->pull(js, controller, freshSignal(js));
      KJ_FAIL_REQUIRE("expected concurrent pull() to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("already in flight"), e.getDescription());
    };

    auto promise = pullPromise.then(js, [&state](jsg::Lock& js) {
      // The original pull is unaffected by the rejected concurrent attempt.
      KJ_EXPECT(state.enqueued.size() == 1);
      KJ_EXPECT(state.enqueued[0].asPtr() == kData.asBytes());
    });
    return env.context.awaitJs(js, kj::mv(promise));
  });
}

// =======================================================================================
// jsgTryUnwrap of TypeScript-backed streams

KJ_TEST("JsReadableStream::tryUnwrapTs adopts TypeScript streams and rejects impostors") {
  auto fixture = makeTsStreamsFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
    auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
    auto constructor =
        KJ_ASSERT_NONNULL(exportsObj.get(js, "ReadableStream"_kj).tryCast<jsg::JsFunction>());
    auto streamObj = constructor.newInstance(js, jsg::JsValue(js.obj()));

    // A genuine TypeScript stream unwraps.
    auto unwrapped = KJ_ASSERT_NONNULL(JsReadableStream::tryUnwrapTs(js, jsg::JsValue(streamObj)));
    KJ_EXPECT(!unwrapped.isNull());
    KJ_EXPECT(!unwrapped.isLocked(js));

    // Unwrap ADOPTS the same underlying stream (it does not copy): locking through the
    // original JS handle is visible through the adopted JsReadableStream.
    auto acquire = KJ_ASSERT_NONNULL(
        exportsObj.get(js, "acquireReadableStreamDrainingReader"_kj).tryCast<jsg::JsFunction>());
    auto reader KJ_UNUSED = acquire.call(js, js.undefined(), jsg::JsValue(streamObj));
    KJ_EXPECT(unwrapped.isLocked(js));

    // Unwrap has no locked/disturbed preconditions (legacy parity): a locked stream
    // still unwraps; consumers enforce their own requirements.
    auto unwrappedWhileLocked =
        KJ_ASSERT_NONNULL(JsReadableStream::tryUnwrapTs(js, jsg::JsValue(streamObj)));
    KJ_EXPECT(unwrappedWhileLocked.isLocked(js));

    // Impostors do not unwrap: plain objects, primitives, and JSG resource objects that
    // are not streams all fail the brand check.
    KJ_EXPECT(JsReadableStream::tryUnwrapTs(js, jsg::JsValue(js.obj())) == kj::none);
    KJ_EXPECT(JsReadableStream::tryUnwrapTs(js, js.str("stream"_kj)) == kj::none);
    KJ_EXPECT(JsReadableStream::tryUnwrapTs(js, js.num(42.0)) == kj::none);
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto sourceObj = jsg::JsValue(handler.wrap(
        js, js.alloc<ReadableStreamNativeSource>(env.context, kj::heap<ContentSource>(kData))));
    KJ_EXPECT(JsReadableStream::tryUnwrapTs(js, sourceObj) == kj::none);
  });
}

KJ_TEST("JsReadableStream::tryUnwrapTs is inert without the compat flag") {
  TestFixture testFixture;
  testFixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;
    // Without the flag there is no TypeScript implementation and no bootstrap export;
    // the guard must return none rather than reaching for the absent export.
    KJ_EXPECT(JsReadableStream::tryUnwrapTs(js, jsg::JsValue(js.obj())) == kj::none);
  });
}

// =======================================================================================
// tee of TypeScript-backed streams

KJ_TEST("JsReadableStream::tee splits a TypeScript-backed queued stream") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected1;
  kj::Vector<kj::byte> collected2;
  bool ended1 = false;
  bool ended2 = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto stream = makeTsStream(js, makeQueuedByteSource(js));
    auto tee = stream.tee(js);
    // tee() consumes the wrapper, exactly like the legacy arm (Body::clone relies on it).
    KJ_EXPECT(stream.isNull());
    KJ_EXPECT(!tee.branch1.isNull());
    KJ_EXPECT(!tee.branch2.isNull());

    // Both branches produce the full content independently (pumped via the queued
    // drain-loop pump; both pumps run concurrently, sequenced only for awaiting).
    auto p1 = tee.branch1.pumpTo(js, kj::heap<CollectingSink>(collected1, ended1), EndStream::YES)
                  .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
    auto p2 = tee.branch2.pumpTo(js, kj::heap<CollectingSink>(collected2, ended2), EndStream::YES)
                  .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
    return p1.then([p2 = kj::mv(p2)]() mutable { return kj::mv(p2); });
  });
  KJ_EXPECT(collected1.asPtr() == kData.asBytes());
  KJ_EXPECT(collected2.asPtr() == kData.asBytes());
  KJ_EXPECT(ended1);
  KJ_EXPECT(ended2);
}

KJ_TEST("JsReadableStream::tee splits a TypeScript-backed native stream") {
  auto fixture = makeTsStreamsFixture();
  kj::Vector<kj::byte> collected1;
  kj::Vector<kj::byte> collected2;
  bool ended1 = false;
  bool ended2 = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    // create() under the flag builds a TypeScript stream over the native source; the
    // C++ tee arm then routes through the TS tee, which drives the native tee hook
    // (ReadableStreamNativeSource::tee -> tryTee fast path here).
    auto stream = JsReadableStream::create(js, env.context, kj::heap<TeeableContentSource>(kData));
    auto tee = stream.tee(js);
    KJ_EXPECT(stream.isNull());

    auto p1 = tee.branch1.pumpTo(js, kj::heap<CollectingSink>(collected1, ended1), EndStream::YES)
                  .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
    auto p2 = tee.branch2.pumpTo(js, kj::heap<CollectingSink>(collected2, ended2), EndStream::YES)
                  .then([](DeferredProxy<void> proxy) { return kj::mv(proxy.proxyTask); });
    return p1.then([p2 = kj::mv(p2)]() mutable { return kj::mv(p2); });
  });
  KJ_EXPECT(collected1.asPtr() == kData.asBytes());
  KJ_EXPECT(collected2.asPtr() == kData.asBytes());
  KJ_EXPECT(ended1);
  KJ_EXPECT(ended2);
}

KJ_TEST("JsReadableStream::tee of a locked TypeScript-backed stream throws") {
  auto fixture = makeTsStreamsFixture();
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& js = env.js;

    auto cppExports = KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
    auto exportsObj = KJ_ASSERT_NONNULL(cppExports.tryCast<jsg::JsObject>());
    auto constructor =
        KJ_ASSERT_NONNULL(exportsObj.get(js, "ReadableStream"_kj).tryCast<jsg::JsFunction>());
    auto streamObj = constructor.newInstance(js, jsg::JsValue(js.obj()));
    auto stream = JsReadableStream(js, streamObj.addRef(js));

    auto acquire = KJ_ASSERT_NONNULL(
        exportsObj.get(js, "acquireReadableStreamDrainingReader"_kj).tryCast<jsg::JsFunction>());
    auto reader KJ_UNUSED = acquire.call(js, js.undefined(), jsg::JsValue(streamObj));
    KJ_EXPECT(stream.isLocked(js));

    JSG_TRY(js) {
      auto tee KJ_UNUSED = stream.tee(js);
      KJ_FAIL_REQUIRE("expected tee() of a locked stream to throw");
    }
    JSG_CATCH(exception) {
      auto e = js.exceptionToKj(kj::mv(exception));
      KJ_EXPECT(e.getDescription().contains("locked"), e.getDescription());
    };
    // The precondition throw happens before consumption: the wrapper is intact.
    KJ_EXPECT(!stream.isNull());
    KJ_EXPECT(stream.isLocked(js));
  });
}

}  // namespace
}  // namespace workerd::api
