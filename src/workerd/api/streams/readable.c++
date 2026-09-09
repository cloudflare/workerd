// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "readable.h"

#include "internal.h"
#include "writable.h"

#include <workerd/api/js-readable-stream.h>
#include <workerd/api/system-streams.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/autogate.h>

namespace workerd::api {

ReaderImpl::ReaderImpl(kj::Ptr<ReadableStreamController::Reader> reader)
    : reader(kj::mv(reader)),
      state(ReaderState::create<Initial>()) {}

ReaderImpl::~ReaderImpl() noexcept(false) {
  KJ_IF_SOME(attached, state.tryGetActiveUnsafe()) {
    attached.stream->getController().releaseReader(reader, kj::none);
  }
}

void ReaderImpl::attach(jsg::Ref<ReadableStream> stream, jsg::Promise<void> closedPromise) {
  KJ_ASSERT(state.is<Initial>());
  state.transitionTo<Attached>(kj::mv(stream));
  this->closedPromise = kj::mv(closedPromise);
}

void ReaderImpl::detach() {
  // Only transition from Attached to Closed.
  // All other states (Initial, Closed, Released) are no-ops.
  if (state.isActive()) {
    state.transitionTo<Closed>();
  }
}

jsg::Promise<void> ReaderImpl::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  assertAttachedOrTerminal();
  if (state.is<Released>()) {
    return js.rejectedPromise<void>(
        js.typeError("This ReadableStream reader has been released."_kj));
  }
  if (state.is<Closed>()) {
    return js.resolvedPromise();
  }
  auto& attached = state.requireActiveUnsafe();
  // In some edge cases, this reader is the last thing holding a strong
  // reference to the stream. Calling cancel might cause the readers strong
  // reference to be cleared, so let's make sure we keep a reference to
  // the stream at least until the call to cancel completes.
  auto ref = attached.stream.addRef();
  return attached.stream->getController().cancel(js, maybeReason);
}

jsg::MemoizedIdentity<jsg::Promise<void>>& ReaderImpl::getClosed() {
  // The closed promise should always be set after the object is created so this assert
  // should always be safe.
  return KJ_ASSERT_NONNULL(closedPromise);
}

void ReaderImpl::lockToStream(jsg::Lock& js, ReadableStream& stream) {
  KJ_ASSERT(!stream.isLocked());
  KJ_ASSERT(stream.getController().lockReader(js, reader));
}

jsg::Promise<ReadResult> ReaderImpl::read(
    jsg::Lock& js, kj::Maybe<ReadableStreamController::ByobOptions> byobOptions) {
  assertAttachedOrTerminal();
  if (state.is<Released>()) {
    return js.rejectedPromise<ReadResult>(
        js.typeError("This ReadableStream reader has been released."_kj));
  }
  if (state.is<Closed>()) {
    return js.rejectedPromise<ReadResult>(js.typeError("This ReadableStream has been closed."_kj));
  }
  auto& attached = state.requireActiveUnsafe();
  KJ_IF_SOME(options, byobOptions) {
    // Per the spec, we must perform these checks before disturbing the stream.
    size_t atLeast = options.atLeast.orDefault(1);
    auto byobView = options.bufferView.getHandle(js);

    if (byobView.isImmutable()) {
      return js.rejectedPromise<ReadResult>(
          js.typeError("Cannot call read() with an immutable ArrayBuffer."_kj));
    }

    size_t byteLength = byobView.size();
    auto elementSize = byobView.getElementSize();

    if (byteLength == 0) {
      return js.rejectedPromise<ReadResult>(
          js.typeError("You must call read() on a \"byob\" reader with a positive-sized "
                       "TypedArray object."_kj));
    }
    if (atLeast == 0) {
      return js.rejectedPromise<ReadResult>(js.typeError(
          kj::str("Requested invalid minimum number of bytes to read (", atLeast, ").")));
    }

    // Both read() and readAtLeast() pass atLeast in element count.
    // Convert to bytes before validation and forwarding to the controller.
    //
    // Reject on the raw element count first. An element occupies at least one byte, so a count
    // that already exceeds the buffer can never be satisfied, and rejecting here bounds atLeast by
    // the buffer size. That keeps the multiplication below from overflowing however large buffers
    // are allowed to get, and it catches a negative minElements, which reaches this point
    // sign-extended to a huge size_t.
    if (atLeast > byteLength) {
      return js.rejectedPromise<ReadResult>(js.typeError(kj::str(
          "Minimum bytes to read (", atLeast, ") exceeds size of buffer (", byteLength, ").")));
    }

    atLeast = atLeast * elementSize;

    if (atLeast > byteLength) {
      return js.rejectedPromise<ReadResult>(js.typeError(kj::str(
          "Minimum bytes to read (", atLeast, ") exceeds size of buffer (", byteLength, ").")));
    }

    options.atLeast = atLeast;
  }

  // Hold a strong reference to the stream across the read() call.
  // The read can synchronously invoke the user's pull() callback, which could
  // call reader.releaseLock() — dropping the jsg::Ref inside Attached. Without
  // this local ref, GC could collect the ReadableStream (and its controller /
  // ValueReadable / ByteReadable) while the C++ stack is still inside read().
  auto ref = attached.stream.addRef();
  return KJ_ASSERT_NONNULL(attached.stream->getController().read(js, kj::mv(byobOptions)));
}

void ReaderImpl::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending reads. This is a recent
  // modification to the spec that we have not yet implemented.
  assertAttachedOrTerminal();
  // Closed and Released states are no-ops.
  KJ_IF_SOME(attached, state.tryGetActiveUnsafe()) {
    // In some edge cases, this reader is the last thing holding a strong
    // reference to the stream. Calling releaseLock might cause the readers strong
    // reference to be cleared, so let's make sure we keep a reference to
    // the stream at least until the call to releaseLock completes.
    auto ref = attached.stream.addRef();
    attached.stream->getController().releaseReader(reader, js);
    state.transitionTo<Released>();
  }
}

void ReaderImpl::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(attached, state.tryGetActiveUnsafe()) {
    visitor.visit(attached.stream);
  }
  visitor.visit(closedPromise);
}

// ======================================================================================

ReadableStreamDefaultReader::ReadableStreamDefaultReader(): impl(addPtrToThis()) {}

jsg::Ref<ReadableStreamDefaultReader> ReadableStreamDefaultReader::constructor(
    jsg::Lock& js, jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(
      !stream->isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");
  auto reader = js.alloc<ReadableStreamDefaultReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamDefaultReader::attach(
    jsg::Ref<ReadableStream> stream, jsg::Promise<void> closedPromise) {
  impl.attach(kj::mv(stream), kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamDefaultReader::cancel(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  return impl.cancel(js, kj::mv(maybeReason));
}

void ReadableStreamDefaultReader::detach() {
  impl.detach();
}

jsg::MemoizedIdentity<jsg::Promise<void>>& ReadableStreamDefaultReader::getClosed() {
  return impl.getClosed();
}

void ReadableStreamDefaultReader::lockToStream(jsg::Lock& js, ReadableStream& stream) {
  impl.lockToStream(js, stream);
}

jsg::Promise<ReadResult> ReadableStreamDefaultReader::read(jsg::Lock& js) {
  return impl.read(js, kj::none);
}

void ReadableStreamDefaultReader::releaseLock(jsg::Lock& js) {
  impl.releaseLock(js);
}

void ReadableStreamDefaultReader::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

// ======================================================================================

ReadableStreamBYOBReader::ReadableStreamBYOBReader(): impl(addPtrToThis()) {}

jsg::Ref<ReadableStreamBYOBReader> ReadableStreamBYOBReader::constructor(
    jsg::Lock& js, jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(
      !stream->isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");

  if (!stream->getController().isClosedOrErrored()) {
    JSG_REQUIRE(stream->getController().isByteOriented(), TypeError,
        "This ReadableStream does not support BYOB reads.");
  }

  auto reader = js.alloc<ReadableStreamBYOBReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamBYOBReader::attach(
    jsg::Ref<ReadableStream> stream, jsg::Promise<void> closedPromise) {
  impl.attach(kj::mv(stream), kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamBYOBReader::cancel(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  return impl.cancel(js, kj::mv(maybeReason));
}

void ReadableStreamBYOBReader::detach() {
  impl.detach();
}

jsg::MemoizedIdentity<jsg::Promise<void>>& ReadableStreamBYOBReader::getClosed() {
  return impl.getClosed();
}

void ReadableStreamBYOBReader::lockToStream(jsg::Lock& js, ReadableStream& stream) {
  impl.lockToStream(js, stream);
}

jsg::Promise<ReadResult> ReadableStreamBYOBReader::read(jsg::Lock& js,
    jsg::JsArrayBufferView byobBuffer,
    jsg::Optional<ReadableStreamBYOBReaderReadOptions> maybeOptions) {
  static const ReadableStreamBYOBReaderReadOptions defaultOptions{};
  auto options = ReadableStreamController::ByobOptions{
    .bufferView = byobBuffer.addRef(js),
    .atLeast = maybeOptions.orDefault(defaultOptions).min.orDefault(1),
    .detachBuffer = FeatureFlags::get(js).getStreamsByobReaderDetachesBuffer(),
  };
  return impl.read(js, kj::mv(options));
}

jsg::Promise<ReadResult> ReadableStreamBYOBReader::readAtLeast(
    jsg::Lock& js, int minElements, jsg::JsArrayBufferView byobBuffer) {
  auto options = ReadableStreamController::ByobOptions{
    .bufferView = byobBuffer.addRef(js),
    .atLeast = minElements,
    .detachBuffer = true,
  };
  return impl.read(js, kj::mv(options));
}

void ReadableStreamBYOBReader::releaseLock(jsg::Lock& js) {
  impl.releaseLock(js);
}

void ReadableStreamBYOBReader::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

// ======================================================================================
// DrainingReader implementation

DrainingReader::DrainingReader() {}

DrainingReader::~DrainingReader() noexcept(false) {
  KJ_IF_SOME(stream, state.tryGet<Attached>()) {
    stream->getController().releaseReader(addPtrToThis(), kj::none);
  }
}

kj::Maybe<kj::Own<DrainingReader>> DrainingReader::create(jsg::Lock& js, ReadableStream& stream) {
  if (stream.isLocked()) {
    return kj::none;
  }
  auto reader = kj::heap<DrainingReader>();
  if (!stream.getController().lockReader(js, reader->getPtr())) {
    return kj::none;
  }
  return kj::mv(reader);
}

void DrainingReader::attach(jsg::Ref<ReadableStream> stream, jsg::Promise<void> closedPromise) {
  KJ_ASSERT(state.is<Initial>());
  state = kj::mv(stream);
  this->closedPromise = kj::mv(closedPromise);
}

void DrainingReader::detach() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      return;
    }
    KJ_CASE_ONEOF(stream, Attached) {
      state.init<StreamStates::Closed>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      return;
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<DrainingReadResult> DrainingReader::read(jsg::Lock& js, size_t maxRead) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      auto& controller = stream->getController();
      KJ_IF_SOME(result, controller.drainingRead(js, maxRead)) {
        return kj::mv(result);
      }
      return js.rejectedPromise<DrainingReadResult>(
          js.typeError("Unable to perform draining read on this stream."_kj));
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<DrainingReadResult>(
          js.typeError("This ReadableStream reader has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.resolvedPromise(DrainingReadResult{
        .chunks = kj::Array<kj::Array<kj::byte>>(),
        .done = true,
      });
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> DrainingReader::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      auto ref = stream.addRef();
      return stream->getController().cancel(js, maybeReason);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.typeError("This ReadableStream reader has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
}

void DrainingReader::releaseLock(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      auto ref = stream.addRef();
      stream->getController().releaseReader(addPtrToThis(), js);
      state.init<Released>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      return;
    }
  }
  KJ_UNREACHABLE;
}

bool DrainingReader::isAttached() const {
  return state.is<Attached>();
}

void DrainingReader::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(stream, state.tryGet<Attached>()) {
    visitor.visit(stream);
  }
  visitor.visit(closedPromise);
}

// ======================================================================================

ReadableStream::ReadableStream(IoContext& ioContext, kj::Own<ReadableStreamSource> source)
    : ReadableStream(newReadableStreamInternalController(ioContext, kj::mv(source))) {}

ReadableStream::ReadableStream(kj::Own<ReadableStreamController> controller)
    : controller(kj::mv(controller)) {
  getController().setOwnerRef(PtrTarget::addWeakToThis());
}

void ReadableStream::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(getController());
  KJ_IF_SOME(pair, eofResolverPair) {
    visitor.visit(pair.resolver);
    visitor.visit(pair.promise);
  }
}

jsg::Ref<ReadableStream> ReadableStream::addRef() {
  return JSG_THIS;
}

bool ReadableStream::isDisturbed() {
  return getController().isDisturbed();
}

bool ReadableStream::isLocked() {
  return getController().isLockedToReader();
}

jsg::Promise<void> ReadableStream::onEof(jsg::Lock& js) {
  eofResolverPair = js.newPromiseAndResolver<void>();
  return kj::mv(KJ_ASSERT_NONNULL(eofResolverPair).promise);
}

void ReadableStream::signalEof(jsg::Lock& js) {
  KJ_IF_SOME(pair, eofResolverPair) {
    pair.resolver.resolve(js);
  }
}

ReadableStreamController& ReadableStream::getController() {
  return *controller;
}

jsg::Promise<void> ReadableStream::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.typeError("This ReadableStream is currently locked to a reader."_kj));
  }
  return getController().cancel(js, maybeReason);
}

ReadableStream::Reader ReadableStream::getReader(
    jsg::Lock& js, jsg::Optional<GetReaderOptions> options) {
  JSG_REQUIRE(!isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");

  bool isByob = false;
  KJ_IF_SOME(o, options) {
    KJ_IF_SOME(mode, o.mode) {
      JSG_REQUIRE(
          mode == "byob", TypeError, "mode must be undefined or 'byob' in call to getReader().");
      // No need to check that the ReadableStream implementation is a byte stream: the first
      // invocation of read() will do that for us and throw if necessary. Also, we should really
      // just support reading non-byte streams with BYOB readers.
      isByob = true;
    }
  }

  if (isByob) {
    return ReadableStreamBYOBReader::constructor(js, JSG_THIS);
  }
  return ReadableStreamDefaultReader::constructor(js, JSG_THIS);
}

jsg::Ref<ReadableStream::ReadableStreamAsyncIterator> ReadableStream::values(
    jsg::Lock& js, jsg::Optional<ValuesOptions> options) {
  static const auto defaultOptions = ValuesOptions{};
  return js.alloc<ReadableStreamAsyncIterator>(
      AsyncIteratorState{.reader = ReadableStreamDefaultReader::constructor(js, JSG_THIS),
        .preventCancel = options.orDefault(defaultOptions).preventCancel.orDefault(false)});
}

jsg::Ref<ReadableStream> ReadableStream::pipeThrough(
    jsg::Lock& js, Transform transform, jsg::Optional<PipeToOptions> maybeOptions) {
  auto& controller = getController();

  auto& destination = transform.writable->getController();
  JSG_REQUIRE(!isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");
  JSG_REQUIRE(!destination.isLockedToWriter(), TypeError,
      "This WritableStream is currently locked to a writer.");

  auto options = kj::mv(maybeOptions).orDefault({});
  options.pipeThrough = true;
  // The lambda intentionally captures self as a visitable reference, ensuring
  // JSG_THIS stays alive until the pipe promise resolves.
  controller.pipeTo(js, destination, kj::mv(options))
      .then(js, [self = JSG_THIS](jsg::Lock& js) {
    return js.resolvedPromise();
  }).markAsHandled(js);
  return kj::mv(transform.readable);
}

jsg::Promise<void> ReadableStream::pipeTo(jsg::Lock& js,
    jsg::Ref<WritableStream> destination,
    jsg::Optional<PipeToOptions> maybeOptions) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.typeError("This ReadableStream is currently locked to a reader."_kj));
  }

  if (destination->getController().isLockedToWriter()) {
    return js.rejectedPromise<void>(
        js.typeError("This WritableStream is currently locked to a writer"_kj));
  }

  auto options = kj::mv(maybeOptions).orDefault({});
  return getController().pipeTo(js, destination->getController(), kj::mv(options));
}

kj::Array<jsg::Ref<ReadableStream>> ReadableStream::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLocked(), TypeError, "This ReadableStream is currently locked to a reader,");
  auto tee = getController().tee(js);
  return kj::arr(kj::mv(tee.branch1), kj::mv(tee.branch2));
}

jsg::JsString ReadableStream::inspectState(jsg::Lock& js) {
  if (controller->isClosedOrErrored()) {
    return js.strIntern(controller->isClosed() ? "closed"_kj : "errored"_kj);
  } else {
    return js.strIntern("readable"_kj);
  }
}

bool ReadableStream::inspectSupportsBYOB() {
  return controller->isByteOriented();
}

jsg::Optional<uint64_t> ReadableStream::inspectLength() {
  return tryGetLength(StreamEncoding::IDENTITY);
}

jsg::Promise<kj::Maybe<jsg::V8Ref<v8::Value>>> ReadableStream::nextFunction(
    jsg::Lock& js, AsyncIteratorState& state) {
  return state.reader->read(js).then(
      js, [reader = state.reader.addRef()](jsg::Lock& js, ReadResult result) mutable {
    if (result.done) {
      reader->releaseLock(js);
      return js.resolvedPromise(kj::Maybe<jsg::V8Ref<v8::Value>>(kj::none));
    }
    return js.resolvedPromise<kj::Maybe<jsg::V8Ref<v8::Value>>>(kj::mv(result.value));
  });
}

jsg::Promise<void> ReadableStream::returnFunction(
    jsg::Lock& js, AsyncIteratorState& state, jsg::Optional<jsg::V8Ref<v8::Value>>& value) {
  if (state.reader.get() != nullptr) {
    auto reader = kj::mv(state.reader);
    if (!state.preventCancel) {
      auto promise =
          reader->cancel(js, value.map([&](jsg::V8Ref<v8::Value>& v) { return v.getHandle(js); }));
      reader->releaseLock(js);
      auto result = promise.then(js, [reader = kj::mv(reader)](jsg::Lock& js) mutable {
        // Ensure that the reader is not garbage collected until the cancel promise resolves.
        return js.resolvedPromise();
      });
      // When the stream is already errored, cancel() returns a rejected promise
      // that propagates through the .then() chain. Mark it as handled so V8 does
      // not fire unhandledrejection events during iterator teardown.
      result.markAsHandled(js);
      return kj::mv(result);
    }

    reader->releaseLock(js);
  }
  return js.resolvedPromise();
}

jsg::Ref<ReadableStream> ReadableStream::detach(jsg::Lock& js, bool ignoreDisturbed) {
  JSG_REQUIRE(
      !isDisturbed() || ignoreDisturbed, TypeError, "The ReadableStream has already been read.");
  JSG_REQUIRE(!isLocked(), TypeError, "The ReadableStream has been locked to a reader.");
  return js.alloc<ReadableStream>(getController().detach(js, ignoreDisturbed));
}

kj::Maybe<uint64_t> ReadableStream::tryGetLength(StreamEncoding encoding) {
  return getController().tryGetLength(encoding);
}

kj::Promise<DeferredProxy<void>> ReadableStream::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, bool end) {
  JSG_REQUIRE(
      IoContext::hasCurrent(), Error, "Unable to consume this ReadableStream outside of a request");
  JSG_REQUIRE(!isLocked(), TypeError, "The ReadableStream has been locked to a reader.");
  return getController().pumpTo(js, kj::mv(sink), end);
}

jsg::Ref<ReadableStream> ReadableStream::constructor(jsg::Lock& js,
    jsg::Optional<UnderlyingSource> underlyingSource,
    jsg::Optional<StreamQueuingStrategy> queuingStrategy) {

  JSG_REQUIRE(FeatureFlags::get(js).getStreamsJavaScriptControllers(), Error,
      "To use the new ReadableStream() constructor, enable the "
      "streams_enable_constructors compatibility flag. "
      "Refer to the docs for more information: https://developers.cloudflare.com/workers/platform/compatibility-dates/#compatibility-flags");
  // We account for the memory usage of the ReadableStream and its controller together because their
  // lifetimes are identical and memory accounting itself has a memory overhead.
  auto controller = newReadableStreamJsController();
  auto stream = js.allocAccounted<ReadableStream>(
      sizeof(ReadableStream) + controller->jsgGetMemorySelfSize(), kj::mv(controller));
  stream->getController().setup(js, kj::mv(underlyingSource), kj::mv(queuingStrategy));
  return kj::mv(stream);
}

jsg::Optional<uint32_t> ByteLengthQueuingStrategy::size(
    jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeValue) {
  KJ_IF_SOME(value, maybeValue) {
    if (value.isArrayBuffer()) {
      v8::Local<v8::ArrayBuffer> buffer = KJ_ASSERT_NONNULL(value.tryCast<jsg::JsArrayBuffer>());
      return buffer->ByteLength();
    } else if (value.isArrayBufferView()) {
      v8::Local<v8::ArrayBufferView> view =
          KJ_ASSERT_NONNULL(value.tryCast<jsg::JsArrayBufferView>());
      return view->ByteLength();
    } else {
      // Per the WHATWG Streams spec, ByteLengthQueuingStrategy.size should return
      // GetV(chunk, "byteLength"), which means getting the byteLength property
      // from any object, not just ArrayBuffer/ArrayBufferView.
      KJ_IF_SOME(obj, value.tryCast<jsg::JsObject>()) {
        auto byteLength = obj.get(js, "byteLength"_kj);
        KJ_IF_SOME(num, byteLength.tryCast<jsg::JsNumber>()) {
          KJ_IF_SOME(val, num.value(js)) {
            return static_cast<uint32_t>(val);
          }
        }
      }
    }
  }
  return kj::none;
}

namespace {

// Wrapper around ReadableStreamSource that prevents deferred proxying. We need this for RPC
// streams because although they are "system streams", they become disconnected when the IoContext
// is destroyed, due to the JsRpcCustomEvent being canceled.
//
// TODO(someday): Devise a better way for RPC streams to extend the lifetime of the RPC session
//   beyond the destruction of the IoContext, if it is being used for deferred proxying.
class NoDeferredProxyReadableStream final: public ReadableStreamSource {
 public:
  NoDeferredProxyReadableStream(kj::Own<ReadableStreamSource> inner, IoContext& ioctx)
      : inner(kj::mv(inner)),
        ioctx(ioctx) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes);
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    return inner->tryReadSync(buffer, minBytes);
  }

  kj::Promise<DeferredProxy<void>> pumpTo(kj::Ptr<WritableStreamSink> output, bool end) override {
    // Move the deferred proxy part of the task over to the non-deferred part. To do this,
    // we use `ioctx.waitForDeferredProxy()`, which returns a single promise covering both parts
    // (and, importantly, registering pending events where needed). Then, we add a noop deferred
    // proxy to the end of that.
    return addNoopDeferredProxy(ioctx.waitForDeferredProxy(inner->pumpTo(kj::mv(output), end)));
  }

  StreamEncoding getPreferredEncoding() override {
    return inner->getPreferredEncoding();
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    return inner->tryGetLength(encoding);
  }

  void cancel(kj::Exception reason) override {
    return inner->cancel(kj::mv(reason));
  }

  kj::Maybe<Tee> tryTee(uint64_t limit) override {
    return inner->tryTee(limit).map([&](Tee tee) {
      return Tee{.branches = {
                   kj::heap<NoDeferredProxyReadableStream>(kj::mv(tee.branches[0]), ioctx),
                   kj::heap<NoDeferredProxyReadableStream>(kj::mv(tee.branches[1]), ioctx),
                 }};
    });
  }

 private:
  kj::Own<ReadableStreamSource> inner;
  IoContext& ioctx;
};

}  // namespace

kj::Own<ReadableStreamSource> newNoDeferredProxyReadableStream(
    IoContext& context, kj::Own<ReadableStreamSource> inner) {
  return kj::heap<NoDeferredProxyReadableStream>(kj::mv(inner), context);
}

RpcSerializerExternalHandler& requireReadableStreamRpcSerializer(jsg::Serializer& serializer) {
  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "ReadableStream can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHandler*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "ReadableStream can only be serialized for RPC.");
  return *externalHandler;
}

namespace {

// The exception a ReadableStream's origin cancels its source with when the receiver of the
// stream (over RPC) reports that it is done with it. If the receiver supplied a reason, its
// serialized form rides along as SERIALIZED_CANCEL_REASON_DETAIL_ID.
kj::Exception rpcReceiverCanceledException(kj::Maybe<kj::Array<kj::byte>> serializedReason) {
  auto exception = JSG_KJ_EXCEPTION(DISCONNECTED, Error,
      "ReadableStream sent over RPC was canceled or released by the remote execution context.");
  KJ_IF_SOME(reason, serializedReason) {
    exception.setDetail(SERIALIZED_CANCEL_REASON_DETAIL_ID, kj::mv(reason));
  }
  return exception;
}

// Where the receiver's cancel lands on the origin's side. Owned by the sink (below); the
// StreamCanceler capability reaches it through a WeakRef, so the capability outliving the sink --
// or being dropped without ever being called, which carries no meaning -- has no effect.
class ReceiverCancelSignal final {
 private:
  // kj::Exception is not copyable, so the forked promise carries it in a refcounted box.
  struct Box: public kj::Refcounted {
    kj::Exception exception;
    explicit Box(kj::Exception exception): exception(kj::mv(exception)) {}
  };

 public:
  ReceiverCancelSignal(
      kj::PromiseFulfillerPair<kj::Rc<Box>> paf = kj::newPromiseAndFulfiller<kj::Rc<Box>>())
      : fulfiller(kj::mv(paf.fulfiller)),
        promise(paf.promise.fork()) {}
  ~ReceiverCancelSignal() noexcept(false) {
    weakRef->invalidate();
  }
  KJ_DISALLOW_COPY_AND_MOVE(ReceiverCancelSignal);

  kj::Own<WeakRef<ReceiverCancelSignal>> addWeakRef() {
    return weakRef->addRef();
  }

  kj::Promise<kj::Exception> whenCanceled() {
    return promise.addBranch().then([](kj::Rc<Box> box) { return box->exception.clone(); });
  }

  void fire(kj::Exception exception) {
    if (fulfiller->isWaiting()) {
      fulfiller->fulfill(kj::rc<Box>(kj::mv(exception)));
    }
  }

 private:
  kj::Own<kj::PromiseFulfiller<kj::Rc<Box>>> fulfiller;
  kj::ForkedPromise<kj::Rc<Box>> promise;
  kj::Own<WeakRef<ReceiverCancelSignal>> weakRef =
      kj::refcounted<WeakRef<ReceiverCancelSignal>>(kj::Badge<ReceiverCancelSignal>(), *this);
};

// The origin's end of the StreamCanceler return channel. Owned by the capability the receiver
// holds, so it can outlive the origin's pump, sink and even IoContext.
class StreamCancelerImpl final: public rpc::StreamCanceler::Server {
 public:
  explicit StreamCancelerImpl(kj::Own<WeakRef<ReceiverCancelSignal>> signal)
      : signal(kj::mv(signal)) {}

  kj::Promise<void> cancel(CancelContext context) override {
    kj::Maybe<kj::Array<kj::byte>> serializedReason;
    auto reason = context.getParams().getReason();
    if (reason.hasV8Serialized() && reason.getV8Serialized().size() > 0) {
      serializedReason = kj::heapArray<kj::byte>(reason.getV8Serialized());
    }
    signal->runIfAlive([&](ReceiverCancelSignal& signal) {
      signal.fire(rpcReceiverCanceledException(kj::mv(serializedReason)));
    });
    return kj::READY_NOW;
  }

 private:
  kj::Own<WeakRef<ReceiverCancelSignal>> signal;
};

// The sink a ReadableStream is pumped into when it is sent over RPC: the system stream over the
// peer's ByteStream, plus knowledge of the receiver canceling the stream, reported through
// whenWriteDisconnected() so the pump can cancel the source while it is idle.
class RpcReadableStreamSink final: public WritableStreamSink {
 public:
  RpcReadableStreamSink(kj::Own<WritableStreamSink> inner, kj::Own<ReceiverCancelSignal> canceled)
      : inner(kj::mv(inner)),
        canceled(kj::mv(canceled)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->write(pieces);
  }
  bool tryWriteSync(kj::ArrayPtr<const byte> buffer) override {
    return inner->tryWriteSync(buffer);
  }
  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->tryWriteSync(pieces);
  }
  kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      kj::Ptr<ReadableStreamSource> input, bool end) override {
    return inner->tryPumpFrom(kj::mv(input), end);
  }
  kj::Promise<void> end() override {
    return inner->end();
  }
  void abort(kj::Exception reason) override {
    inner->abort(kj::mv(reason));
  }
  StreamEncoding disownEncodingResponsibility() override {
    return inner->disownEncodingResponsibility();
  }
  kj::Promise<kj::Exception> whenWriteDisconnected() override {
    return canceled->whenCanceled();
  }

 private:
  kj::Own<WritableStreamSink> inner;
  kj::Own<ReceiverCancelSignal> canceled;
};

}  // namespace

kj::Own<WritableStreamSink> newReadableStreamSerializeSink(
    RpcSerializerExternalHandler& externalHandler,
    StreamEncoding encoding,
    kj::Maybe<uint64_t> expectedLength) {
  // Serialize by effectively creating a `JsRpcStub` around the stream and serializing that.
  // Except we don't actually want to do _exactly_ that, because we do not want to actually create
  // a `JsRpcStub` locally. So do the important parts of `JsRpcStub::constructor()` followed by
  // `JsRpcStub::serialize()`.

  IoContext& ioctx = IoContext::current();

  // The receiver reports canceling or releasing its copy of the stream through a StreamCanceler
  // capability that we host; its calls fire `canceled`, which the sink below exposes to the pump.
  kj::Maybe<kj::Own<ReceiverCancelSignal>> canceled;
  kj::Maybe<rpc::StreamCanceler::Client> canceler;
  if (util::Autogate::isEnabled(util::AutogateKey::JSRPC_READABLE_CANCEL_PROPAGATION)) {
    auto signal = kj::heap<ReceiverCancelSignal>();
    canceler = rpc::StreamCanceler::Client(kj::heap<StreamCancelerImpl>(signal->addWeakRef()));
    canceled = kj::mv(signal);
  }

  capnp::ByteStream::Client streamCap = [&]() {
    auto req = externalHandler.getExternalPusher().pushByteStreamRequest(capnp::MessageSize{2, 0});
    KJ_IF_SOME(el, expectedLength) {
      req.setLengthPlusOne(el + 1);
    }
    auto pipeline = req.sendForPipeline();

    externalHandler.write(
        [encoding, expectedLength, source = pipeline.getSource(), canceler = kj::mv(canceler)](
            rpc::JsValue::External::Builder builder) mutable {
      auto rs = builder.initReadableStream();
      rs.setStream(kj::mv(source));
      rs.setEncoding(encoding);
      KJ_IF_SOME(c, canceler) {
        rs.setCanceler(kj::mv(c));
      }
    });

    return pipeline.getSink();
  }();

  kj::Own<capnp::ExplicitEndOutputStream> kjStream =
      ioctx.getByteStreamFactory().capnpToKjExplicitEnd(kj::mv(streamCap));

  auto sink = newSystemStream(kj::mv(kjStream), encoding, ioctx);
  KJ_IF_SOME(c, canceled) {
    return kj::heap<RpcReadableStreamSink>(kj::mv(sink), kj::mv(c));
  }
  return kj::mv(sink);
}

void ReadableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // NOTE: We're counting on `pumpTo()`, below, to check that the stream is not locked or disturbed
  //   and other common checks. It's important that we don't modify the stream in any way before
  //   that call.

  auto& externalHandler = requireReadableStreamRpcSerializer(serializer);

  IoContext& ioctx = IoContext::current();

  auto& controller = getController();
  StreamEncoding encoding = controller.getPreferredEncoding();
  auto expectedLength = controller.tryGetLength(encoding);

  auto sink = newReadableStreamSerializeSink(externalHandler, encoding, expectedLength);

  ioctx.addTask(
      ioctx.waitForDeferredProxy(pumpTo(js, kj::mv(sink), true)).catch_([](kj::Exception&& e) {
    // Errors in pumpTo() are automatically propagated to the source and destination. We don't
    // want to throw them from here since it'll cause an uncaught exception to be reported, even
    // if the application actually does handle it!
  }));
}

namespace {

// The StreamCanceler that came along with a ReadableStream received over RPC, shared by every
// RpcReadableStreamSource standing for that stream: the one built at hydration and, after each
// tee(), one per branch. The origin is told to cancel its source once the last of them is done
// with the stream before EOF, with the most recent reason any of them gave (if any). It is told
// nothing once any of them reaches EOF or sees the origin's side fail, since the origin then has
// nothing left to learn from this side.
//
// The canceler is a capability of the RPC session that delivered the stream, and the session (with
// the callee's execution context) stays open while any such capability is held. It is therefore
// released the moment the origin no longer needs to hear from this side, not when this object is
// eventually collected.
class RpcStreamCancelerState final: public kj::Refcounted {
 public:
  RpcStreamCancelerState(rpc::StreamCanceler::Client canceler, IoContext& ioctx)
      : canceler(kj::mv(canceler)),
        ioctx(ioctx) {}

  void sourceCreated() {
    ++liveSources;
  }

  // A source is done with the stream before EOF: canceled, with its reason, or destroyed. The
  // origin is told once the last live source reports.
  void sourceDone(kj::Maybe<kj::Exception&> reason) {
    KJ_IF_SOME(r, reason) {
      rememberReason(r);
    }
    KJ_DASSERT(liveSources > 0);
    if (--liveSources == 0) {
      notifyOrigin();
    }
  }

  // The origin no longer needs to hear from this side: the stream was read to EOF, or the origin's
  // side failed.
  void originDone() {
    canceler = kj::none;
    serializedReason = kj::none;
  }

 private:
  kj::Maybe<rpc::StreamCanceler::Client> canceler;
  IoContext& ioctx;
  uint liveSources = 0;
  // The most recent reason a source was canceled with, serialized at the time (which needs the
  // isolate lock) and held until the last source is done and it can be sent.
  kj::Maybe<kj::Array<kj::byte>> serializedReason;

  // The reason can only be serialized when this runs under the isolate lock, as it does for a
  // JavaScript-initiated cancel(); KJ-side teardown supplies no reason and the origin synthesizes
  // one. The serialized reason is subject to the same size limit as any other value sent over JS
  // RPC; one that exceeds it is left out, and the origin synthesizes a reason for that case too.
  void rememberReason(kj::Exception& reason) {
    if (canceler == kj::none) return;
    KJ_IF_SOME(lock, ioctx.tryGetCurrentLock()) {
      jsg::Lock& js = lock;
      jsg::Serializer serializer(js);
      serializer.write(js, js.exceptionToJsValue(reason.clone()).getHandle(js));
      auto data = serializer.release().data;
      if (data.size() <= MAX_JS_RPC_MESSAGE_SIZE) {
        serializedReason = kj::mv(data);
      }
    }
  }

  // Sends the cancel to the origin (at most once).
  void notifyOrigin() {
    KJ_IF_SOME(c, canceler) {
      auto req = c.cancelRequest();
      KJ_IF_SOME(r, serializedReason) {
        req.getReason().setV8Serialized(r);
      }
      // Fire and forget: a failure here means the origin is already gone.
      req.send().detach([](kj::Exception&&) {});
      originDone();
    }
  }
};

// The receiving end of a ReadableStream transferred over RPC. Wraps the hydrated source and, when
// this side is done with the stream before reaching EOF -- cancel(), with its reason, or being
// dropped -- reports that to the shared RpcStreamCancelerState, which tells the origin so that it
// cancels its source promptly instead of finding out on its next write (which, for an idle source,
// may never come). Everything else forwards to the inner source; in particular pumpTo() and
// tryTee() forward so that the inner system stream's optimizations still apply, with each tee
// branch wrapped in another instance sharing the same state.
class RpcReadableStreamSource final: public ReadableStreamSource {
 public:
  RpcReadableStreamSource(kj::Own<ReadableStreamSource> inner, kj::Rc<RpcStreamCancelerState> state)
      : inner(kj::mv(inner)),
        state(kj::mv(state)) {
    this->state->sourceCreated();
  }

  ~RpcReadableStreamSource() noexcept(false) {
    reportDone(kj::none);
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return inner->tryRead(buffer, minBytes, maxBytes)
        .then([state = state.addRef(), minBytes](size_t amount) mutable {
      if (amount < minBytes) state->originDone();
      return amount;
    }, [state = state.addRef()](kj::Exception&& exception) mutable -> size_t {
      // The origin's side failed; there is nothing left to tell it.
      state->originDone();
      kj::throwFatalException(kj::mv(exception));
    });
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<kj::byte> buffer, size_t minBytes) override {
    KJ_IF_SOME(amount, inner->tryReadSync(buffer, minBytes)) {
      if (amount < minBytes) state->originDone();
      return amount;
    }
    return kj::none;
  }

  kj::Promise<DeferredProxy<void>> pumpTo(kj::Ptr<WritableStreamSink> output, bool end) override {
    // A pump that completes has read the stream to EOF. One that fails or is dropped leaves
    // the canceler in place: the pump's owner cancels the source in those cases, which reports
    // to the origin with the failure as the reason.
    return inner->pumpTo(kj::mv(output), end)
        .then([state = state.addRef()](DeferredProxy<void> proxy) mutable {
      proxy.proxyTask =
          proxy.proxyTask.then([state = kj::mv(state)]() mutable { state->originDone(); });
      return kj::mv(proxy);
    });
  }

  StreamEncoding getPreferredEncoding() override {
    return inner->getPreferredEncoding();
  }

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override {
    return inner->tryGetLength(encoding);
  }

  void cancel(kj::Exception reason) override {
    reportDone(reason);
    inner->cancel(kj::mv(reason));
  }

  kj::Maybe<Tee> tryTee(uint64_t limit) override {
    KJ_IF_SOME(tee, inner->tryTee(limit)) {
      // The branches take over the stream, each as another source sharing the state. This object's
      // controller destroys it next, and that report leaves the branches as the state's live
      // sources: the origin hears from this side only once the last of them is done before EOF.
      for (auto& branch: tee.branches) {
        branch = kj::heap<RpcReadableStreamSource>(kj::mv(branch), state.addRef());
      }
      return kj::mv(tee);
    }
    return kj::none;
  }

 private:
  kj::Own<ReadableStreamSource> inner;
  kj::Rc<RpcStreamCancelerState> state;
  // Whether this object has reported itself done to the state, which happens at most once: on
  // cancel(), or otherwise on destruction.
  bool reported = false;

  void reportDone(kj::Maybe<kj::Exception&> reason) {
    if (reported) return;
    reported = true;
    state->sourceDone(reason);
  }
};

// Constructs the source backing a ReadableStream received over RPC.
kj::Own<ReadableStreamSource> newRpcReadableStreamSource(
    IoContext& ioctx, rpc::JsValue::External::ReadableStream::Reader reader) {
  auto encoding = reader.getEncoding();

  KJ_REQUIRE(
      static_cast<uint>(encoding) < capnp::Schema::from<StreamEncoding>().getEnumerants().size(),
      "unknown StreamEncoding received from peer");

  kj::Own<kj::AsyncInputStream> in = ioctx.getExternalPusher()->unwrapStream(reader.getStream());

  kj::Own<ReadableStreamSource> source =
      kj::heap<NoDeferredProxyReadableStream>(newSystemStream(kj::mv(in), encoding, ioctx), ioctx);

  if (reader.hasCanceler() &&
      util::Autogate::isEnabled(util::AutogateKey::JSRPC_READABLE_CANCEL_PROPAGATION)) {
    source = kj::heap<RpcReadableStreamSource>(
        kj::mv(source), kj::rc<RpcStreamCancelerState>(reader.getCanceler(), ioctx));
  }
  return kj::mv(source);
}

}  // namespace

JsReadableStream hydrateRpcReadableStream(
    jsg::Lock& js, IoContext& ioctx, rpc::JsValue::External::ReadableStream::Reader reader) {
  // JsReadableStream::create() dispatches on the typescript_implemented_streams compat flag,
  // so the received stream is backed by whichever implementation this isolate runs.
  return JsReadableStream::create(js, ioctx, newRpcReadableStreamSource(ioctx, reader));
}

JsReadableStream ReadableStream::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  // No JavaScript may execute here: V8's deserializer forbids it for the duration of the value
  // graph read. Everything JS-executing happened in hydrateRpcReadableStream() during
  // RpcDeserializerExternalHandler::prepare(); this function only claims the result (or, when
  // the rpc-externals-hydration autogate is off, constructs the legacy stream in place, which
  // requires no JS).
  auto& handler = KJ_REQUIRE_NONNULL(
      deserializer.getExternalHandler(), "got ReadableStream on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHandler*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got ReadableStream on non-RPC serialized object?");

  KJ_IF_SOME(prebuilt, externalHandler->claimPrebuiltReadable()) {
    return kj::mv(prebuilt);
  }

  // Not hydrated: the rpc-externals-hydration autogate is off, which
  // RpcDeserializerExternalHandler::prepare() only permits for legacy-streams isolates (the
  // typescript_implemented_streams flag requires the gate), so constructing the legacy
  // stream in place -- which runs no JS -- is the only case here.
  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isReadableStream(), "external table slot type doesn't match serialization tag");

  auto& ioctx = IoContext::current();
  return JsReadableStream(js.alloc<ReadableStream>(
      ioctx, newRpcReadableStreamSource(ioctx, reader.getReadableStream())));
}

kj::StringPtr ReaderImpl::jsgGetMemoryName() const {
  return "ReaderImpl"_kjc;
}

size_t ReaderImpl::jsgGetMemorySelfSize() const {
  return sizeof(ReaderImpl);
}

void ReaderImpl::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(attached, state.tryGetActiveUnsafe()) {
    tracker.trackField("stream", attached.stream);
  }
  tracker.trackField("closedPromise", closedPromise);
}

void ReadableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("controller", controller);
  tracker.trackField("eofResolverPair", eofResolverPair);
}

}  // namespace workerd::api
