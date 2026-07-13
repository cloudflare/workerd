// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/tests/test-fixture.h>

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
    memcpy(buffer, data.begin() + offset, amount);
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
      js.tryCatch([&] {
        stream.detach(js);  // defaults to IgnoreDisturbed::NO
        KJ_FAIL_REQUIRE("expected detach() of a consumed stream to throw");
      }, [&](jsg::Value exception) {
        auto e = js.exceptionToKj(kj::mv(exception));
        KJ_EXPECT(e.getDescription().contains("TypeError"), e.getDescription());
      });
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

}  // namespace
}  // namespace workerd::api
