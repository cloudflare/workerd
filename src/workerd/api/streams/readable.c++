// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "readable.h"
#include "writable.h"
#include "internal.h"
#include <workerd/io/features.h>
#include <workerd/jsg/buffersource.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/api/system-streams.h>

namespace workerd::api {

ReaderImpl::ReaderImpl(ReadableStreamController::Reader& reader)
    : ioContext(tryGetIoContext()),
      reader(reader) {}

ReaderImpl::~ReaderImpl() noexcept(false) {
  KJ_IF_SOME(stream, state.tryGet<Attached>()) {
    stream->getController().releaseReader(reader, kj::none);
  }
}

void ReaderImpl::attach(ReadableStreamController& controller, jsg::Promise<void> closedPromise) {
  KJ_ASSERT(state.is<Initial>());
  state = controller.addRef();
  this->closedPromise = kj::mv(closedPromise);
}

void ReaderImpl::detach() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      // Do nothing in this case.
      return;
    }
    KJ_CASE_ONEOF(stream, Attached) {
      state.init<StreamStates::Closed>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      // Do nothing in this case.
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      // Do nothing in this case.
      return;
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> ReaderImpl::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      // In some edge cases, this reader is the last thing holding a strong
      // reference to the stream. Calling cancel might cause the readers strong
      // reference to be cleared, so let's make sure we keep a reference to
      // the stream at least until the call to cancel completes.
      auto ref = stream.addRef();
      return stream->getController().cancel(js, maybeReason);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This ReadableStream reader has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
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
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      KJ_IF_SOME(options, byobOptions) {
        // Per the spec, we must perform these checks before disturbing the stream.
        size_t atLeast = options.atLeast.orDefault(1);

        if (options.byteLength == 0) {
          return js.rejectedPromise<ReadResult>(
              js.v8TypeError("You must call read() on a \"byob\" reader with a positive-sized "
                             "TypedArray object."_kj));
        }
        if (atLeast == 0) {
          return js.rejectedPromise<ReadResult>(js.v8TypeError(
              kj::str("Requested invalid minimum number of bytes to read (", atLeast, ").")));
        }
        if (atLeast > options.byteLength) {
          return js.rejectedPromise<ReadResult>(js.v8TypeError(kj::str("Minimum bytes to read (",
              atLeast, ") exceeds size of buffer (", options.byteLength, ").")));
        }

        jsg::BufferSource source(js, options.bufferView.getHandle(js));
        options.atLeast = atLeast * source.getElementSize();
      }

      return KJ_ASSERT_NONNULL(stream->getController().read(js, kj::mv(byobOptions)));
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("This ReadableStream reader has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("This ReadableStream has been closed."_kj));
    }
  }
  KJ_UNREACHABLE;
}

void ReaderImpl::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending reads. This is a recent
  // modification to the spec that we have not yet implemented.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      // In some edge cases, this reader is the last thing holding a strong
      // reference to the stream. Calling releaseLock might cause the readers strong
      // reference to be cleared, so let's make sure we keep a reference to
      // the stream at least until the call to releaseLock completes.
      auto ref = stream.addRef();
      stream->getController().releaseReader(reader, js);
      state.init<Released>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      // Do nothing in this case.
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      // Do nothing in this case.
      return;
    }
  }
  KJ_UNREACHABLE;
}

void ReaderImpl::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(readable, state.tryGet<Attached>()) {
    visitor.visit(readable);
  }
  visitor.visit(closedPromise);
}

// ======================================================================================

ReadableStreamDefaultReader::ReadableStreamDefaultReader(): impl(*this) {}

jsg::Ref<ReadableStreamDefaultReader> ReadableStreamDefaultReader::constructor(
    jsg::Lock& js, jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(
      !stream->isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");
  auto reader = jsg::alloc<ReadableStreamDefaultReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamDefaultReader::attach(
    ReadableStreamController& controller, jsg::Promise<void> closedPromise) {
  impl.attach(controller, kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamDefaultReader::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
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

ReadableStreamBYOBReader::ReadableStreamBYOBReader(): impl(*this) {}

jsg::Ref<ReadableStreamBYOBReader> ReadableStreamBYOBReader::constructor(
    jsg::Lock& js, jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(
      !stream->isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");

  if (!stream->getController().isClosedOrErrored()) {
    JSG_REQUIRE(stream->getController().isByteOriented(), TypeError,
        "This ReadableStream does not support BYOB reads.");
  }

  auto reader = jsg::alloc<ReadableStreamBYOBReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamBYOBReader::attach(
    ReadableStreamController& controller, jsg::Promise<void> closedPromise) {
  impl.attach(controller, kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamBYOBReader::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
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
    v8::Local<v8::ArrayBufferView> byobBuffer,
    jsg::Optional<ReadableStreamBYOBReaderReadOptions> maybeOptions) {
  static const ReadableStreamBYOBReaderReadOptions defaultOptions{};
  auto options = ReadableStreamController::ByobOptions{
    .bufferView = js.v8Ref(byobBuffer),
    .byteOffset = byobBuffer->ByteOffset(),
    .byteLength = byobBuffer->ByteLength(),
    .atLeast = maybeOptions.orDefault(defaultOptions).min.orDefault(1),
    .detachBuffer = FeatureFlags::get(js).getStreamsByobReaderDetachesBuffer(),
  };
  return impl.read(js, kj::mv(options));
}

jsg::Promise<ReadResult> ReadableStreamBYOBReader::readAtLeast(
    jsg::Lock& js, int minBytes, v8::Local<v8::ArrayBufferView> byobBuffer) {
  auto options = ReadableStreamController::ByobOptions{
    .bufferView = js.v8Ref(byobBuffer),
    .byteOffset = byobBuffer->ByteOffset(),
    .byteLength = byobBuffer->ByteLength(),
    .atLeast = minBytes,
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

ReadableStream::ReadableStream(IoContext& ioContext, kj::Own<ReadableStreamSource> source)
    : ReadableStream(newReadableStreamInternalController(ioContext, kj::mv(source))) {}

ReadableStream::ReadableStream(kj::Own<ReadableStreamController> controller)
    : ioContext(tryGetIoContext()),
      controller(kj::mv(controller)) {
  getController().setOwnerRef(*this);
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

jsg::Promise<void> ReadableStream::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This ReadableStream is currently locked to a reader."_kj));
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
          mode == "byob", RangeError, "mode must be undefined or 'byob' in call to getReader().");
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
  return jsg::alloc<ReadableStreamAsyncIterator>(AsyncIteratorState{.ioContext = ioContext,
    .reader = ReadableStreamDefaultReader::constructor(js, JSG_THIS),
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
  controller.pipeTo(js, destination, kj::mv(options))
      .then(js,
          JSG_VISITABLE_LAMBDA(
              (self = JSG_THIS), (self), (jsg::Lock& js) { return js.resolvedPromise(); }),
          JSG_VISITABLE_LAMBDA((self = JSG_THIS), (self),
              (jsg::Lock& js, auto&& exception) {
                return js.rejectedPromise<void>(kj::mv(exception));
              }))
      .markAsHandled(js);
  return kj::mv(transform.readable);
}

jsg::Promise<void> ReadableStream::pipeTo(jsg::Lock& js,
    jsg::Ref<WritableStream> destination,
    jsg::Optional<PipeToOptions> maybeOptions) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This ReadableStream is currently locked to a reader."_kj));
  }

  if (destination->getController().isLockedToWriter()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer"_kj));
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

jsg::Promise<kj::Maybe<jsg::Value>> ReadableStream::nextFunction(
    jsg::Lock& js, AsyncIteratorState& state) {
  return state.reader->read(js).then(
      js, [reader = state.reader.addRef()](jsg::Lock& js, ReadResult result) mutable {
    if (result.done) {
      reader->releaseLock(js);
      return js.resolvedPromise(kj::Maybe<jsg::Value>(kj::none));
    }
    return js.resolvedPromise<kj::Maybe<jsg::Value>>(kj::mv(result.value));
  });
}

jsg::Promise<void> ReadableStream::returnFunction(
    jsg::Lock& js, AsyncIteratorState& state, jsg::Optional<jsg::Value> value) {
  if (state.reader.get() != nullptr) {
    auto reader = kj::mv(state.reader);
    if (!state.preventCancel) {
      auto promise = reader->cancel(js, value.map([&](jsg::Value& v) { return v.getHandle(js); }));
      reader->releaseLock(js);
      return promise.then(js,
          JSG_VISITABLE_LAMBDA((reader = kj::mv(reader)), (reader), (jsg::Lock& js) {
            // Ensure that the reader is not garbage collected until the cancel promise resolves.
            return js.resolvedPromise();
          }));
    }

    reader->releaseLock(js);
  }
  return js.resolvedPromise();
}

jsg::Ref<ReadableStream> ReadableStream::detach(jsg::Lock& js, bool ignoreDisturbed) {
  JSG_REQUIRE(
      !isDisturbed() || ignoreDisturbed, TypeError, "The ReadableStream has already been read.");
  JSG_REQUIRE(!isLocked(), TypeError, "The ReadableStream has been locked to a reader.");
  return jsg::alloc<ReadableStream>(getController().detach(js, ignoreDisturbed));
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
  auto stream = jsg::alloc<ReadableStream>(newReadableStreamJsController());
  stream->getController().setup(js, kj::mv(underlyingSource), kj::mv(queuingStrategy));
  return kj::mv(stream);
}

jsg::Optional<uint32_t> ByteLengthQueuingStrategy::size(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeValue) {
  KJ_IF_SOME(value, maybeValue) {
    if ((value)->IsArrayBuffer()) {
      auto buffer = value.As<v8::ArrayBuffer>();
      return buffer->ByteLength();
    } else if ((value)->IsArrayBufferView()) {
      auto view = value.As<v8::ArrayBufferView>();
      return view->ByteLength();
    }
  }
  return kj::none;
}

namespace {

// HACK: We need as async pipe, like kj::newOneWayPipe(), except supporting explicit end(). So we
//   wrap the two ends of the pipe in special adapters that track whether end() was called.
class ExplicitEndOutputPipeAdapter final: public capnp::ExplicitEndOutputStream {
public:
  ExplicitEndOutputPipeAdapter(
      kj::Own<kj::AsyncOutputStream> inner, kj::Own<kj::RefcountedWrapper<bool>> ended)
      : inner(kj::mv(inner)),
        ended(kj::mv(ended)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return KJ_REQUIRE_NONNULL(inner)->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return KJ_REQUIRE_NONNULL(inner)->write(pieces);
  }

  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount) override {
    return KJ_REQUIRE_NONNULL(inner)->tryPumpFrom(input, amount);
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return KJ_REQUIRE_NONNULL(inner)->whenWriteDisconnected();
  }

  kj::Promise<void> end() override {
    // Signal to the other side that end() was actually called.
    ended->getWrapped() = true;
    inner = kj::none;
    return kj::READY_NOW;
  }

private:
  kj::Maybe<kj::Own<kj::AsyncOutputStream>> inner;
  kj::Own<kj::RefcountedWrapper<bool>> ended;
};

class ExplicitEndInputPipeAdapter final: public kj::AsyncInputStream {
public:
  ExplicitEndInputPipeAdapter(kj::Own<kj::AsyncInputStream> inner,
      kj::Own<kj::RefcountedWrapper<bool>> ended,
      kj::Maybe<uint64_t> expectedLength)
      : inner(kj::mv(inner)),
        ended(kj::mv(ended)),
        expectedLength(expectedLength) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    size_t result = co_await inner->tryRead(buffer, minBytes, maxBytes);

    KJ_IF_SOME(l, expectedLength) {
      KJ_ASSERT(result <= l);
      l -= result;
      if (l == 0) {
        // If we got all the bytes we expected, we treat this as a successful end, because the
        // underlying KJ pipe is not actually going to wait for the other side to drop. This is
        // consistent with the behavior of Content-Length in HTTP anyway.
        ended->getWrapped() = true;
      }
    }

    if (result < minBytes) {
      // Verify that end() was called.
      if (!ended->getWrapped()) {
        JSG_FAIL_REQUIRE(Error, "ReadableStream received over RPC disconnected prematurely.");
      }
    }
    co_return result;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner->tryGetLength();
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    return inner->pumpTo(output, amount);
  }

private:
  kj::Own<kj::AsyncInputStream> inner;
  kj::Own<kj::RefcountedWrapper<bool>> ended;
  kj::Maybe<uint64_t> expectedLength;
};

// Wrapper around ReadableStreamSource that prevents deferred proxying. We need this for RPC
// streams because although they are "system streams", they become disconnected when the IoContext
// is destroyed, due to the JsRpcCustomEventImpl being canceled.
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

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override {
    // Move the deferred proxy part of the task over to the non-deferred part. To do this,
    // we use `ioctx.waitForDeferredProxy()`, which returns a single promise covering both parts
    // (and, importantly, registering pending events where needed). Then, we add a noop deferred
    // proxy to the end of that.
    return addNoopDeferredProxy(ioctx.waitForDeferredProxy(inner->pumpTo(output, end)));
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

void ReadableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // Serialize by effectively creating a `JsRpcStub` around this object and serializing that.
  // Except we don't actually want to do _exactly_ that, because we do not want to actually create
  // a `JsRpcStub` locally. So do the important parts of `JsRpcStub::constructor()` followed by
  // `JsRpcStub::serialize()`.

  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "ReadableStream can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "ReadableStream can only be serialized for RPC.");

  // NOTE: We're counting on `pumpTo()`, below, to check that the stream is not locked or disturbed
  //   and other common checks. It's important that we don't modify the stream in any way before
  //   that call.

  IoContext& ioctx = IoContext::current();

  auto& controller = getController();
  StreamEncoding encoding = controller.getPreferredEncoding();
  auto expectedLength = controller.tryGetLength(encoding);

  auto streamCap = externalHandler->writeStream(
      [encoding, expectedLength](rpc::JsValue::External::Builder builder) mutable {
    auto rs = builder.initReadableStream();
    rs.setEncoding(encoding);
    KJ_IF_SOME(l, expectedLength) {
      rs.getExpectedLength().setKnown(l);
    }
  });

  kj::Own<capnp::ExplicitEndOutputStream> kjStream =
      ioctx.getByteStreamFactory().capnpToKjExplicitEnd(
          kj::mv(streamCap).castAs<capnp::ByteStream>());

  auto sink = newSystemStream(kj::mv(kjStream), encoding, ioctx);

  ioctx.addTask(
      ioctx.waitForDeferredProxy(pumpTo(js, kj::mv(sink), true)).catch_([](kj::Exception&& e) {
    // Errors in pumpTo() are automatically propagated to the source and destination. We don't
    // want to throw them from here since it'll cause an uncaught exception to be reported, even
    // if the application actually does handle it!
  }));
}

jsg::Ref<ReadableStream> ReadableStream::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(
      deserializer.getExternalHandler(), "got ReadableStream on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHander*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got ReadableStream on non-RPC serialized object?");

  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isReadableStream(), "external table slot type doesn't match serialization tag");

  auto rs = reader.getReadableStream();
  auto encoding = rs.getEncoding();

  KJ_REQUIRE(
      static_cast<uint>(encoding) < capnp::Schema::from<StreamEncoding>().getEnumerants().size(),
      "unknown StreamEncoding received from peer");

  auto& ioctx = IoContext::current();

  kj::Maybe<uint64_t> expectedLength;
  auto el = rs.getExpectedLength();
  if (el.isKnown()) {
    expectedLength = el.getKnown();
  }

  auto pipe = kj::newOneWayPipe(expectedLength);

  auto endedFlag = kj::refcounted<kj::RefcountedWrapper<bool>>(false);

  auto out = kj::heap<ExplicitEndOutputPipeAdapter>(kj::mv(pipe.out), kj::addRef(*endedFlag));
  auto in =
      kj::heap<ExplicitEndInputPipeAdapter>(kj::mv(pipe.in), kj::mv(endedFlag), expectedLength);

  externalHandler->setLastStream(ioctx.getByteStreamFactory().kjToCapnp(kj::mv(out)));

  return jsg::alloc<ReadableStream>(ioctx,
      kj::heap<NoDeferredProxyReadableStream>(newSystemStream(kj::mv(in), encoding, ioctx), ioctx));
}

kj::StringPtr ReaderImpl::jsgGetMemoryName() const {
  return "ReaderImpl"_kjc;
}

size_t ReaderImpl::jsgGetMemorySelfSize() const {
  return sizeof(ReaderImpl);
}

void ReaderImpl::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(stream, state.tryGet<Attached>()) {
    tracker.trackField("stream", stream);
  }
  tracker.trackField("closedPromise", closedPromise);
}

void ReadableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("controller", controller);
  tracker.trackField("eofResolverPair", eofResolverPair);
}

}  // namespace workerd::api
