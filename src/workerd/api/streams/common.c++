// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "common.h"

#include "identity-transform-stream.h"

#include <workerd/api/util.h>
#include <workerd/io/features.h>

namespace workerd::api {

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::PromiseResolverPair<void> prp, jsg::JsValue reason, Reject reject)
    : resolver(kj::mv(prp.resolver)),
      promise(kj::mv(prp.promise)),
      reason(reason.addRef(js)),
      reject(reject) {}

WritableStreamController::PendingAbort::PendingAbort(
    jsg::Lock& js, jsg::JsValue reason, Reject reject)
    : WritableStreamController::PendingAbort(js, js.newPromiseAndResolver<void>(), reason, reject) {
}

void WritableStreamController::PendingAbort::complete(jsg::Lock& js) {
  if (reject) {
    fail(js, reason.getHandle(js));
  } else {
    maybeResolvePromise(js, resolver);
  }
}

void WritableStreamController::PendingAbort::fail(jsg::Lock& js, jsg::JsValue reason) {
  maybeRejectPromise<void>(js, resolver, reason);
}

kj::Maybe<kj::Own<WritableStreamController::PendingAbort>> WritableStreamController::PendingAbort::
    dequeue(kj::Maybe<kj::Own<WritableStreamController::PendingAbort>>& maybePendingAbort) {
  return kj::mv(maybePendingAbort);
}

// ====================================================================================

UnderlyingSinkImpl::UnderlyingSinkImpl(
    jsg::Lock& js, UnderlyingSink sink, StreamQueuingStrategy strategy)
    : start_(kj::mv(sink.start)),
      write_(kj::mv(sink.write)),
      writev_(kj::mv(sink.writev)),
      abort_(kj::mv(sink.abort)),
      close_(kj::mv(sink.close)),
      size_(kj::mv(strategy.size)),
      highWaterMark_(strategy.highWaterMark.orDefault(DEFAULT_HIGH_WATER_MARK)) {
  // Per the streams spec, the size function should be called with `undefined` as `this`,
  // not as a method on the strategy object.
  KJ_IF_SOME(size, size_) {
    size.setReceiver(js.v8Ref(js.v8Undefined()));
  }
  if (FeatureFlags::get(js).getPedanticWpt()) {
    // Per the spec, the type property for WritableStream's underlying sink must be undefined.
    // If it's anything else, throw a RangeError.
    JSG_REQUIRE(sink.type == kj::none, RangeError,
        "Invalid underlying sink type. Only undefined is valid.");
  }
}

void UnderlyingSinkImpl::clearStart() {
  start_ = kj::none;
}

void UnderlyingSinkImpl::clear() {
  start_ = kj::none;
  write_ = kj::none;
  writev_ = kj::none;
  abort_ = kj::none;
  close_ = kj::none;
  size_ = kj::none;
}

UnderlyingSourceImpl::UnderlyingSourceImpl(
    jsg::Lock& js, UnderlyingSource source, StreamQueuingStrategy strategy)
    : start_(kj::mv(source.start)),
      pull_(kj::mv(source.pull)),
      cancel_(kj::mv(source.cancel)),
      size_(kj::mv(strategy.size)),
      isBytes_(source.type.map([](kj::StringPtr s) { return s == "bytes"; }).orDefault(false)),
      highWaterMark_(strategy.highWaterMark.orDefault(
          isBytes_ ? DEFAULT_HIGH_WATER_MARK_BYTES : DEFAULT_HIGH_WATER_MARK_VALUE)),
      expectedLength_(source.expectedLength),
      autoAllocateChunkSize_(source.autoAllocateChunkSize) {
  // Per the streams spec, the size function should be called with `undefined` as `this`,
  // not as a method on the strategy object.
  KJ_IF_SOME(size, size_) {
    size.setReceiver(js.v8Ref(js.v8Undefined()));
  }
  // Per the spec, the type property for ReadableStream's underlying source must be
  // undefined, the empty string, or "bytes".
  KJ_IF_SOME(type, source.type) {
    JSG_REQUIRE(type == "" || type == "bytes", RangeError,
        "Invalid underlying source type. Only undefined, '' and 'bytes' are valid.");
  }
}

void UnderlyingSourceImpl::clearStart() {
  start_ = kj::none;
}

void UnderlyingSourceImpl::clear() {
  start_ = kj::none;
  pull_ = kj::none;
  cancel_ = kj::none;
  size_ = kj::none;
}

TransformerImpl::TransformerImpl(jsg::Lock& js, Transformer transformer)
    : start_(kj::mv(transformer.start)),
      transform_(kj::mv(transformer.transform)),
      flush_(kj::mv(transformer.flush)),
      cancel_(kj::mv(transformer.cancel)),
      transformv_(kj::mv(transformer.transformv)) {
  // Per the spec, both readableType and writableType must be undefined.
  JSG_REQUIRE(transformer.readableType == kj::none, RangeError,
      "Invalid transformer readableType. Only undefined is valid.");
  JSG_REQUIRE(transformer.writableType == kj::none, RangeError,
      "Invalid transformer writableType. Only undefined is valid.");
}

void TransformerImpl::clearStart() {
  start_ = kj::none;
}

void TransformerImpl::clear() {
  start_ = kj::none;
  transform_ = kj::none;
  flush_ = kj::none;
  cancel_ = kj::none;
  transformv_ = kj::none;
}

// Adapt ReadableStreamSource to kj::AsyncInputStream's interface for use with `kj::newTee()`.
TeeAdapter::TeeAdapter(kj::Own<ReadableStreamSource> inner): inner(kj::mv(inner)) {}

kj::Promise<size_t> TeeAdapter::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  return inner->tryRead(buffer, minBytes, maxBytes);
}

kj::Maybe<uint64_t> TeeAdapter::tryGetLength() {
  return inner->tryGetLength(StreamEncoding::IDENTITY);
}

TeeBranch::TeeBranch(kj::Own<kj::AsyncInputStream> inner): inner(kj::mv(inner)) {}

kj::Promise<size_t> TeeBranch::tryRead(void* buffer, size_t minBytes, size_t maxBytes) {
  return inner->tryRead(buffer, minBytes, maxBytes);
}

kj::Promise<DeferredProxy<void>> TeeBranch::pumpTo(WritableStreamSink& output, End end) {
#ifdef KJ_NO_RTTI
  // Yes, I'm paranoid.
  static_assert(!KJ_NO_RTTI, "Need RTTI for correctness");
#endif

  // HACK: If `output` is another TransformStream, we don't allow pumping to it, in order to
  //   guarantee that we can't create cycles. Note that currently TeeBranch only ever wraps
  //   TransformStreams, never system streams.
  JSG_REQUIRE(!isIdentityTransformStream(output), TypeError,
      "Inter-TransformStream ReadableStream.pipeTo() is not implemented.");

  // It is important we actually call `inner->pumpTo()` so that `kj::newTee()` is aware of this
  // pump operation's backpressure. So we can't use the default `ReadableStreamSource::pumpTo()`
  // implementation, and have to implement our own.

  PumpAdapter outputAdapter(output);
  co_await inner->pumpTo(outputAdapter);

  if (end) {
    co_await output.end();
  }

  // We only use `TeeBranch` when a locally-sourced stream was tee'd (because system streams
  // implement `tryTee()` in a different way that doesn't use `TeeBranch`). So, we know that
  // none of the pump can be performed without the IoContext active, and thus we do not
  // `KJ_CO_MAGIC BEGIN_DEFERRED_PROXYING`.
  co_return;
}

kj::Maybe<uint64_t> TeeBranch::tryGetLength(StreamEncoding encoding) {
  if (encoding == StreamEncoding::IDENTITY) {
    return inner->tryGetLength();
  } else {
    return kj::none;
  }
}

kj::Maybe<ReadableStreamSource::Tee> TeeBranch::tryTee(uint64_t limit) {
  KJ_IF_SOME(t, inner->tryTee(limit)) {
    auto branch = kj::heap<TeeBranch>(newTeeErrorAdapter(kj::mv(t)));
    auto consumed = kj::heap<TeeBranch>(kj::mv(inner));
    return Tee{kj::mv(branch), kj::mv(consumed)};
  }

  return kj::none;
}

void TeeBranch::cancel(kj::Exception reason) {
  // TODO(someday): What to do?
}

TeeBranch::PumpAdapter::PumpAdapter(WritableStreamSink& inner): inner(inner) {}

kj::Promise<void> TeeBranch::PumpAdapter::write(kj::ArrayPtr<const byte> buffer) {
  return inner.write(buffer);
}

kj::Promise<void> TeeBranch::PumpAdapter::write(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return inner.write(pieces);
}

kj::Promise<void> TeeBranch::PumpAdapter::whenWriteDisconnected() {
  KJ_UNIMPLEMENTED("whenWriteDisconnected() not expected on PumpAdapter");
}

}  // namespace workerd::api
