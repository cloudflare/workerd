// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "kj/common.h"

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/api/streams/readable-source.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>
#include <workerd/io/features.h>
#include <workerd/io/per-isolate-bootstrap.h>
#include <workerd/jsg/jsg.h>

#include <kj/debug.h>

namespace workerd::api {

namespace {
// The exact error message produced when attempting to consume an already-consumed body. This
// text is user-visible and load-bearing: it matches the message historically produced by the
// Body mixin (see http.c++) and must not change.
constexpr kj::StringPtr kBodyUsedError =
    "Body has already been used. It can only be used once. Use tee() first if you need to "
    "read it twice."_kj;

// Convert a kj::String to owned bytes, excluding the trailing NUL terminator. The returned array
// owns the original character buffer but views only the string's bytes.
kj::Array<const kj::byte> stringToBytes(kj::String data) {
  size_t len = data.size();
  auto chars = data.releaseArray();
  return chars.first(len).asBytes().attach(kj::mv(chars));
}

// Copy the bytes viewed by a JsBufferSource into an owned array. Copying (rather than retaining the
// JsBufferSource) severs the dependency on the V8 backing store, which could otherwise be freed if
// the source ArrayBuffer is detached/transferred and then garbage collected. This also satisfies
// the Fetch requirement to copy the input buffer.
kj::Array<const kj::byte> bufferSourceToBytes(jsg::Lock& js, jsg::JsBufferSource view) {
  return kj::heapArray<kj::byte>(view.asArrayPtr());
}

// Fetches a named function export of the webstreams/cpp_exports bootstrap module. The
// module is eagerly required by the bootstrap when the typescript_implemented_streams
// compat flag is enabled, so lookups can only fail on internal errors or gross
// misconfiguration (e.g. the flag enabled without the bootstrap autogate); the resulting
// "internal error" surfaced to user code is intended.
jsg::JsFunction getCppExport(jsg::Lock& js, kj::StringPtr name) {
  auto cppExports = KJ_REQUIRE_NONNULL(tryGetBootstrapExport(js, "webstreams/cpp_exports"));
  auto cppExportsObj = KJ_REQUIRE_NONNULL(cppExports.tryCast<jsg::JsObject>());
  return KJ_REQUIRE_NONNULL(cppExportsObj.get(js, name).tryCast<jsg::JsFunction>());
}

template <jsg::IsJsValue... Args>
jsg::JsValue dispatchCall(jsg::Lock& js, kj::StringPtr name, Args... args) {
  auto func = getCppExport(js, name);
  return func.call(js, js.undefined(), kj::fwd<Args>(args)...);
}

// Calls the named method on the given object, with the object itself as the receiver.
// Used to invoke the TypeScript conduit's controller facade methods (enqueue, close,
// respond, ...). The facade objects are module-owned TypeScript code, not user objects,
// so a missing method indicates an internal error.
template <jsg::IsJsValue... Args>
jsg::JsValue invokeMethod(jsg::Lock& js, jsg::JsObject obj, kj::StringPtr name, Args... args) {
  auto func =
      KJ_REQUIRE_NONNULL(obj.get(js, name).tryCast<jsg::JsFunction>(), "method not found", name);
  return func.call(js, obj, args...);
}

bool getReadableStreamIsDisturbed(jsg::Lock& js, jsg::JsObject obj) {
  return dispatchCall(js, "getReadableStreamIsDisturbed", obj).isTrue();
}

// The TypeScript implementation's private-brand check. True only for genuine
// TypeScript-implemented ReadableStream instances (including subclasses); false for
// everything else, including proxies wrapping a stream (private fields do not tunnel
// through proxies, deliberately matching the TS-side behavior).
bool isTypeScriptReadableStream(jsg::Lock& js, jsg::JsObject obj) {
  return dispatchCall(js, "isReadableStream", obj).isTrue();
}

bool getReadableStreamIsLocked(jsg::Lock& js, jsg::JsObject obj) {
  return dispatchCall(js, "isReadableStreamLocked", obj).isTrue();
}

jsg::Promise<void> readableStreamCancel(
    jsg::Lock& js, jsg::JsObject obj, jsg::Optional<jsg::JsValue>& reason) {
  jsg::JsValue result =
      dispatchCall(js, "readableStreamCancel", obj, reason.orDefault(js.undefined()));
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toVoidPromise(promise);
}

void setReadableStreamPendingClosure(jsg::Lock& js, jsg::JsObject obj) {
  // The result is undefined/ignored
  auto res KJ_UNUSED = dispatchCall(js, "setReadableStreamPendingClosure", obj);
}

jsg::Promise<void> getReadableStreamOnEof(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = dispatchCall(js, "getReadableStreamOnEof", obj);
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toVoidPromise(promise);
}

kj::Maybe<uint64_t> getReadableStreamExpectedLength(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = dispatchCall(js, "getReadableStreamExpectedLength", obj);
  KJ_IF_SOME(bi, result.tryCast<jsg::JsBigInt>()) {
    KJ_IF_SOME(len, bi.tryToUint64(js)) {
      return len;
    }
  }
  return kj::none;
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> getReadableStreamArrayBuffer(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      dispatchCall(js, "consumeReadableStreamAsArrayBuffer", obj, js.bigInt(limit));
  // The result must be a promise for an Arraybuffer
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(value.tryCast<jsg::JsArrayBuffer>());
    return ab.addRef(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> getReadableStreamBytes(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      dispatchCall(js, "consumeReadableStreamAsUint8Array", obj, js.bigInt(limit));
  // The result must be a promise for an Uint8Array
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(value.tryCast<jsg::JsUint8Array>());
    return ab.addRef(js);
  });
}

jsg::Promise<kj::String> getReadableStreamText(jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result = dispatchCall(js, "consumeReadableStreamAsText", obj, js.bigInt(limit));
  // The result must be a promise for a String
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    return value.toString(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> getReadableStreamJson(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result = dispatchCall(js, "consumeReadableStreamAsJSON", obj, js.bigInt(limit));
  // The result must be a promise for a JS value
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toPromise(promise).then(
      js, [](jsg::Lock& js, jsg::Value ref) { return jsg::JsValue(ref.getHandle(js)).addRef(js); });
}

jsg::Promise<jsg::Ref<Blob>> getReadableStreamBlob(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit, kj::String contentType) {
  jsg::JsValue result =
      dispatchCall(js, "consumeReadableStreamAsArrayBuffer", obj, js.bigInt(limit));
  // The result must be a promise for an Arraybuffer
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(result.tryCast<jsg::JsPromise>());
  return js.toPromise(promise).then(
      js, [contentType = kj::mv(contentType)](jsg::Lock& js, jsg::Value ref) mutable {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(value.tryCast<jsg::JsArrayBuffer>());
    return js.alloc<Blob>(js, jsg::JsBufferSource(ab), kj::mv(contentType));
  });
}

// Adapts a ReadableStreamSource into a kj::AsyncInputStream so that it can feed
// kj::newTee() -- the generic tee fallback for sources without an optimized tryTee().
class TeeInputAdapter final: public kj::AsyncInputStream {
 public:
  TeeInputAdapter(kj::Own<ReadableStreamSource> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength(StreamEncoding::IDENTITY);
  }

 private:
  kj::Own<ReadableStreamSource> inner;
};

// Adapts a kj::newTee() branch back into a ReadableStreamSource so that a branch
// ReadableStreamNativeSource can own it.
class TeeBranchSource final: public ReadableStreamSource {
 public:
  TeeBranchSource(kj::Own<kj::AsyncInputStream> inner): inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      return inner->tryGetLength();
    }
    return kj::none;
  }

  void cancel(kj::Exception reason) override {
    // Nothing to do here: dropping the branch is the real cancellation (kj::newTee
    // releases the upstream once every branch is gone), and the owning
    // ReadableStreamNativeSource drops us right after this call. This matches the legacy
    // internal controller's tee branches, whose cancel() is likewise a no-op.
  }

 private:
  kj::Own<kj::AsyncInputStream> inner;
};

// An always-EOF ReadableStreamSource, used when pumping a native-backed stream whose
// source already completed (extraction of closed streams is legal per the contract; the
// pump simply finishes).
class NullSource final: public ReadableStreamSource {
 public:
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return static_cast<size_t>(0);
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    return static_cast<uint64_t>(0);
  }

  void cancel(kj::Exception reason) override {
    // Nothing to do here.
  }
};

// Serves the given prefix bytes, then delegates to the inner source. Used for the rare
// pump-with-stashed-bytes case (a tee-seeded branch extracted before being read).
// Deliberately does NOT override pumpTo(): the generic pump loop is used, at the cost of
// deferred proxying, which only this rare path pays.
// TODO(streams-ts): Since the prefix is a kj::Array, this actually can implement
// pumpTo and support deferred proxying.
class PrefixedSource final: public ReadableStreamSource {
 public:
  PrefixedSource(kj::Array<kj::byte> prefix, kj::Own<ReadableStreamSource> inner)
      : maybePrefix(kj::mv(prefix)),
        inner(kj::mv(inner)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    auto dest = kj::arrayPtr(static_cast<kj::byte*>(buffer), maxBytes);
    KJ_IF_SOME(prefix, maybePrefix) {
      if (prefix.view != nullptr) {
        size_t amount = kj::min(dest.size(), prefix.view.size());
        // Because the AI review agent likes to flag this, the dest.write(...)
        // will advance the internal pointer of dest by amount, so the tryRead
        // below picks up at the right place and does not overwrite the prefix
        // bytes.
        dest.write(prefix.view.first(amount));
        prefix.view = prefix.view.slice(amount);
        if (prefix.view == nullptr) {
          // We have fully consumed the prefix. Clear it out.
          maybePrefix = kj::none;
        }
        if (amount >= minBytes) {
          co_return amount;
        }
        minBytes -= amount;
        maxBytes -= amount;
        size_t n = co_await inner->tryRead(dest.begin(), minBytes, maxBytes);
        co_return amount + n;
      } else {
        maybePrefix = kj::none;
      }
    }
    co_return co_await inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    if (encoding == StreamEncoding::IDENTITY) {
      KJ_IF_SOME(length, inner->tryGetLength(encoding)) {
        size_t prefixSize = 0;
        KJ_IF_SOME(prefix, maybePrefix) {
          prefixSize = prefix.view.size();
        }
        return length + prefixSize;
      }
    }
    return kj::none;
  }

  void cancel(kj::Exception reason) override {
    maybePrefix = kj::none;
    inner->cancel(kj::mv(reason));
  }

 private:
  struct Prefix {
    kj::Array<kj::byte> owned;
    kj::ArrayPtr<const kj::byte> view;
    Prefix(kj::Array<kj::byte> owned): owned(kj::mv(owned)), view(this->owned.asPtr()) {}
  };
  kj::Maybe<Prefix> maybePrefix;
  kj::Own<ReadableStreamSource> inner;
};

// Pumps an extracted native source into the sink, mirroring the legacy internal
// controller's pump (ReadableStreamInternalController::pumpTo): the sink and source ride
// a refcounted holder attached through both deferred-proxy phases; dropping the pump
// cancels the source; a pump failure aborts the sink and cancels the source.
kj::Promise<DeferredProxy<void>> pumpExtractedSource(
    kj::Own<ReadableStreamSource> source, kj::Own<WritableStreamSink> sink, bool end) {
  struct Holder {
    kj::Own<WritableStreamSink> sink;
    kj::Own<ReadableStreamSource> source;
    bool done = false;

    Holder(kj::Own<WritableStreamSink> sink, kj::Own<ReadableStreamSource> source)
        : sink(kj::mv(sink)),
          source(kj::mv(source)) {}
    ~Holder() noexcept(false) {
      if (!done) {
        // The pump was canceled (e.g. the client disconnected); make sure the source
        // finds out so anything feeding it doesn't hang.
        source->cancel(KJ_EXCEPTION(DISCONNECTED, "pump canceled"));
      }
    }
  };

  auto holder = kj::rc<Holder>(kj::mv(sink), kj::mv(source));
  return holder->source->pumpTo(holder->sink->getPtr(), end)
      .then([holder = holder.addRef()](DeferredProxy<void> proxy) mutable -> DeferredProxy<void> {
    proxy.proxyTask = proxy.proxyTask.attach(holder.addRef());
    holder->done = true;
    return kj::mv(proxy);
  }, [holder = holder.addRef()](kj::Exception&& exception) mutable -> DeferredProxy<void> {
    holder->sink->abort(exception.clone());
    holder->source->cancel(exception.clone());
    holder->done = true;
    kj::throwFatalException(kj::mv(exception));
  });
}

// Writes one drained batch to the sink and, when this is the final batch, ends it. A
// free-standing coroutine (NOT a capturing lambda) so that everything it touches across
// suspension points lives in the coroutine frame: `pieces` is moved in (the vectored
// write's pointers reference its arrays and must stay valid for the duration of the
// write), and `endAfter` is copied in. `sink` is passed by reference: the owning
// IoOwn handle rides in queuedPumpStep's post-write continuation, which cannot be
// released while this coroutine is pending (the pending awaitIo holds the resolver that
// keeps the continuation reachable), and cancellation destroys the coroutine at its
// suspension point without touching the sink again.
kj::Promise<void> queuedWriteStep(
    WritableStreamSink& sink, kj::Array<kj::Array<const kj::byte>> pieces, bool endAfter) {
  if (pieces.size() > 0) {
    auto ptrs = KJ_MAP(piece, pieces) -> kj::ArrayPtr<const kj::byte> { return piece.asPtr(); };
    co_await sink.write(ptrs.asPtr());
  }
  if (endAfter) {
    co_await sink.end();
  }
}

// One iteration of the queued-backend pump: collect everything the draining reader has
// buffered (one isolate-lock trip per batch), copy it to KJ-owned memory, perform a
// vectored write, and recurse until done. All JS state (the reader) travels through the
// jsg promise chain.
jsg::Promise<void> queuedPumpStep(jsg::Lock& js,
    jsg::JsRef<jsg::JsObject> reader,
    IoOwn<WritableStreamSink> sink,
    EndStream end) {
  auto& context = IoContext::current();
  auto readResult = invokeMethod(js, reader.getHandle(js), "read"_kj);
  auto readPromise = KJ_REQUIRE_NONNULL(readResult.tryCast<jsg::JsPromise>());
  return js.toPromise(readPromise)
      .then(js,
          context.addFunctor([reader = kj::mv(reader), sink = kj::mv(sink), end](
                                 jsg::Lock& js, jsg::Value value) mutable -> jsg::Promise<void> {
    auto& context = IoContext::current();
    auto result = KJ_REQUIRE_NONNULL(jsg::JsValue(value.getHandle(js)).tryCast<jsg::JsObject>());
    bool done = result.get(js, "done"_kj).isTrue();
    auto chunks = KJ_REQUIRE_NONNULL(result.get(js, "chunks"_kj).tryCast<jsg::JsArray>());

    // Only ArrayBuffer/ArrayBufferView chunks are usable as bytes. Everything else
    // (including strings) rejects, matching the legacy pump's historical behavior and
    // error text exactly.
    auto pieces = kj::Vector<kj::Array<const kj::byte>>(chunks.size());
    for (uint32_t i = 0; i < chunks.size(); i++) {
      auto chunk = chunks.get(js, i);
      KJ_IF_SOME(view, chunk.tryCast<jsg::JsArrayBufferView>()) {
        pieces.add(bufferSourceToBytes(js, jsg::JsBufferSource(view)));
      } else KJ_IF_SOME(buffer, chunk.tryCast<jsg::JsArrayBuffer>()) {
        pieces.add(bufferSourceToBytes(js, jsg::JsBufferSource(buffer)));
      } else {
        JSG_FAIL_REQUIRE(TypeError, "This ReadableStream did not return bytes.");
      }
    }

    // Evaluation order matters: `*sink` is dereferenced (IoContext current here) and the
    // coroutine launched before `kj::mv(sink)` in the continuation's capture-init runs.
    // Moving the IoOwn transfers only the handle -- the sink object itself lives in the
    // IoContext and its address is stable -- so the reference remains valid.
    bool endAfter = done && end == EndStream::YES;
    return context.awaitIo(js, queuedWriteStep(*sink, pieces.releaseAsArray(), endAfter))
        .then(js,
            context.addFunctor(
                [reader = kj::mv(reader), sink = kj::mv(sink), end, done](jsg::Lock& js) mutable {
      if (done) {
        return js.resolvedPromise();
      }
      return queuedPumpStep(js, kj::mv(reader), kj::mv(sink), end);
    }));
  }));
}

// Pumps a queued-backed (JS underlying source) TypeScript stream into the sink by
// driving the internal ReadableStreamDrainingReader. Isolate-bound (the JS conduit is in
// the data path), so the deferred-proxy phase is a no-op.
kj::Promise<DeferredProxy<void>> pumpQueuedTsStream(jsg::Lock& js,
    IoContext& context,
    jsg::JsObject reader,
    kj::Own<WritableStreamSink> sink,
    EndStream end) {
  auto loop =
      queuedPumpStep(js, reader.addRef(js), context.addObject(kj::mv(sink)), end)
          .catch_(js, [reader = reader.addRef(js)](jsg::Lock& js, jsg::Value exception) mutable {
    // The pump failed (source error, non-byte chunk, or sink failure): cancel the reader
    // so the underlying source learns the pipe is gone, then propagate the failure.
    // TODO(streams-ts): consider explicitly aborting the sink as well, for exact parity
    // with the legacy pump's error path.
    auto reason = jsg::JsValue(exception.getHandle(js));
    // Call the reader's cancel() method. It returns a promise, but we are not
    // going to await it, the pump is already failing.
    auto cancelResult = invokeMethod(js, reader.getHandle(js), "cancel"_kj, reason);
    KJ_IF_SOME(promise, cancelResult.tryCast<jsg::JsPromise>()) {
      promise.markAsHandled(js);
    }
    js.throwException(kj::mv(exception));
  });
  return addNoopDeferredProxy(context.awaitJs(js, kj::mv(loop)));
}
}  // namespace

// The -Wdangling-field warnings below are false positives. view captures the heap buffer pointer
// managed by data, not the address of the data parameter itself. Moving data into owned transfers
// ownership of that heap buffer without changing its address, so view remains valid.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdangling-field"
JsReadableStream::Buffer::Buffer(kj::Array<const kj::byte> data): view(data), owned(kj::mv(data)) {}

JsReadableStream::Buffer::Buffer(jsg::Ref<Blob> data): view(data->getData()), owned(kj::mv(data)) {}
#pragma clang diagnostic pop

JsReadableStream::Impl JsReadableStream::bufferBackedImpl(jsg::Lock& js, kj::Rc<Buffer> buffer) {
  // Use streams::newMemorySource() rather than newSystemStream() wrapping a memory input stream:
  // buffer-backed bodies may have V8 heap provenance and therefore must NOT be deferred-proxied.
  // The data must be consumed and destroyed while under the isolate lock.
  //
  // TODO(streams-ts): Like create(), the stream construction here must dispatch on the
  // worker's configuration once the TypeScript implementation lands.
  auto view = buffer->view;
  auto source = streams::newMemorySource(view, buffer.addRef().toOwn());
  return Impl{
    .stream = StreamImpl(js.alloc<ReadableStream>(IoContext::current(), kj::mv(source))),
    .maybeOwnedBuffer = kj::mv(buffer),
  };
}

JsReadableStream::JsReadableStream(jsg::Ref<ReadableStream> stream)
    : impl(Impl{
        .stream = StreamImpl(kj::mv(stream)),
        .maybeOwnedBuffer = kj::none,
      }) {}

JsReadableStream::JsReadableStream(jsg::Lock&, jsg::JsRef<jsg::JsObject> obj)
    : impl(Impl{
        .stream = StreamImpl(kj::mv(obj)),
      }) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, kj::Array<const kj::byte> data)
    : impl(bufferBackedImpl(js, kj::rc<Buffer>(kj::mv(data)))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, kj::String data)
    : JsReadableStream(js, stringToBytes(kj::mv(data))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::JsRef<jsg::JsBufferSource> view)
    : JsReadableStream(js, bufferSourceToBytes(js, view.getHandle(js))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<Blob> blob)
    : impl(bufferBackedImpl(js, kj::rc<Buffer>(kj::mv(blob)))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<api::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream::JsReadableStream(
    jsg::Lock& js, jsg::Ref<api::url::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream JsReadableStream::create(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source) {
  if (FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    // TypeScript-implemented streams: wrap the native source in a
    // ReadableStreamNativeSource -- whose instances are born carrying the kNativeSource
    // marker (JSG_PRIVATE_SYMBOL) that the TypeScript ReadableStream constructor detects
    // -- and construct the TypeScript stream over it via the constructor exposed through
    // the bootstrap's cpp_exports module.
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto sourceObj = jsg::JsValue(
        handler.wrap(js, js.alloc<ReadableStreamNativeSource>(ioContext, kj::mv(source))));
    auto constructor = getCppExport(js, "ReadableStream");
    return JsReadableStream(js, constructor.newInstance(js, sourceObj).addRef(js));
  }
  return JsReadableStream(js.alloc<ReadableStream>(ioContext, kj::mv(source)));
}

kj::Maybe<JsReadableStream> JsReadableStream::tryUnwrapTs(
    jsg::Lock& js, v8::Local<v8::Value> handle) {
  // Without the flag there is no TypeScript implementation (and no bootstrap export to
  // ask), so nothing can match. This also keeps the flag-off unwrap path allocation- and
  // JS-call-free.
  if (!FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    return kj::none;
  }
  KJ_IF_SOME(obj, jsg::JsValue(handle).tryCast<jsg::JsObject>()) {
    // PERF NOTE: this is a JS call per unwrap attempt on any object-typed value. Since
    // JsReadableStream is typically the first alternative in consumer OneOfs (e.g.
    // Body::Initializer), object bodies that are NOT streams (ArrayBuffer, Blob, FormData,
    // ...) pay it before falling through. If this shows up in profiles, the alternative is
    // an own api-symbol marker stamped by the conduit constructor (same machinery as
    // kNativeSource) -- see the design doc's unwrap decision entry.
    if (isTypeScriptReadableStream(js, obj)) {
      return JsReadableStream(js, obj.addRef(js));
    }
  }
  return kj::none;
}

JsReadableStream JsReadableStream::addRef(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return JsReadableStream(Impl{
          .stream = StreamImpl(stream.addRef()),
          .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return JsReadableStream(Impl{
          .stream = StreamImpl(obj.addRef(js)),
          .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
        });
      }
    }
    KJ_UNREACHABLE;
  }
  // addRef() of a null stream is a null stream.
  return JsReadableStream();
}

bool JsReadableStream::isNull() const {
  return impl == kj::none;
}

bool JsReadableStream::isBufferBacked() const {
  KJ_IF_SOME(i, impl) {
    return i.maybeOwnedBuffer != kj::none;
  }
  return false;
}

bool JsReadableStream::isDisturbed(jsg::Lock& js) {
  // Disturbed is a one-way switch
  if (cachedIsDisturbed) return true;
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return cachedIsDisturbed = stream->isDisturbed();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return cachedIsDisturbed = getReadableStreamIsDisturbed(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

bool JsReadableStream::isLocked(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->isLocked();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamIsLocked(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
  return false;
}

jsg::Promise<void> JsReadableStream::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->cancel(js, kj::mv(reason));
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // TODO(streams-ts): This bypasses the locked check. We need a variant
        // that rejects if the stream is locked.
        return readableStreamCancel(js, obj.getHandle(js), reason);
      }
    }
    KJ_UNREACHABLE;
  }
  // Canceling a null stream is a no-op.
  return js.resolvedPromise();
}

jsg::Promise<void> JsReadableStream::forceCancel(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        // Going through the controller (rather than ReadableStream::cancel()) deliberately
        // bypasses the "is locked" check: this cancels the stream out from under any reader.
        return stream->getController().cancel(js, kj::mv(reason));
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return readableStreamCancel(js, obj.getHandle(js), reason);
      }
    }
    KJ_UNREACHABLE;
  }
  // Canceling a null stream is a no-op.
  return js.resolvedPromise();
}

void JsReadableStream::setPendingClosure(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->getController().setPendingClosure();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return setReadableStreamPendingClosure(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
}

jsg::Promise<void> JsReadableStream::onEof(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "onEof() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      return stream->onEof(js);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      return getReadableStreamOnEof(js, obj.getHandle(js));
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> JsReadableStream::tryGetLength(jsg::Lock& js, StreamEncoding encoding) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->tryGetLength(encoding);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamExpectedLength(js, obj.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> JsReadableStream::arrayBuffer(
    jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsArrayBuffer>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamArrayBuffer(js, obj.getHandle(js), limit);
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(jsg::JsArrayBuffer::create(js, 0).addRef(js));
}

jsg::Promise<kj::String> JsReadableStream::text(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<kj::String>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllText(js, limit);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamText(js, obj.getHandle(js), limit);
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(kj::String());
}

jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> JsReadableStream::bytes(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsUint8Array>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit).then(
            js, [](jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> data) {
          auto handle = data.getHandle(js);
          return jsg::JsUint8Array::create(js, handle).addRef(js);
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamBytes(js, obj.getHandle(js), limit);
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty result.
  return js.resolvedPromise(jsg::JsUint8Array::create(js, static_cast<size_t>(0)).addRef(js));
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> JsReadableStream::json(jsg::Lock& js, uint64_t limit) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::JsRef<jsg::JsValue>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllText(js, limit).then(
            js, [](jsg::Lock& js, kj::String text) {
          auto parsed = js.parseJson(text);
          return jsg::JsValue(parsed.getHandle(js)).addRef(js);
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamJson(js, obj.getHandle(js), limit);
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream is an empty body. Match Body::json() semantics exactly: resolve the empty
  // text first, then parse it in a continuation. Parsing "" as JSON throws a SyntaxError, so
  // the returned promise rejects.
  return js.resolvedPromise(kj::String()).then(js, [](jsg::Lock& js, kj::String text) {
    auto parsed = js.parseJson(text);
    return jsg::JsValue(parsed.getHandle(js)).addRef(js);
  });
}

jsg::Promise<jsg::Ref<Blob>> JsReadableStream::blob(
    jsg::Lock& js, uint64_t limit, kj::String contentType) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        if (stream->isDisturbed()) {
          return js.rejectedPromise<jsg::Ref<Blob>>(js.typeError(kBodyUsedError));
        }
        return stream->getController().readAllBytes(js, limit).then(js,
            [contentType = kj::mv(contentType)](
                jsg::Lock& js, jsg::JsRef<jsg::JsArrayBuffer> buffer) mutable {
          return js.alloc<Blob>(js, jsg::JsBufferSource(buffer.getHandle(js)), kj::mv(contentType));
        });
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        return getReadableStreamBlob(js, obj.getHandle(js), limit, kj::mv(contentType));
      }
    }
    KJ_UNREACHABLE;
  }

  // A null stream yields an empty Blob with the given Content-Type.
  return js.resolvedPromise(js.alloc<Blob>(kj::mv(contentType)));
}

// pumpTo and pipeTo/pipeThrough are similar but serve different purposes.
// pumpTo is a low-level primitive that pumps bytes from a ReadableStream
// to a WritableStreamSink, always internal, and potentially supporting
// deferred proxying. pipeTo and pipeThrough are higher-level operations that
// pump data from a ReadableStream to a WritableStream.

kj::Promise<DeferredProxy<void>> JsReadableStream::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, EndStream end) {
  auto& i = KJ_ASSERT_NONNULL(impl, "pumpTo() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      return stream->pumpTo(js, kj::mv(sink), end == EndStream::YES);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Precondition parity with the legacy arm (ReadableStream::pumpTo).
      JSG_REQUIRE(IoContext::hasCurrent(), Error,
          "Unable to consume this ReadableStream outside of a request");
      auto handle = obj.getHandle(js);
      JSG_REQUIRE(!getReadableStreamIsLocked(js, handle), TypeError,
          "The ReadableStream has been locked to a reader.");
      auto& context = IoContext::current();

      // Classify the backend by probing for the extraction marker: an own property keyed
      // by the kExtractNativeSource API-registry symbol, present only on native-backed
      // streams (see the contract in src/per_isolate/webstreams/native.ts).
      auto extractSymbol = jsg::JsValue(
          v8::Symbol::ForApi(js.v8Isolate, jsg::v8StrIntern(js.v8Isolate, "kExtractNativeSource")));
      if (handle.has(js, extractSymbol, jsg::JsObject::HasOption::OWN)) {
        // Native-backed: extract the underlying source and pump entirely at the C++
        // layer, preserving each source's own deferred-proxy behavior. The extractor is
        // atomic (validate, detach, lock + disturb) and returns the source wrapper.
        auto extractor =
            KJ_REQUIRE_NONNULL(handle.get(js, extractSymbol).tryCast<jsg::JsFunction>());
        auto sourceObj = extractor.call(js, handle);
        auto& handler =
            KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
        auto source = KJ_REQUIRE_NONNULL(handler.tryUnwrap(js, sourceObj),
            "the extractor did not return a ReadableStreamNativeSource");
        auto released = source->releaseForPump(js);
        if (released.prefix.size() > 0) {
          released.source =
              kj::heap<PrefixedSource>(kj::mv(released.prefix), kj::mv(released.source));
        }
        return pumpExtractedSource(kj::mv(released.source), kj::mv(sink), end == EndStream::YES);
      }

      // Queued-backed (JS underlying source): the JS conduit stays in the data path, so
      // drive the internal DrainingReader batch by batch under the isolate lock.
      auto reader = KJ_REQUIRE_NONNULL(
          dispatchCall(js, "acquireReadableStreamDrainingReader", handle).tryCast<jsg::JsObject>());
      return pumpQueuedTsStream(js, context, reader, kj::mv(sink), end);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> JsReadableStream::pipeTo(
    jsg::Lock& js, JsWritableStream& destination, PipeToOptions options) {
  auto& i = KJ_ASSERT_NONNULL(impl, "pipeTo() called on a null JsReadableStream");
  auto& d = KJ_ASSERT_NONNULL(
      destination.impl, "pipeTo() called with a null destination JsWritableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      KJ_SWITCH_ONEOF(d.stream) {
        KJ_CASE_ONEOF(writable, jsg::Ref<WritableStream>) {
          // Both ends are C++: delegate to ReadableStream::pipeTo, which preserves the exact
          // observable behavior (locked-end rejections and their texts) and internally selects
          // the most efficient pump for the endpoint types. The internal pipe keeps only a bare
          // reference to the source (WritableStreamInternalController::PipeLocked) and the
          // destination controller holds only a kj::Weak<WritableStream> owner, so retain both
          // ends across the pipe ourselves rather than making liveness the caller's
          // responsibility.
          // Note: there's no use for using JSG_VISITABLE_LAMBDA here since promise
          // continuations are never actually visited for GC.
          return stream->pipeTo(js, writable.addRef(), kj::mv(options))
              .then(js, [source = stream.addRef(), dest = writable.addRef()](jsg::Lock& js) {});
        }
        KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
          // TODO(streams-ts): TS/TS pipes go through the TS pipeTo hook; mixed-backend pipes
          // are a wiring-session concern.
          KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
        }
      }
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

JsReadableStream JsReadableStream::pipeThrough(
    jsg::Lock& js, JsReadableWritablePair transform, PipeToOptions options) {
  auto& i = KJ_ASSERT_NONNULL(impl, "pipeThrough() called on a null JsReadableStream");
  auto& r = KJ_ASSERT_NONNULL(
      transform.readable.impl, "pipeThrough() called with a null transform.readable");
  auto& w = KJ_ASSERT_NONNULL(
      transform.writable.impl, "pipeThrough() called with a null transform.writable");

  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      KJ_SWITCH_ONEOF(w.stream) {
        KJ_CASE_ONEOF(writable, jsg::Ref<WritableStream>) {
          KJ_SWITCH_ONEOF(r.stream) {
            KJ_CASE_ONEOF(readable, jsg::Ref<ReadableStream>) {
              // All three arms are C++: delegate to ReadableStream::pipeThrough, which preserves
              // the exact observable behavior (synchronous locked-end throws and their texts,
              // the pipeThrough-marked handled pipe promise, and the source keepalive).
              auto returned = stream->pipeThrough(js,
                  ReadableStream::Transform{
                    .readable = readable.addRef(),
                    .writable = writable.addRef(),
                  },
                  kj::mv(options));
              // ReadableStream::pipeThrough returns the transform's readable untouched. Return
              // the abstraction value it arrived in (the same underlying stream), which
              // preserves any additional abstraction-level state (e.g. buffer-backedness).
              KJ_ASSERT(returned.get() == readable.get());
              return kj::mv(transform.readable);
            }
            KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
              KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
            }
          }
          KJ_UNREACHABLE;
        }
        KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
          // TODO(streams-ts): all-TS pipes go through the TS pipeThrough hook; mixed-backend
          // pipes are a wiring-session concern.
          KJ_UNIMPLEMENTED("TypeScript-backed WritableStream is not yet supported");
        }
      }
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

JsReadableStream::Tee JsReadableStream::tee(jsg::Lock& js) {
  auto& i = KJ_ASSERT_NONNULL(impl, "tee() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      auto branches = stream->tee(js);
      // Both branches share the retransmit buffer (if any) so they remain independently rewindable.
      auto buffer1 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });
      auto buffer2 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });

      // tee() consumes `this`: the original stream is now locked/disturbed and unusable, so
      // represent that by nullifying. Everything needed for the result has already been extracted
      // into locals above.
      impl = kj::none;

      return Tee{
        .branch1 = JsReadableStream(Impl{
          .stream = StreamImpl(kj::mv(branches[0])),
          .maybeOwnedBuffer = kj::mv(buffer1),
        }),
        .branch2 = JsReadableStream(Impl{
          .stream = StreamImpl(kj::mv(branches[1])),
          .maybeOwnedBuffer = kj::mv(buffer2),
        }),
      };
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Delegate to the TypeScript implementation's internal tee (the same
      // backend-dispatched machinery ReadableStream.prototype.tee uses, including the
      // native-source tee hook for C++-backed streams). It throws (e.g. for a locked
      // stream) exactly as the legacy arm does; the exception propagates as-is.
      auto result = dispatchCall(js, "readableStreamTee", obj.getHandle(js));
      auto branches = KJ_REQUIRE_NONNULL(
          result.tryCast<jsg::JsArray>(), "readableStreamTee must return an array");
      KJ_REQUIRE(branches.size() == 2, "readableStreamTee must return two branches");
      auto branch1 = KJ_REQUIRE_NONNULL(branches.get(js, 0).tryCast<jsg::JsObject>());
      auto branch2 = KJ_REQUIRE_NONNULL(branches.get(js, 1).tryCast<jsg::JsObject>());

      // TS-backed streams cannot currently be buffer-backed (bufferBackedImpl always
      // constructs the legacy arm; design-doc open question F), but carry the buffer
      // through anyway so this arm stays correct if F ever changes that.
      auto buffer1 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });
      auto buffer2 = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); });

      // Same consumption contract as the legacy arm: the original JS stream object is
      // left locked by the tee (spec behavior); this wrapper drops its handle.
      impl = kj::none;

      return Tee{
        .branch1 = JsReadableStream(Impl{
          .stream = StreamImpl(branch1.addRef(js)),
          .maybeOwnedBuffer = kj::mv(buffer1),
        }),
        .branch2 = JsReadableStream(Impl{
          .stream = StreamImpl(branch2.addRef(js)),
          .maybeOwnedBuffer = kj::mv(buffer2),
        }),
      };
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<JsReadableStream> JsReadableStream::tryClone(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_IF_SOME(buffer, i.maybeOwnedBuffer) {
      // Non-mutating: build a fresh stream over the same buffer bytes, carrying the buffer forward
      // so the clone is itself rewindable. `this` is left untouched.
      return JsReadableStream(bufferBackedImpl(js, buffer.addRef()));
    }
  }
  // Null or not buffer-backed: nothing to rewind.
  return kj::none;
}

JsReadableStream JsReadableStream::detach(jsg::Lock& js, IgnoreDisturbed ignoreDisturbed) {
  auto& i = KJ_ASSERT_NONNULL(impl, "detach() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      // ReadableStream::detach() takes over the internal state, leaving the original stream (i.e.
      // `this`) locked and disturbed. `this` retains a reference to the retransmit buffer.
      auto detached = stream->detach(js, ignoreDisturbed == IgnoreDisturbed::YES);
      return JsReadableStream(Impl{
        .stream = StreamImpl(kj::mv(detached)),
        .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
      });
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      KJ_UNIMPLEMENTED("TypeScript-backed ReadableStream is not yet supported");
    }
  }
  KJ_UNREACHABLE;
}

void JsReadableStream::nullify() {
  impl = kj::none;
}

void JsReadableStream::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        visitor.visit(stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        visitor.visit(obj);
      }
    }
    // Note: the retransmit Buffer's owned Blob reference is intentionally NOT traced here, matching
    // the pre-refactor Body behavior (it is kept alive as a strong reference, not via GC tracing).
  }
}

void JsReadableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        tracker.trackField("stream", stream);
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // TODO(streams-ts): track the JS object's memory once TS-backed streams are supported.
        // (jsg::JsRef does not satisfy the MemoryRetainer concept, so it can't be passed to
        // trackField() directly.)
      }
    }
    KJ_IF_SOME(buffer, i.maybeOwnedBuffer) {
      tracker.trackFieldWithSize("buffer", buffer->view.size());
    }
  }
}

// =======================================================================================
// ReadableStreamNativeSource

ReadableStreamNativeSource::ReadableStreamNativeSource(
    IoContext& ioContext, kj::Own<ReadableStreamSource> source)
    : state(Active{.source = ioContext.addObject(kj::mv(source))}) {}

jsg::Promise<void> ReadableStreamNativeSource::pull(
    jsg::Lock& js, jsg::JsObject controller, jsg::Ref<AbortSignal> signal) {
  // Defensive: the TypeScript conduit guarantees at most one pull in flight (standard
  // pulling/pullAgain serialization); a concurrent pull is a contract violation and must
  // fail loudly.
  JSG_REQUIRE(!pullInFlight, TypeError, "pull() is already in flight.");

  KJ_IF_SOME(active, state) {
    // The read mode follows the consumer: a non-null byobRequest means a BYOB read is at
    // the head of the conduit's request queue; null means a default read is.
    KJ_IF_SOME(byobRequest, controller.get(js, "byobRequest"_kj).tryCast<jsg::JsObject>()) {
      return pullByob(js, controller, byobRequest, kj::mv(signal), active);
    }
    return pullDefault(js, controller, kj::mv(signal), active);
  }

  // The source already reached EOF or was canceled. The conduit stops pulling once it
  // observes close/error, so this is unreachable in practice; tolerate it defensively.
  return js.resolvedPromise();
}

jsg::Promise<void> ReadableStreamNativeSource::pullDefault(
    jsg::Lock& js, jsg::JsObject controller, jsg::Ref<AbortSignal> signal, Active& active) {
  // Bytes retained from an abandoned pull are redelivered first, before any new data.
  if (!stash.empty()) {
    // Contract: check the signal synchronously immediately before delivering. If the
    // consumer has already abandoned this read too, keep the bytes for the next pull.
    if (!signal->getAborted(js)) {
      auto chunk = jsg::JsUint8Array::create(js, stash.asPtr());
      stash.clear();
      invokeMethod(js, controller, "enqueue"_kj, chunk);
    }
    return js.resolvedPromise();
  }

  ensureScratch(kScratchSize);

  auto& ioContext = IoContext::current();
  pullInFlight = true;
  return ioContext.awaitIo(js, active.source->tryRead(scratch.begin(), 1, scratch.size()))
      .then(js,
          [self = JSG_THIS, controller = controller.addRef(js), signal = kj::mv(signal)](
              jsg::Lock& js, size_t amount) mutable {
    self->pullInFlight = false;
    if (self->pendingCancel) {
      // cancel() arrived while the read was in flight: complete the deferred teardown and
      // discard the bytes (post-cancel stragglers are discarded, per the contract).
      self->pendingCancel = false;
      self->state = kj::none;
      return;
    }
    if (signal->getAborted(js)) {
      // The consumer abandoned the read while it was in flight (e.g. releaseLock()).
      // Retain the bytes for redelivery on the next pull; the conduit treats this pull's
      // settlement as inert.
      self->stash.addAll(self->scratch.first(amount));
      return;
    }
    if (amount == 0) {
      // EOF. Settle our own state before notifying, in case the close call re-enters.
      self->state = kj::none;
      invokeMethod(js, controller.getHandle(js), "close"_kj);
      return;
    }
    invokeMethod(js, controller.getHandle(js), "enqueue"_kj,
        jsg::JsUint8Array::create(js, self->scratch.first(amount)));
  },
          [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
    // The read failed; the source is no longer usable. Rethrow to reject the pull promise
    // -- the conduit errors the stream (or ignores the settlement if this pull was already
    // abandoned).
    self->pullInFlight = false;
    self->pendingCancel = false;
    self->state = kj::none;
    js.throwException(kj::mv(exception));
  });
}

jsg::Promise<void> ReadableStreamNativeSource::pullByob(jsg::Lock& js,
    jsg::JsObject controller,
    jsg::JsObject byobRequest,
    jsg::Ref<AbortSignal> signal,
    Active& active) {
  // Extract the request's view and minimum. The view is a Uint8Array over the remaining
  // unfilled region of the consumer's (transferred, conduit-owned) buffer; atLeast is the
  // minimum number of bytes that must be delivered to satisfy the read (>= 1). These come
  // from the module-owned conduit facade, not from user code, so shape violations indicate
  // internal errors and fail loudly.
  auto view = KJ_REQUIRE_NONNULL(
      byobRequest.get(js, "view"_kj).tryCast<jsg::JsUint8Array>(), "the BYOB request has no view");
  size_t atLeast = 1;
  KJ_IF_SOME(num, byobRequest.get(js, "atLeast"_kj).tryCast<jsg::JsNumber>()) {
    KJ_IF_SOME(value, num.value(js)) {
      atLeast = kj::max(static_cast<size_t>(1), static_cast<size_t>(value));
    }
  }
  auto dest = view.asArrayPtr();
  JSG_REQUIRE(dest.size() > 0, TypeError, "The BYOB request view is empty or detached.");
  JSG_REQUIRE(
      atLeast <= dest.size(), TypeError, "The BYOB request's minimum exceeds its view size.");

  // Bytes retained from an abandoned pull are redelivered first. If they alone satisfy the
  // read's minimum, no I/O is needed at all.
  if (stash.size() >= atLeast) {
    // Contract: check the signal synchronously immediately before delivering.
    if (!signal->getAborted(js)) {
      size_t amount = kj::min(stash.size(), dest.size());
      dest.write(stash.asPtr().first(amount));
      consumeStash(amount);
      invokeMethod(js, byobRequest, "respond"_kj, js.num(static_cast<double>(amount)));
    }
    return js.resolvedPromise();
  }

  // Top up with a fresh read. Any retained bytes count toward the minimum. The underlying
  // source performs its own internal accumulation toward minBytes (KJ tryRead semantics),
  // so this single read is the source's complete answer for the read: delivering fewer
  // than the minimum in total implicitly signals EOF (the conduit commits the partial fill
  // fused as {done: true, value: partialView} and closes the stream).
  size_t stashed = stash.size();
  size_t minBytes = atLeast - stashed;
  ensureScratch(kj::max(kScratchSize, minBytes));
  size_t maxBytes = kj::min(dest.size() - stashed, scratch.size());

  auto& ioContext = IoContext::current();
  pullInFlight = true;
  return ioContext.awaitIo(js, active.source->tryRead(scratch.begin(), minBytes, maxBytes))
      .then(js,
          [self = JSG_THIS, controller = controller.addRef(js),
              byobRequest = byobRequest.addRef(js), view = view.addRef(js), signal = kj::mv(signal),
              minBytes](jsg::Lock& js, size_t amount) mutable {
    self->pullInFlight = false;
    if (self->pendingCancel) {
      // cancel() arrived while the read was in flight: complete the deferred teardown and
      // discard the bytes.
      self->pendingCancel = false;
      self->state = kj::none;
      return;
    }
    if (signal->getAborted(js)) {
      // The consumer abandoned the read; retain the bytes (after any previously retained
      // ones, preserving order) for redelivery on the next pull.
      self->stash.addAll(self->scratch.first(amount));
      return;
    }
    size_t stashed = self->stash.size();
    size_t total = stashed + amount;
    if (total == 0) {
      // EOF with nothing to deliver: respond(0) is forbidden; close() is the EOF signal.
      self->state = kj::none;
      invokeMethod(js, controller.getHandle(js), "close"_kj);
      return;
    }
    auto dest = view.getHandle(js).asArrayPtr();
    if (dest.size() < total) {
      // The view was detached while the read was in flight. Treat the read as abandoned:
      // retain the bytes for the next consumer.
      self->stash.addAll(self->scratch.first(amount));
      return;
    }
    if (stashed > 0) {
      // write() advances dest past the copied prefix, so the fresh bytes below land
      // immediately after the redelivered stash.
      dest.write(self->stash.asPtr());
      self->stash.clear();
    }
    if (amount > 0) {
      dest.write(self->scratch.first(amount));
    }
    bool eof = amount < minBytes;
    if (eof) {
      // The source delivered fewer than minBytes: EOF (KJ semantics). Settle our own
      // state before making the JS calls below.
      self->state = kj::none;
    }
    invokeMethod(js, byobRequest.getHandle(js), "respond"_kj, js.num(static_cast<double>(total)));
    if (eof) {
      // Fused close-commit: deliver the partial bytes, then explicitly signal EOF in the
      // same pull turn. (The under-delivered respond() above already implies closure to
      // the conduit, which tolerates this close as a no-op; the explicit close keeps the
      // EOF signal unambiguous rather than relying on that inference.)
      invokeMethod(js, controller.getHandle(js), "close"_kj);
    }
  },
          [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
    self->pullInFlight = false;
    self->pendingCancel = false;
    self->state = kj::none;
    js.throwException(kj::mv(exception));
  });
}

void ReadableStreamNativeSource::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) {
  KJ_IF_SOME(active, state) {
    kj::Exception exception = [&]() {
      KJ_IF_SOME(r, reason) {
        return js.exceptionToKj(r);
      }
      return JSG_KJ_EXCEPTION(DISCONNECTED, Error, "This ReadableStream was cancelled.");
    }();
    active.source->cancel(kj::mv(exception));
    if (pullInFlight) {
      // A read is in flight; releasing the source now would destroy it out from under its
      // own read. Defer the release to the pull's settlement.
      pendingCancel = true;
    } else {
      state = kj::none;
    }
  }
  // Canceling an already-done source is a no-op.
}

kj::Array<jsg::Ref<ReadableStreamNativeSource>> ReadableStreamNativeSource::tee(jsg::Lock& js) {
  auto& active = JSG_REQUIRE_NONNULL(
      state, TypeError, "This ReadableStream source has already been consumed.");

  // An abandoned pull's read may still be in flight (the reader was released mid-pull).
  // Bytes that read produces after the split would land in this (dead) source's stash,
  // invisible to both branches -- silent loss. Refuse loudly instead. Revisit per the
  // design doc's open question D if this proves reachable in practice.
  JSG_REQUIRE(
      !pullInFlight, TypeError, "Cannot tee this ReadableStream while a read is in flight.");

  auto& ioContext = IoContext::current();
  auto limit = ioContext.getLimitEnforcer().getBufferingLimit();

  auto branches = [&]() -> ReadableStreamSource::Tee {
    KJ_IF_SOME(tee, active.source->tryTee(limit)) {
      // The underlying source has an optimized tee implementation.
      return kj::mv(tee);
    }
    // Generic fallback (mirroring the legacy internal controller's tee): pull the source
    // out of its IoOwn and run it through kj::newTee, with each branch wrapped back into
    // a ReadableStreamSource. streams::wrapTeeBranch applies the same tee error
    // translation the legacy path uses.
    auto tee = kj::newTee(
        kj::heap<TeeInputAdapter>(kj::Own<ReadableStreamSource>(kj::mv(active.source))), limit);
    return ReadableStreamSource::Tee{
      .branches = {kj::heap<TeeBranchSource>(streams::wrapTeeBranch(kj::mv(tee.branches[0]))),
        kj::heap<TeeBranchSource>(streams::wrapTeeBranch(kj::mv(tee.branches[1])))},
    };
  }();

  auto branch1 = js.alloc<ReadableStreamNativeSource>(ioContext, kj::mv(branches.branches[0]));
  auto branch2 = js.alloc<ReadableStreamNativeSource>(ioContext, kj::mv(branches.branches[1]));

  // Any bytes retained from an abandoned pull were already consumed from the underlying
  // source, so the branch sources will never produce them: seed a copy into BOTH branches
  // (they are stream content that precedes everything upstream), and let each branch
  // deliver them before any new data.
  if (!stash.empty()) {
    branch1->stash.addAll(stash.asPtr());
    branch2->stash.addAll(stash.asPtr());
    stash.clear();
  }

  // This source is consumed: per the contract it must never be pulled again.
  state = kj::none;

  return kj::arr(kj::mv(branch1), kj::mv(branch2));
}

jsg::Optional<jsg::JsBigInt> ReadableStreamNativeSource::getExpectedLength(jsg::Lock& js) {
  KJ_IF_SOME(active, state) {
    KJ_IF_SOME(length, active.source->tryGetLength(StreamEncoding::IDENTITY)) {
      // Bytes retained in the stash (from an abandoned pull, or inherited from a tee
      // parent) were already consumed from the underlying source but not yet delivered,
      // so they count toward the total this source will produce. Getting this right
      // matters for tee branches: the conduit reads expectedLength at construction and
      // enforces it as an exact total.
      return js.bigInt(length + stash.size());
    }
  }
  return kj::none;
}

ReadableStreamNativeSource::Released ReadableStreamNativeSource::releaseForPump(jsg::Lock& js) {
  KJ_IF_SOME(active, state) {
    // Extraction requires an undisturbed stream, and any pull implies a read (which
    // disturbs), so no read can be in flight here.
    KJ_ASSERT(!pullInFlight);
    Released released{
      .source = kj::Own<ReadableStreamSource>(kj::mv(active.source)),
      .prefix = stash.releaseAsArray(),
    };
    state = kj::none;
    return released;
  }
  // The source already completed (EOF, or cancel released it); the pump simply finishes.
  return Released{.source = kj::heap<NullSource>(), .prefix = nullptr};
}

void ReadableStreamNativeSource::ensureScratch(size_t capacity) {
  if (scratch.size() < capacity) {
    scratch = kj::heapArray<kj::byte>(capacity);
  }
}

void ReadableStreamNativeSource::consumeStash(size_t bytes) {
  KJ_DASSERT(bytes <= stash.size());
  if (bytes >= stash.size()) {
    stash.clear();
  } else {
    // Partial consumption (rare: a BYOB view smaller than the current stash). Rebuild
    // from the remainder rather than shifting in place: ArrayPtr::copyFrom() forbids
    // overlapping ranges.
    kj::Vector<kj::byte> remainder;
    remainder.addAll(stash.asPtr().slice(bytes, stash.size()));
    stash = kj::mv(remainder);
  }
}

void ReadableStreamNativeSource::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackFieldWithSize("scratch", scratch.size());
  tracker.trackFieldWithSize("stash", stash.size());
}

}  // namespace workerd::api
