// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/blob.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/api/js-streams-bridge.h>
#include <workerd/api/js-writable-stream.h>
#include <workerd/api/streams/common.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>
#include <workerd/io/features.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/autogate.h>
#include <workerd/util/strong-bool.h>

#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::api {

namespace {

// The exact error message produced when attempting to consume an already-consumed body. This
// text is user-visible: it matches the message historically produced by the
// Body mixin (see http.c++) and must not change.
constexpr kj::StringPtr kBodyUsedError =
    "Body has already been used. It can only be used once. Use tee() first if you need to "
    "read it twice."_kj;

bool getReadableStreamIsDisturbed(jsg::Lock& js, jsg::JsObject obj) {
  return webstreams::dispatchCall(js, "getReadableStreamIsDisturbed", obj).isTrue();
}

// Recognizes the TypeScript implementation's ReadableStream (including subclasses) by the own
// api-symbol brand its constructor stamps on every instance. Runs no JavaScript -- recognition
// must work during RPC deserialization, inside V8's no-JS-execution scope -- so it probes for
// the brand rather than asking the TS implementation. Proxies answer false: an own-property
// probe on a proxy would invoke its traps, and the TS-side #-brand does not tunnel through
// proxies either.
//
// This is recognition, not authentication. An api symbol stays visible to reflection
// (Object.getOwnPropertySymbols) on every instance, so user code can read it off a real stream
// and stamp it on an object of its own: a true answer means "route this as a TypeScript
// stream", not "this is one". Genuine instances cannot lose the brand, which is stamped
// non-writable and non-configurable. What protects the consumers is that recognition grants
// nothing on its own -- every operation reached afterwards goes through the TS internal
// algorithms, whose real #-brand checks throw a TypeError on an impostor, and on both RPC
// serialize arms that rejection lands before anything is written to the wire.
//
// An impostor also cannot arrive over the wire, because V8's value serializer emits only own
// enumerable string keys (dropping any symbol-keyed brand) and will not serialize an
// unrecognized class instance at all. Every branded object reachable inside the no-JS
// deserialization scope is therefore one this runtime just built -- the premise the state
// probes in isDisturbed() and isLocked() rest on.
bool isTypeScriptReadableStream(jsg::Lock& js, jsg::JsObject obj) {
  if (v8::Local<v8::Value>(obj)->IsProxy()) {
    return false;
  }
  return obj.has(js, js.symbolInternal("kReadableStreamBrand"), jsg::JsObject::HasOption::OWN);
}

bool getReadableStreamIsLocked(jsg::Lock& js, jsg::JsObject obj) {
  return webstreams::dispatchCall(js, "isReadableStreamLocked", obj).isTrue();
}

jsg::Promise<void> readableStreamCancel(
    jsg::Lock& js, jsg::JsObject obj, jsg::Optional<jsg::JsValue>& reason) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "readableStreamCancel", obj, reason.orDefault(js.undefined()));
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toVoidPromise(promise);
}

void setReadableStreamPendingClosure(jsg::Lock& js, jsg::JsObject obj) {
  // The result is undefined/ignored
  auto res KJ_UNUSED = webstreams::dispatchCall(js, "setReadableStreamPendingClosure", obj);
}

jsg::Promise<void> getReadableStreamOnEof(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = webstreams::dispatchCall(js, "getReadableStreamOnEof", obj);
  // The result must be a promise
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toVoidPromise(promise);
}

kj::Maybe<uint64_t> getReadableStreamExpectedLength(jsg::Lock& js, jsg::JsObject obj) {
  jsg::JsValue result = webstreams::dispatchCall(js, "getReadableStreamExpectedLength", obj);
  KJ_IF_SOME(bi, JSG_TRY_CAST(result, JsBigInt)) {
    KJ_IF_SOME(len, bi.tryToUint64(js)) {
      return len;
    }
  }
  return kj::none;
}

jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> getReadableStreamArrayBuffer(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "consumeReadableStreamAsArrayBuffer", obj, js.bigInt(limit));
  // The result must be a promise for an Arraybuffer
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_ARRAYBUFFER(value));
    return ab.addRef(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsUint8Array>> getReadableStreamBytes(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "consumeReadableStreamAsUint8Array", obj, js.bigInt(limit));
  // The result must be a promise for an Uint8Array
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_UINT8ARRAY(value));
    return ab.addRef(js);
  });
}

jsg::Promise<kj::String> getReadableStreamText(jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "consumeReadableStreamAsText", obj, js.bigInt(limit));
  // The result must be a promise for a String
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toPromise(promise).then(js, [](jsg::Lock& js, jsg::Value ref) {
    auto value = jsg::JsValue(ref.getHandle(js));
    return value.toString(js);
  });
}

jsg::Promise<jsg::JsRef<jsg::JsValue>> getReadableStreamJson(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "consumeReadableStreamAsJSON", obj, js.bigInt(limit));
  // The result must be a promise for a JS value
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toPromise(promise).then(
      js, [](jsg::Lock& js, jsg::Value ref) { return jsg::JsValue(ref.getHandle(js)).addRef(js); });
}

jsg::Promise<jsg::Ref<Blob>> getReadableStreamBlob(
    jsg::Lock& js, jsg::JsObject obj, uint64_t limit, kj::String contentType) {
  jsg::JsValue result =
      webstreams::dispatchCall(js, "consumeReadableStreamAsArrayBuffer", obj, js.bigInt(limit));
  // The result must be a promise for an Arraybuffer
  jsg::JsPromise promise = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(result));
  return js.toPromise(promise).then(
      js, [contentType = kj::mv(contentType)](jsg::Lock& js, jsg::Value ref) mutable {
    auto value = jsg::JsValue(ref.getHandle(js));
    // If it throws, it manifests as an internal error. That's intended.
    auto ab = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_ARRAYBUFFER(value));
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

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    // kj::newTee's branches serve already-buffered data synchronously.
    return inner->tryReadSync(buffer, minBytes);
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

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    // Always at EOF, which is a valid synchronous answer.
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

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_REQUIRE(minBytes <= buffer.size());
    KJ_IF_SOME(prefix, maybePrefix) {
      if (prefix.view != nullptr) {
        size_t amount = kj::min(buffer.size(), prefix.view.size());
        if (amount >= minBytes) {
          // The prefix alone satisfies the read: serve it synchronously. (buffer is a
          // by-value view; write() advancing it does not affect the caller.)
          buffer.write(prefix.view.first(amount));
          prefix.view = prefix.view.slice(amount);
          if (prefix.view == nullptr) {
            maybePrefix = kj::none;
          }
          return amount;
        }
        // The prefix alone cannot satisfy minBytes, so the remainder must come from the
        // inner source. Probe the inner source into the tail of the buffer FIRST: a
        // declined synchronous read must have no side effects, so the prefix may only be
        // consumed once the combined read is known to complete. Note that amount <
        // minBytes <= buffer.size() implies amount == prefix.view.size(), so completing
        // consumes the entire prefix.
        KJ_IF_SOME(n, inner->tryReadSync(buffer.slice(amount, buffer.size()), minBytes - amount)) {
          buffer.write(prefix.view.first(amount));
          maybePrefix = kj::none;
          return amount + n;
        }
        return kj::none;
      }
      // An empty prefix behaves identically to no prefix. Leave the normalization to the
      // asynchronous paths: a declined synchronous read must have no side effects.
    }
    return inner->tryReadSync(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(kj::Ptr<WritableStreamSink> output, bool end) override {
    // Write the (kj-heap) prefix bytes in the pre-proxy phase, then delegate to the inner
    // source's own pump so its deferred-proxy classification passes through: once the
    // inner pump's outer promise resolves, this pump enters deferred proxying itself and
    // rides the inner proxy task.
    KJ_IF_SOME(prefix, maybePrefix) {
      if (prefix.view != nullptr) {
        co_await output->write(prefix.view);
      }
      maybePrefix = kj::none;
    }
    auto deferred = co_await inner->pumpTo(kj::mv(output), end);
    KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING;
    co_await deferred.proxyTask;
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

// Writes one drained batch to the sink and, when this is the final batch, ends it.
kj::Promise<void> queuedWriteStep(
    kj::Ptr<WritableStreamSink> sink, kj::Array<kj::Array<const kj::byte>> pieces, EndStream end) {
  if (pieces.size() > 0) {
    auto ptrs = KJ_MAP(piece, pieces) -> kj::ArrayPtr<const kj::byte> { return piece.asPtr(); };
    co_await sink->write(ptrs.asPtr());
  }
  if (end) {
    co_await sink->end();
  }
}

struct QueuedPumpState {
  jsg::JsRef<jsg::JsObject> reader;
  IoOwn<WritableStreamSink> sink;
};

// One iteration of the queued-backend pump: collect everything the draining reader has
// buffered (one isolate-lock trip per batch), copy it to KJ-owned memory, perform a
// vectored write, and recurse until done. All JS state (the reader) travels through the
// jsg promise chain.
jsg::Promise<void> queuedPumpStep(jsg::Lock& js, kj::Rc<QueuedPumpState> state, EndStream end) {
  auto& context = IoContext::current();
  auto readResult = webstreams::invokeMethod(js, state->reader.getHandle(js), "read"_kj);
  auto readPromise = JSG_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(readResult), TypeError,
      "ReadableStreamDrainingReader.read() did not return a promise.");
  return js.toPromise(readPromise)
      .then(js,
          context.addFunctor([state = kj::mv(state), end](jsg::Lock& js, IoContext& context,
                                 jsg::Value value) mutable -> jsg::Promise<void> {
    auto result = JSG_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(jsg::JsValue(value.getHandle(js))),
        TypeError, "ReadableStreamDrainingReader.read() promise did not resolve to an object.");
    bool done = result.get(js, "done"_kj).isTrue();
    auto chunks = JSG_REQUIRE_NONNULL(JSG_TRY_CAST(result.get(js, "chunks"_kj), JsArray), TypeError,
        "ReadableStreamDrainingReader.read() promise did not resolve to an object "
        "with a chunks array.");

    // Only ArrayBuffer/ArrayBufferView chunks are usable as bytes. Everything else
    // (including strings) rejects, matching the legacy pump's historical behavior and
    // error text exactly.
    auto pieces = kj::Vector<kj::Array<const kj::byte>>(chunks.size());
    for (uint32_t i = 0; i < chunks.size(); i++) {
      auto chunk = chunks.get(js, i);
      KJ_IF_SOME(view, JSG_TRY_CAST(chunk, JsArrayBufferView)) {
        pieces.add(jsg::JsBufferSource(view).copy());
      } else KJ_IF_SOME(buffer, JSG_TRY_CAST_ARRAYBUFFER(chunk)) {
        pieces.add(jsg::JsBufferSource(buffer).copy());
      } else {
        JSG_FAIL_REQUIRE(TypeError, "This ReadableStream did not return bytes.");
      }
    }

    auto writeStep =
        queuedWriteStep(state->sink->getPtr(), pieces.releaseAsArray(), done ? end : EndStream::NO);
    return context.awaitIo(js, kj::mv(writeStep),
        context.addFunctor([state = kj::mv(state), end, done](jsg::Lock& js, IoContext&) mutable {
      return done ? js.resolvedPromise() : queuedPumpStep(js, kj::mv(state), end);
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

  auto state = kj::rc<QueuedPumpState>(
      QueuedPumpState{.reader = reader.addRef(js), .sink = context.addObject(kj::mv(sink))});

  auto loop = queuedPumpStep(js, state.addRef(), end)
                  .catch_(js,
                      context.addFunctor([state = kj::mv(state)](jsg::Lock& js, IoContext&,
                                             jsg::Value exception) mutable {
    // The pump failed: abort the sink, cancel the reader, then propagate the failure.
    auto reason = jsg::JsValue(exception.getHandle(js));
    state->sink->abort(js.exceptionToKj(reason));

    auto cancelResult =
        webstreams::invokeMethod(js, state->reader.getHandle(js), "cancel"_kj, reason);
    KJ_IF_SOME(promise, JSG_TRY_CAST_PROMISE(cancelResult)) {
      promise.markAsHandled(js);
    }
    js.throwException(kj::mv(exception));
  }));
  return addNoopDeferredProxy(context.awaitJs(js, kj::mv(loop)));
}
}  // namespace

// The -Wdangling-field warnings below are false positives. view captures the heap buffer pointer
// managed by data, not the address of the data parameter itself. Moving data into owned transfers
// ownership of that heap buffer without changing its address, so view remains valid.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdangling-field"
JsReadableStream::Buffer::Buffer(kj::Array<const kj::byte> data)
    : owned(kj::mv(data)),
      view(owned) {}
#pragma clang diagnostic pop

// Blob contents live in a V8 ArrayBuffer.  The streams built from this Buffer are read on the
// kj event loop, where the isolate's MPK-protected sandbox pages are unreadable, so take a
// kj-heap copy.  It is shared by every stream derived from this Buffer -- rewinds and tee
// branches alike.
JsReadableStream::Buffer::Buffer(jsg::Ref<Blob> data): Buffer(kj::heapArray(data->getData())) {}

namespace {

// Constructs the stream backing a JsReadableStream over the given native data source. This
// is the compatibility-flag dispatch point shared by create() and bufferBackedImpl(): when
// the typescript_implemented_streams compat flag is enabled, the source is wrapped in a
// ReadableStreamNativeSource -- whose instances are born carrying the kNativeSource marker
// (JSG_PRIVATE_SYMBOL) that the TypeScript ReadableStream constructor detects -- and the
// TypeScript stream is constructed over it via the constructor exposed through the
// bootstrap's cpp_exports module. Otherwise the legacy C++ ReadableStream is used.
JsReadableStream::StreamImpl newBackingStream(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source) {
  if (FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
    auto sourceObj = jsg::JsValue(
        handler.wrap(js, js.alloc<ReadableStreamNativeSource>(ioContext, kj::mv(source))));
    auto constructor = webstreams::getCppExport(js, "ReadableStream");
    return JsReadableStream::StreamImpl(constructor.newInstance(js, sourceObj).addRef(js));
  }
  return JsReadableStream::StreamImpl(js.alloc<ReadableStream>(ioContext, kj::mv(source)));
}

}  // namespace

JsReadableStream::Impl JsReadableStream::bufferBackedImpl(jsg::Lock& js, kj::Rc<Buffer> buffer) {
  // Use newMemorySource() rather than newSystemStream() wrapping a memory input stream:
  // it reads the Buffer's bytes in place, so every stream derived from this Buffer -- rewinds
  // and tee branches alike -- shares the one allocation.
  auto view = buffer->view;
  auto source = newMemorySource(view, buffer.addRef().toOwn());
  return Impl{
    .stream = newBackingStream(js, IoContext::current(), kj::mv(source)),
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
    : JsReadableStream(js, webstreams::stringToBytes(kj::mv(data))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::JsRef<jsg::JsBufferSource> view)
    : JsReadableStream(js, view.getHandle(js).copy()) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<Blob> blob)
    : impl(bufferBackedImpl(js, kj::rc<Buffer>(kj::mv(blob)))) {}

JsReadableStream::JsReadableStream(jsg::Lock& js, jsg::Ref<api::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream::JsReadableStream(
    jsg::Lock& js, jsg::Ref<api::url::URLSearchParams> urlSearchParams)
    : JsReadableStream(js, urlSearchParams->toString()) {}

JsReadableStream JsReadableStream::create(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source) {
  return JsReadableStream(Impl{.stream = newBackingStream(js, ioContext, kj::mv(source))});
}

JsReadableStream JsReadableStream::from(jsg::Lock& js, jsg::AsyncGenerator<jsg::Value> generator) {
  if (!FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    return JsReadableStream(ReadableStream::from(js, kj::mv(generator)));
  }

  // TypeScript arm: construct a TypeScript ReadableStream over a C++-built JS underlying
  // source whose pull/cancel hooks drive the generator, following the same algorithm as
  // the legacy ReadableStream::from(): one generator.next() per pull, promise-typed
  // values awaited before enqueue (jsg's sync-iterator fallback does not apply
  // async-from-sync value wrapping itself, so the pull compensates -- for async
  // iterators this awaiting is technically beyond what the spec asks, matching the
  // legacy arm), close on completion, cancel forwarding to the generator's return().
  // Pull rejections (generator.next() failure or a rejected value promise) propagate to
  // the standard machinery, which errors the stream with the same reason.

  auto pull = js.wrapPromiseReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((generator = generator.addRef(js)), (generator),
          (jsg::Lock & js, const v8::FunctionCallbackInfo<v8::Value>& info) mutable {
            auto controller =
                jsg::JsRef(js, KJ_ASSERT_NONNULL(jsg::JsValue(info[0]).tryCast<jsg::JsObject>()));
            return generator.next(js).then(js,
                JSG_VISITABLE_LAMBDA((controller = kj::mv(controller)), (controller),
                    (jsg::Lock & js,
                        kj::Maybe<jsg::Value> value) mutable->jsg::Promise<jsg::Value> {
                      KJ_IF_SOME(v, value) {
                      auto handle = v.getHandle(js);
                      if (handle->IsPromise()) {
                      return js.toPromise(handle.As<v8::Promise>())
                          .then(js,
                              JSG_VISITABLE_LAMBDA((controller = kj::mv(controller)), (controller),
                                  (jsg::Lock & js, jsg::Value val) mutable {
                                    webstreams::dispatchCall(js, "readableControllerEnqueue",
                                        jsg::JsValue(controller.getHandle(js)),
                                        jsg::JsValue(val.getHandle(js)));
                                    return js.v8Ref<v8::Value>(js.v8Undefined());
                                  }));
                      }
                      webstreams::dispatchCall(js, "readableControllerEnqueue",
                          jsg::JsValue(controller.getHandle(js)), jsg::JsValue(handle));
                      } else {
                      webstreams::dispatchCall(
                          js, "readableControllerClose", jsg::JsValue(controller.getHandle(js)));
                      }
                      return js.resolvedPromise(js.v8Ref<v8::Value>(js.v8Undefined()));
                    }));
          }));

  auto cancel = js.wrapPromiseReturningFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((generator = generator.addRef(js)), (generator),
          (jsg::Lock & js, const v8::FunctionCallbackInfo<v8::Value>& info) mutable {
            return generator.return_(js, js.v8Ref<v8::Value>(v8::Local<v8::Value>(info[0])))
                .then(js, [](jsg::Lock& js, kj::Maybe<jsg::Value>) {
              // The generator might produce a value on return and might even want to continue,
              // but the stream has been canceled at this point, so we stop here.
              return js.v8Ref<v8::Value>(js.v8Undefined());
            });
          }));

  auto sourceObj = js.obj();
  sourceObj.set(js, "pull"_kj, jsg::JsValue(pull));
  sourceObj.set(js, "cancel"_kj, jsg::JsValue(cancel));
  // Demand-driven pulls only, per the spec's ReadableStreamFromIterable (and the legacy
  // arm's StreamQueuingStrategy{.highWaterMark = 0}).
  auto strategyObj = js.obj();
  strategyObj.set(js, "highWaterMark"_kj, jsg::JsValue(js.num(0)));

  auto constructor = webstreams::getCppExport(js, "ReadableStream");
  return JsReadableStream(js,
      constructor.newInstance(js, jsg::JsValue(sourceObj), jsg::JsValue(strategyObj)).addRef(js));
}

kj::Maybe<JsReadableStream> JsReadableStream::tryUnwrapTs(
    jsg::Lock& js, v8::Local<v8::Value> handle) {
  // Without the flag there is no TypeScript implementation (and no bootstrap export to
  // ask), so nothing can match. This also keeps the flag-off unwrap path allocation- and
  // JS-call-free.
  if (!FeatureFlags::get(js).getTypeScriptImplementedStreams()) {
    return kj::none;
  }
  KJ_IF_SOME(obj, JSG_TRY_CAST_OBJECT(jsg::JsValue(handle))) {
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
        if (js.isJavascriptExecutionDisallowed()) {
          // Asking the TypeScript side would execute JS, which is forbidden here. The only
          // no-JS scope in which TypeScript-backed streams are reachable is RPC
          // deserialization (V8 forbids JS for the whole value-graph read; the legacy
          // queue's drain scope never touches TS-backed streams), and every TS stream
          // reachable there is hydration-fresh: it was just constructed by
          // RpcDeserializerExternalHandler::prepare(), user code has never had it, and no
          // transition mechanism exists inside the scope. Fresh streams are undisturbed by
          // construction.
          return false;
        }
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
        if (js.isJavascriptExecutionDisallowed()) {
          // Hydration-fresh by the same reasoning as isDisturbed() above; fresh streams are
          // unlocked by construction.
          return false;
        }
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
        // Reject-if-locked precondition (parity with ReadableStream::cancel, including the
        // rejection text), then the lock-blind internal cancel; the check-then-act pair is
        // atomic under the isolate lock. Composed here from cppExports operations rather
        // than invoking the prototype method, which user code can patch. forceCancel() is
        // the variant that skips the lock check.
        auto handle = obj.getHandle(js);
        if (getReadableStreamIsLocked(js, handle)) {
          return js.rejectedPromise<void>(
              js.typeError("This ReadableStream is currently locked to a reader."_kj));
        }
        return readableStreamCancel(js, handle, reason);
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
        auto handle = obj.getHandle(js);
        if (encoding == StreamEncoding::IDENTITY) {
          // The controller-level expected length is an identity byte count (declared by
          // the source or the expectedLength extension), for both backends.
          return getReadableStreamExpectedLength(js, handle);
        }
        // Non-identity encodings: only a native underlying source can answer (parity with
        // the legacy internal controller, which forwards the encoding to its source).
        // Queued streams answer kj::none -- their identity-byte expectedLength is never a
        // valid encoded length. (The legacy JS controller ignores the encoding and reports
        // its identity expectedLength anyway; that is a wire-protocol footgun -- a wrong
        // Content-Length for an encoded body -- that this arm deliberately does not
        // reproduce.)
        auto sourceValue = webstreams::dispatchCall(js, "getReadableStreamNativeSource", handle);
        if (sourceValue.isUndefined()) {
          return kj::none;
        }
        auto& handler =
            KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
        auto source = KJ_REQUIRE_NONNULL(handler.tryUnwrap(js, sourceValue),
            "getReadableStreamNativeSource did not return a ReadableStreamNativeSource");
        return source->tryGetLength(encoding);
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

StreamEncoding JsReadableStream::getPreferredEncoding(jsg::Lock& js) {
  KJ_IF_SOME(i, impl) {
    KJ_SWITCH_ONEOF(i.stream) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        return stream->getController().getPreferredEncoding();
      }
      KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
        // Only a native underlying source can prefer a non-identity encoding; queued
        // (JS-sourced) streams produce identity bytes.
        auto sourceValue =
            webstreams::dispatchCall(js, "getReadableStreamNativeSource", obj.getHandle(js));
        if (sourceValue.isUndefined()) {
          return StreamEncoding::IDENTITY;
        }
        auto& handler =
            KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
        auto source = KJ_REQUIRE_NONNULL(handler.tryUnwrap(js, sourceValue),
            "getReadableStreamNativeSource did not return a ReadableStreamNativeSource");
        return source->getPreferredEncoding();
      }
    }
    KJ_UNREACHABLE;
  }
  return StreamEncoding::IDENTITY;
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
      auto extractSymbol = js.symbolInternal("kExtractNativeSource");
      if (handle.has(js, extractSymbol, jsg::JsObject::HasOption::OWN)) {
        // Native-backed: extract the underlying source and pump entirely at the C++
        // layer, preserving each source's own deferred-proxy behavior. The extractor is
        // atomic (validate, detach, lock + disturb) and returns the source wrapper.
        auto extractor = JSG_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(handle.get(js, extractSymbol)),
            TypeError, "ReadableStream kExtractNativeSource property is not a function");
        auto sourceObj = extractor.call(js, handle);
        auto& handler =
            KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<ReadableStreamNativeSource>>());
        auto source = JSG_REQUIRE_NONNULL(handler.tryUnwrap(js, sourceObj), TypeError,
            "The kExtractNativeSource extractor did not return a ReadableStreamNativeSource");
        return pumpExtractedSource(source->releaseForPump(js), kj::mv(sink), end == EndStream::YES);
      }

      // Queued-backed (JS underlying source): the JS conduit stays in the data path, so
      // drive the internal DrainingReader batch by batch under the isolate lock.
      auto reader = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(
          webstreams::dispatchCall(js, "acquireReadableStreamDrainingReader", handle)));
      return pumpQueuedTsStream(js, context, reader, kj::mv(sink), end);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> JsReadableStream::pipeTo(
    jsg::Lock& js, JsWritableStream& destination, PipeToOptions options) {
  auto& i = KJ_ASSERT_NONNULL(impl, "pipeTo() called on a null JsReadableStream");

  KJ_REQUIRE(!destination.isNull(), "pipeTo() called with a null destination JsWritableStream");

  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(source, jsg::Ref<ReadableStream>) {
      // Both ends must be legacy C++: the TS implementation, when enabled, constructs all
      // streams as TS-backed. Mixed backends are never valid and indicate a migration bug.
      auto dest = KJ_ASSERT_NONNULL(destination.tryGetLegacy(js),
          "pipeTo: legacy ReadableStream paired with a non-legacy WritableStream; "
          "mixed backends are not supported");
      return source->pipeTo(js, dest.addRef(), kj::mv(options))
          .then(js, [source = source.addRef(), dest = kj::mv(dest)](jsg::Lock& js) {});
    }
    KJ_CASE_ONEOF(source, jsg::JsRef<jsg::JsObject>) {
      auto dest = KJ_ASSERT_NONNULL(destination.tryGetTs(js),
          "pipeTo: TS-backed ReadableStream paired with a non-TS WritableStream; "
          "mixed backends are not supported");

      // Forward the options as a StreamPipeOptions dictionary. Absent booleans are simply
      // omitted (the TS pump coerces missing members to false). The internal pipeThrough
      // marker is NOT forwarded: at this level its only effect (marking the pipe promise
      // handled) is applied by pipeThrough() itself.
      auto optionsObj = js.obj();
      KJ_IF_SOME(preventAbort, options.preventAbort) {
        optionsObj.set(js, "preventAbort"_kj, js.boolean(preventAbort));
      }
      KJ_IF_SOME(preventCancel, options.preventCancel) {
        optionsObj.set(js, "preventCancel"_kj, js.boolean(preventCancel));
      }
      KJ_IF_SOME(preventClose, options.preventClose) {
        optionsObj.set(js, "preventClose"_kj, js.boolean(preventClose));
      }
      KJ_IF_SOME(signal, options.signal) {
        auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<AbortSignal>>());
        optionsObj.set(js, "signal"_kj, jsg::JsValue(handler.wrap(js, kj::mv(signal))));
      }

      // Call out to the TypeScript implementation's shared pipeTo entry point via
      // cppExports -- NOT the user-patchable pipeTo property, so a user-replaced
      // stream.pipeTo cannot intercept internal pipes (the same captured-call discipline
      // as cancel/tee). When BOTH ends are native-backed and no prevent* option is set,
      // the pipe dispatch extracts the two endpoints and runs the pipe entirely at the
      // C++ layer via the native sink's pipeFrom hook; otherwise the JS pump drives the
      // pipe.
      auto handle = source.getHandle(js);
      auto res = JSG_REQUIRE_NONNULL(JSG_TRY_CAST_PROMISE(webstreams::dispatchCall(
                                         js, "readableStreamPipeTo", handle, dest, optionsObj)),
          TypeError, "The readableStreamPipeTo function did not return a promise");
      return js.toVoidPromise(res).then(
          js, [source = source.addRef(js), dest = dest.addRef(js)](jsg::Lock& js) {});
    }
  }
  KJ_UNREACHABLE;
}

JsReadableStream JsReadableStream::pipeThrough(
    jsg::Lock& js, JsReadableWritablePair transform, PipeToOptions options) {
  JSG_REQUIRE(!isLocked(js), TypeError, "The ReadableStream has been locked to a reader.");
  JSG_REQUIRE(
      !transform.writable.isLocked(js), TypeError, "This WritableStream is locked to a writer.");
  // We set up the pipeTo promise but we don't need to await it here.
  pipeTo(js, transform.writable, kj::mv(options)).markAsHandled(js);
  return kj::mv(transform.readable);
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
      auto result = webstreams::dispatchCall(js, "readableStreamTee", obj.getHandle(js));
      auto branches = KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST(result, JsArray), "readableStreamTee must return an array");
      KJ_REQUIRE(branches.size() == 2, "readableStreamTee must return two branches");
      auto branch1 = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(branches.get(js, 0)));
      auto branch2 = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_OBJECT(branches.get(js, 1)));

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
      // The TypeScript-side detach performs the whole takeover atomically: precondition
      // checks (legacy-exact error texts), internal-state transfer into a fresh stream,
      // and neutralization of the original (permanently locked + disturbed).
      auto detached = webstreams::dispatchCall(js, "detachReadableStream", obj.getHandle(js),
          js.boolean(ignoreDisturbed == IgnoreDisturbed::YES));
      auto detachedObj = KJ_REQUIRE_NONNULL(
          JSG_TRY_CAST_OBJECT(detached), "detachReadableStream did not return a stream");
      return JsReadableStream(Impl{
        .stream = StreamImpl(detachedObj.addRef(js)),
        .maybeOwnedBuffer = i.maybeOwnedBuffer.map([](kj::Rc<Buffer>& b) { return b.addRef(); }),
      });
    }
  }
  KJ_UNREACHABLE;
}

void JsReadableStream::nullify() {
  impl = kj::none;
}

void JsReadableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  auto& i = KJ_ASSERT_NONNULL(impl, "serialize() called on a null JsReadableStream");
  KJ_SWITCH_ONEOF(i.stream) {
    KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
      stream->serialize(js, serializer);
    }
    KJ_CASE_ONEOF(obj, jsg::JsRef<jsg::JsObject>) {
      // Mirrors ReadableStream::serialize(): pumpTo() performs the lock/disturb validation,
      // so the stream must not be modified before that call (the encoding/length queries are
      // non-mutating reads).
      auto& externalHandler = requireReadableStreamRpcSerializer(serializer);

      IoContext& ioctx = IoContext::current();

      auto encoding = getPreferredEncoding(js);
      auto expectedLength = tryGetLength(js, encoding);

      auto sink = newReadableStreamSerializeSink(externalHandler, encoding, expectedLength);

      ioctx.addTask(ioctx.waitForDeferredProxy(pumpTo(js, kj::mv(sink), EndStream::YES))
                        .catch_([](kj::Exception&& e) {
        // Errors in pumpTo() are automatically propagated to the source and destination. We
        // don't want to throw them from here since it'll cause an uncaught exception to be
        // reported, even if the application actually does handle it!
      }));
    }
  }
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
    KJ_IF_SOME(byobRequest, JSG_TRY_CAST_OBJECT(controller.get(js, "byobRequest"_kj))) {
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
      webstreams::invokeMethod(js, controller, "enqueue"_kj, chunk);
    }
    return js.resolvedPromise();
  }

  ensureScratch(kScratchSize);

  // Fast path: complete the read synchronously when data is already available (e.g.
  // buffered tee or decompressed data), settling the pull without an event-loop round
  // trip. Skipped when the read has already been abandoned: the asynchronous path's
  // settlement is inert in that case (the bytes are stashed for redelivery), and there
  // is no reason to hurry it.
  if (!signal->getAborted(js) &&
      util::Autogate::isEnabled(util::AutogateKey::STREAM_CONTROLLER_SYNC_FAST_PATHS)) {
    kj::Maybe<size_t> maybeSyncAmount;
    KJ_TRY {
      maybeSyncAmount = active.source->tryReadSync(scratch.asPtr(), 1);
    }
    KJ_CATCH(exception) {
      // tryReadSync() may throw when a synchronous read is possible but fails. Handle it
      // exactly like the asynchronous catch_ below: the source is no longer usable.
      state = kj::none;
      return js.rejectedPromise<void>(
          jsg::JsValue(js.exceptionToJs(kj::mv(exception)).getHandle(js)));
    }
    KJ_IF_SOME(amount, maybeSyncAmount) {
      // Mirrors the asynchronous continuation below, minus the in-flight-only concerns:
      // no cancel() or abandonment can have interleaved within this synchronous frame.
      if (amount == 0) {
        // EOF. Settle our own state before notifying, in case the close call re-enters.
        state = kj::none;
        webstreams::invokeMethod(js, controller, "close"_kj);
        return js.resolvedPromise();
      }
      webstreams::invokeMethod(
          js, controller, "enqueue"_kj, jsg::JsUint8Array::create(js, scratch.first(amount)));
      return js.resolvedPromise();
    }
  }

  auto& ioContext = IoContext::current();
  pullInFlight = true;
  return ioContext
      .awaitIo(js, active.source->tryRead(scratch.begin(), 1, scratch.size()),
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
    auto data = self->scratch.first(amount);
    if (signal->getAborted(js)) {
      // The consumer abandoned the read while it was in flight (e.g. releaseLock()).
      // Retain the bytes for redelivery on the next pull; the conduit treats this pull's
      // settlement as inert.
      self->stash.addAll(data);
      return;
    }
    if (amount == 0) {
      // EOF. Settle our own state before notifying, in case the close call re-enters.
      self->state = kj::none;
      webstreams::invokeMethod(js, controller.getHandle(js), "close"_kj);
      return;
    }
    webstreams::invokeMethod(
        js, controller.getHandle(js), "enqueue"_kj, jsg::JsUint8Array::create(js, data));
  }).catch_(js, [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
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
      JSG_TRY_CAST_UINT8ARRAY(byobRequest.get(js, "view"_kj)), "the BYOB request has no view");
  size_t atLeast = 1;
  KJ_IF_SOME(num, JSG_TRY_CAST(byobRequest.get(js, "atLeast"_kj), JsNumber)) {
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
      webstreams::invokeMethod(js, byobRequest, "respond"_kj, js.num(static_cast<double>(amount)));
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

  // Fast path: complete the read synchronously when the data is already available,
  // settling the pull without an event-loop round trip. Skipped when the read has
  // already been abandoned (matching pullDefault's fast path). Unlike the asynchronous
  // read, no JS runs between the view extraction above and the delivery below, so the
  // view cannot have been detached in between.
  if (!signal->getAborted(js) &&
      util::Autogate::isEnabled(util::AutogateKey::STREAM_CONTROLLER_SYNC_FAST_PATHS)) {
    kj::Maybe<size_t> maybeSyncAmount;
    KJ_TRY {
      maybeSyncAmount = active.source->tryReadSync(scratch.first(maxBytes), minBytes);
    }
    KJ_CATCH(exception) {
      // Handle a synchronous failure exactly like the asynchronous catch_ below.
      state = kj::none;
      return js.rejectedPromise<void>(
          jsg::JsValue(js.exceptionToJs(kj::mv(exception)).getHandle(js)));
    }
    KJ_IF_SOME(amount, maybeSyncAmount) {
      // Mirrors the asynchronous continuation below, minus the in-flight-only concerns:
      // no cancel(), abandonment, or view detachment can have interleaved within this
      // synchronous frame.
      size_t total = stashed + amount;
      if (total == 0) {
        // EOF with nothing to deliver: respond(0) is forbidden; close() is the EOF
        // signal.
        state = kj::none;
        webstreams::invokeMethod(js, controller, "close"_kj);
        return js.resolvedPromise();
      }
      KJ_ASSERT(total <= dest.size());
      if (stashed > 0) {
        // write() advances dest past the copied prefix, so the fresh bytes below land
        // immediately after the redelivered stash.
        dest.write(stash.asPtr());
        stash.clear();
      }
      if (amount > 0) {
        dest.write(scratch.first(amount));
      }
      bool eof = amount < minBytes;
      if (eof) {
        // The source delivered fewer than minBytes: EOF (KJ semantics). Settle our own
        // state before making the JS calls below.
        state = kj::none;
      }
      webstreams::invokeMethod(js, byobRequest, "respond"_kj, js.num(static_cast<double>(total)));
      if (eof) {
        // Fused close-commit, as in the asynchronous continuation below.
        webstreams::invokeMethod(js, controller, "close"_kj);
      }
      return js.resolvedPromise();
    }
  }

  auto& ioContext = IoContext::current();
  pullInFlight = true;
  return ioContext
      .awaitIo(js, active.source->tryRead(scratch.begin(), minBytes, maxBytes),
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
    auto data = self->scratch.first(amount);
    if (signal->getAborted(js)) {
      // The consumer abandoned the read; retain the bytes (after any previously retained
      // ones, preserving order) for redelivery on the next pull.
      self->stash.addAll(data);
      return;
    }
    size_t stashed = self->stash.size();
    size_t total = stashed + amount;
    if (total == 0) {
      // EOF with nothing to deliver: respond(0) is forbidden; close() is the EOF signal.
      self->state = kj::none;
      webstreams::invokeMethod(js, controller.getHandle(js), "close"_kj);
      return;
    }
    auto dest = view.getHandle(js).asArrayPtr();
    if (dest.size() < total) {
      // The view was detached while the read was in flight. Treat the read as abandoned:
      // retain the bytes for the next consumer.
      self->stash.addAll(data);
      return;
    }
    if (stashed > 0) {
      // write() advances dest past the copied prefix, so the fresh bytes below land
      // immediately after the redelivered stash.
      dest.write(self->stash.asPtr());
      self->stash.clear();
    }
    if (amount > 0) {
      dest.write(data);
    }
    bool eof = amount < minBytes;
    if (eof) {
      // The source delivered fewer than minBytes: EOF (KJ semantics). Settle our own
      // state before making the JS calls below.
      self->state = kj::none;
    }
    webstreams::invokeMethod(
        js, byobRequest.getHandle(js), "respond"_kj, js.num(static_cast<double>(total)));
    if (eof) {
      // Fused close-commit: deliver the partial bytes, then explicitly signal EOF in the
      // same pull turn. (The under-delivered respond() above already implies closure to
      // the conduit, which tolerates this close as a no-op; the explicit close keeps the
      // EOF signal unambiguous rather than relying on that inference.)
      webstreams::invokeMethod(js, controller.getHandle(js), "close"_kj);
    }
  }).catch_(js, [self = JSG_THIS](jsg::Lock& js, jsg::Value exception) mutable {
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
    // a ReadableStreamSource. wrapTeeBranch applies the same tee error
    // translation the legacy path uses.
    auto tee = kj::newTee(
        kj::heap<TeeInputAdapter>(kj::Own<ReadableStreamSource>(kj::mv(active.source))), limit);
    return ReadableStreamSource::Tee{
      .branches = {kj::heap<TeeBranchSource>(wrapTeeBranch(kj::mv(tee.branches[0]))),
        kj::heap<TeeBranchSource>(wrapTeeBranch(kj::mv(tee.branches[1])))},
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
  KJ_IF_SOME(length, tryGetLength(StreamEncoding::IDENTITY)) {
    return js.bigInt(length);
  }
  return kj::none;
}

kj::Maybe<uint64_t> ReadableStreamNativeSource::tryGetLength(StreamEncoding encoding) {
  KJ_IF_SOME(active, state) {
    if (encoding == StreamEncoding::IDENTITY) {
      KJ_IF_SOME(length, active.source->tryGetLength(StreamEncoding::IDENTITY)) {
        // Bytes retained in the stash (from an abandoned pull, or inherited from a tee
        // parent) were already consumed from the underlying source but not yet delivered,
        // so they count toward the total this source will produce. Getting this right
        // matters for tee branches: the conduit reads expectedLength at construction and
        // enforces it as an exact total.
        return length + stash.size();
      }
      return kj::none;
    }
    // Stashed bytes are identity bytes already drawn from the source: once any exist, an
    // encoded length no longer describes what this source will deliver.
    if (!stash.empty()) {
      return kj::none;
    }
    return active.source->tryGetLength(encoding);
  }
  // EOF'd, canceled, or consumed: nothing more will be produced, but distinguishing
  // "closed, hence zero" from "unknown" is the stream layer's business, not the
  // source's; report unknown.
  return kj::none;
}

StreamEncoding ReadableStreamNativeSource::getPreferredEncoding() {
  KJ_IF_SOME(active, state) {
    // Stashed bytes are identity bytes already drawn from the source: once any exist, the
    // remaining content is not entirely in the source's preferred encoding, and only
    // IDENTITY describes it.
    if (!stash.empty()) {
      return StreamEncoding::IDENTITY;
    }
    return active.source->getPreferredEncoding();
  }
  // EOF'd, canceled, or consumed: nothing more will be produced; IDENTITY trivially
  // describes the empty remainder.
  return StreamEncoding::IDENTITY;
}

kj::Own<ReadableStreamSource> ReadableStreamNativeSource::releaseForPump(jsg::Lock& js) {
  KJ_IF_SOME(active, state) {
    // Extraction requires an undisturbed stream, and any pull implies a read (which
    // disturbs), so no read can be in flight here.
    KJ_ASSERT(!pullInFlight);
    kj::Own<ReadableStreamSource> source(kj::mv(active.source));
    state = kj::none;
    if (!stash.empty()) {
      // Stashed bytes were already consumed from the source, so the source will never
      // produce them: serve them ahead of everything else.
      source = kj::heap<PrefixedSource>(stash.releaseAsArray(), kj::mv(source));
    }
    return source;
  }
  // The source already completed (EOF, or cancel released it); the pump simply finishes.
  return kj::heap<NullSource>();
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
