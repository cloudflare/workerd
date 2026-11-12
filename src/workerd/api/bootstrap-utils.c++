// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "bootstrap-utils.h"

#include "http.h"
#include "js-readable-stream.h"
#include "js-writable-stream.h"
#include "streams/identity-transform-stream.h"
#include "streams/readable.h"
#include "util.h"

#include <workerd/io/io-context.h>
#include <workerd/jsg/url.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

namespace {

// A ReadableStream of either implementation, if `value` is one.
kj::Maybe<JsReadableStream> tryUnwrapReadableStream(jsg::Lock& js, jsg::JsValue value) {
  // ReadableStream is registered in EW_STREAMS_ISOLATE_TYPES whichever implementation is in use.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStream>>(),
      "ReadableStream is missing from the isolate type list");
  KJ_IF_SOME(legacy, handler.tryUnwrap(js, value)) {
    return JsReadableStream(kj::mv(legacy));
  }
  return JsReadableStream::tryUnwrapTs(js, value);
}

// A WritableStreamSink over a bootstrap object's write(), end() and abort(); see
// BootstrapUtils::pumpTo().
class CallbackSink final: public WritableStreamSink {
 public:
  CallbackSink(IoContext& context, jsg::JsRef<jsg::JsObject> sink)
      : context(context),
        sink(kj::mv(sink)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    if (buffer == nullptr) return kj::READY_NOW;
    return context.run([weakThis = addWeakToThis(), buffer](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "sink destroyed before write ran");
      // A copy: the bytes are only valid until the write completes.
      return self.call(lock, "write"_kj, jsg::JsValue(jsg::JsArrayBuffer::create(js, buffer)));
    });
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    size_t amount = 0;
    for (auto& piece: pieces) {
      amount += piece.size();
    }
    if (amount == 0) return kj::READY_NOW;
    return context.run([weakThis = addWeakToThis(), amount, pieces](Worker::Lock& lock) mutable {
      jsg::Lock& js = lock;
      auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "sink destroyed before write ran");
      auto buffer = jsg::JsArrayBuffer::create(js, amount);
      auto out = buffer.asArrayPtr();
      for (auto& piece: pieces) {
        out.first(piece.size()).copyFrom(piece);
        out = out.slice(piece.size());
      }
      return self.call(lock, "write"_kj, jsg::JsValue(buffer));
    });
  }

  kj::Promise<void> end() override {
    return context.run([weakThis = addWeakToThis()](Worker::Lock& lock) mutable {
      auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "sink destroyed before end ran");
      return self.call(lock, "end"_kj, kj::none);
    });
  }

  void abort(kj::Exception reason) override {
    // Called with or without the isolate lock, so the call into the isolate is a task, which
    // takes the sink: nothing writes to it after an abort.
    KJ_IF_SOME(s, sink) {
      auto owned = kj::mv(s);
      sink = kj::none;
      if (!context.hasCurrentIncomingRequest()) return;
      context.addTask(
          context.run([sink = kj::mv(owned), reason = kj::mv(reason)](Worker::Lock& lock) mutable {
        jsg::Lock& js = lock;
        auto obj = sink.getHandle(js);
        auto abort = KJ_ASSERT_NONNULL(obj.get(js, "abort"_kj).tryCast<jsg::JsFunction>());
        abort.call(js, obj, jsg::JsValue(js.exceptionToJsValue(kj::mv(reason)).getHandle(js)));
      }));
    }
  }

 private:
  IoContext& context;
  // None once aborted.
  kj::Maybe<jsg::JsRef<jsg::JsObject>> sink;

  kj::Promise<void> call(Worker::Lock& lock, kj::StringPtr name, kj::Maybe<jsg::JsValue> argument) {
    jsg::Lock& js = lock;
    auto& ref = KJ_REQUIRE_NONNULL(sink, "the sink has been aborted");
    auto obj = ref.getHandle(js);
    auto method = KJ_ASSERT_NONNULL(obj.get(js, name).tryCast<jsg::JsFunction>());
    auto result = method.call(js, obj, argument.orDefault(js.undefined()));
    if (!result.isPromise()) return kj::READY_NOW;
    return context.awaitJs(lock, js.toVoidPromise(v8::Local<v8::Value>(result)));
  }
};

}  // namespace

bool BootstrapUtils::isFetcher(jsg::Lock& js, jsg::JsValue value) {
  // Fetcher is registered in EW_HTTP_ISOLATE_TYPES.
  auto& handler = KJ_ASSERT_NONNULL(
      js.tryGetTypeHandler<jsg::Ref<Fetcher>>(), "Fetcher is missing from the isolate type list");
  return handler.tryUnwrap(js, value) != kj::none;
}

jsg::JsValue BootstrapUtils::parseUrl(jsg::Lock& js, jsg::JsValue input) {
  auto str =
      JSG_REQUIRE_NONNULL(input.tryCast<jsg::JsString>(), TypeError, "parseUrl() expects a string")
          .toString(js);
  KJ_IF_SOME(url, jsg::Url::tryParse(str.asPtr())) {
    auto result = js.objNoProto();
    result.set(js, "href"_kj, js.str(url.getHref()));
    result.set(js, "origin"_kj, js.str(url.getOrigin()));
    return result;
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::mimeTypeEssence(jsg::Lock& js, jsg::JsValue input) {
  auto str = JSG_REQUIRE_NONNULL(
      input.tryCast<jsg::JsString>(), TypeError, "mimeTypeEssence() expects a string")
                 .toString(js);
  KJ_IF_SOME(parsed, MimeType::tryParse(str)) {
    return js.str(parsed.essence());
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::mimeTypeParameter(
    jsg::Lock& js, jsg::JsValue input, jsg::JsValue name) {
  auto str = JSG_REQUIRE_NONNULL(
      input.tryCast<jsg::JsString>(), TypeError, "mimeTypeParameter() expects a string")
                 .toString(js);
  auto key = JSG_REQUIRE_NONNULL(
      name.tryCast<jsg::JsString>(), TypeError, "mimeTypeParameter() expects a string")
                 .toString(js);
  KJ_IF_SOME(value, readContentTypeParameter(str, key)) {
    return js.str(value);
  }
  return js.undefined();
}

jsg::JsValue BootstrapUtils::newIdentityPipe(jsg::Lock& js) {
  JSG_REQUIRE(IoContext::hasCurrent(), Error,
      "newIdentityPipe() can only be called within the context of a request.");
  auto& ioContext = IoContext::current();
  auto pipe = api::newIdentityPipe();
  auto readable = JsReadableStream::create(js, ioContext, kj::mv(pipe.in));
  auto writable = JsWritableStream::create(js, ioContext, kj::mv(pipe.out), kj::none);
  return js.arr(
      jsg::JsValue(kj::mv(readable).toJsObject(js)), jsg::JsValue(kj::mv(writable).toJsObject(js)));
}

jsg::JsValue BootstrapUtils::pumpTo(jsg::Lock& js, jsg::JsValue stream, jsg::JsValue sink) {
  JSG_REQUIRE(IoContext::hasCurrent(), Error,
      "pumpTo() can only be called within the context of a request.");
  auto& ioContext = IoContext::current();
  auto readable = JSG_REQUIRE_NONNULL(
      tryUnwrapReadableStream(js, stream), TypeError, "pumpTo() expects a ReadableStream");
  auto sinkObject =
      JSG_REQUIRE_NONNULL(sink.tryCast<jsg::JsObject>(), TypeError, "pumpTo() expects a sink");
  auto pump =
      readable.pumpTo(js, kj::heap<CallbackSink>(ioContext, sinkObject.addRef(js)), EndStream::YES);
  return jsg::JsValue(
      ioContext.awaitIo(js, ioContext.waitForDeferredProxy(kj::mv(pump))).consumeHandle(js));
}

jsg::JsValue BootstrapUtils::readableStreamState(jsg::Lock& js, jsg::JsValue value) {
  KJ_IF_SOME(stream, tryUnwrapReadableStream(js, value)) {
    if (stream.isLocked(js)) return js.str("locked"_kj);
    if (stream.isDisturbed(js)) return js.str("disturbed"_kj);
    return js.str("readable"_kj);
  }
  return js.undefined();
}

}  // namespace workerd::api
