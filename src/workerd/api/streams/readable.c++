// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "readable.h"
#include "writable.h"

namespace workerd::api {

ReaderImpl::ReaderImpl(ReadableStreamController::Reader& reader) : reader(reader) {}

ReaderImpl::~ReaderImpl() noexcept(false) {
  KJ_IF_MAYBE(stream, state.tryGet<Attached>()) {
    // There's a very good likelihood that this is called during GC or other
    // cleanup so we have to make sure that releasing the reader does not also
    // trigger resolution of the close promise.
    (*stream)->getController().releaseReader(reader, nullptr);
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
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
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
    jsg::Lock& js,
    kj::Maybe<ReadableStreamController::ByobOptions> byobOptions) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      KJ_IF_MAYBE(options, byobOptions) {
        // Per the spec, we must perform these checks before disturbing the stream.
        size_t atLeast = options->atLeast.orDefault(1);

        if (options->byteLength == 0) {
          return js.rejectedPromise<ReadResult>(
              js.v8TypeError(
                  "You must call read() on a \"byob\" reader with a positive-sized "
                  "TypedArray object."_kj));
        }
        if (atLeast == 0) {
          return js.rejectedPromise<ReadResult>(
              js.v8TypeError(kj::str(
                  "Requested invalid minimum number of bytes to read (", atLeast, ").")));
        }
        if (atLeast > options->byteLength) {
          return js.rejectedPromise<ReadResult>(
              js.v8TypeError(kj::str(
                  "Minimum bytes to read (", atLeast,
                  ") exceeds size of buffer (", options->byteLength, ").")));
        }

        jsg::BufferSource source(js, options->bufferView.getHandle(js));
        options->atLeast = atLeast * source.getElementSize();
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
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this reader was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
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
  visitor.visit(closedPromise);
}

// ======================================================================================

ReadableStreamDefaultReader::ReadableStreamDefaultReader() : impl(*this) {}

jsg::Ref<ReadableStreamDefaultReader> ReadableStreamDefaultReader::constructor(
    jsg::Lock& js,
    jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError,
                "This ReadableStream is currently locked to a reader.");
  auto reader = jsg::alloc<ReadableStreamDefaultReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamDefaultReader::attach(
    ReadableStreamController& controller,
    jsg::Promise<void> closedPromise){
  impl.attach(controller, kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamDefaultReader::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
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
  return impl.read(js, nullptr);
}

void ReadableStreamDefaultReader::releaseLock(jsg::Lock& js) {
  impl.releaseLock(js);
}

void ReadableStreamDefaultReader::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

// ======================================================================================

ReadableStreamBYOBReader::ReadableStreamBYOBReader() : impl(*this) {}

jsg::Ref<ReadableStreamBYOBReader> ReadableStreamBYOBReader::constructor(
    jsg::Lock& js,
    jsg::Ref<ReadableStream> stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError,
                "This ReadableStream is currently locked to a reader.");

  if (!stream->getController().isClosedOrErrored()) {
    JSG_REQUIRE(stream->getController().isByteOriented(), TypeError,
                "This ReadableStream does not support BYOB reads.");
  }

  auto reader = jsg::alloc<ReadableStreamBYOBReader>();
  reader->lockToStream(js, *stream);
  return kj::mv(reader);
}

void ReadableStreamBYOBReader::attach(
    ReadableStreamController& controller,
    jsg::Promise<void> closedPromise) {
  impl.attach(controller, kj::mv(closedPromise));
}

jsg::Promise<void> ReadableStreamBYOBReader::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
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

jsg::Promise<ReadResult> ReadableStreamBYOBReader::read(
    jsg::Lock& js,
    v8::Local<v8::ArrayBufferView> byobBuffer,
    CompatibilityFlags::Reader featureFlags) {
  auto options = ReadableStreamController::ByobOptions {
    .bufferView = js.v8Ref(byobBuffer),
    .byteOffset = byobBuffer->ByteOffset(),
    .byteLength = byobBuffer->ByteLength(),
    .atLeast = 1,
    .detachBuffer = featureFlags.getStreamsByobReaderDetachesBuffer(),
  };
  return impl.read(js, kj::mv(options));
}

jsg::Promise<ReadResult> ReadableStreamBYOBReader::readAtLeast(
    jsg::Lock& js,
    int minBytes,
    v8::Local<v8::ArrayBufferView> byobBuffer) {
  auto options = ReadableStreamController::ByobOptions {
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

ReadableStream::ReadableStream(
    IoContext& ioContext,
    kj::Own<ReadableStreamSource> source)
    : controller(kj::heap<ReadableStreamInternalController>(ioContext.addObject(kj::mv(source)))) {
  getController().setOwnerRef(*this);
}

ReadableStream::ReadableStream(Controller controller) : controller(kj::mv(controller)) {
  getController().setOwnerRef(*this);
}

ReadableStreamController& ReadableStream::getController() {
  KJ_SWITCH_ONEOF(controller) {
    KJ_CASE_ONEOF(c, kj::Own<ReadableStreamInternalController>) {
      return *c;
    }
    KJ_CASE_ONEOF(c, kj::Own<ReadableStreamJsController>) {
      return *c;
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> ReadableStream::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  if (getController().isLockedToReader()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This ReadableStream is currently locked to a reader."_kj));
  }
  return getController().cancel(js, maybeReason);
}

ReadableStream::Reader ReadableStream::getReader(
    jsg::Lock& js,
    jsg::Optional<GetReaderOptions> options) {
  JSG_REQUIRE(!isLocked(), TypeError, "This ReadableStream is currently locked to a reader.");

  bool isByob = false;
  KJ_IF_MAYBE(o, options) {
    KJ_IF_MAYBE(mode, o->mode) {
      JSG_REQUIRE(*mode == "byob", RangeError,
          "mode must be undefined or 'byob' in call to getReader().");
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
    jsg::Lock& js,
    jsg::Optional<ValuesOptions> options) {
  static auto defaultOptions = ValuesOptions {};
  return jsg::alloc<ReadableStreamAsyncIterator>(AsyncIteratorState {
    .reader = ReadableStreamDefaultReader::constructor(js, JSG_THIS),
    .preventCancel = options.orDefault(defaultOptions).preventCancel.orDefault(false)
  });
}

jsg::Ref<ReadableStream> ReadableStream::pipeThrough(
    jsg::Lock& js,
    Transform transform,
    jsg::Optional<PipeToOptions> maybeOptions) {
  auto& controller = getController();
  auto& destination = transform.writable->getController();
  JSG_REQUIRE(!controller.isLockedToReader(), TypeError,
               "This ReadableStream is currently locked to a reader.");
  JSG_REQUIRE(!destination.isLockedToWriter(), TypeError,
               "This WritableStream is currently locked to a writer.");

  auto options = kj::mv(maybeOptions).orDefault({});
  options.pipeThrough = true;
  maybePipeThrough = controller.pipeTo(js, destination, kj::mv(options)).then(js,
      JSG_VISITABLE_LAMBDA((this, self = JSG_THIS), (self), (jsg::Lock& js) {
    maybePipeThrough = nullptr;
    return js.resolvedPromise();
  }), JSG_VISITABLE_LAMBDA((this, self = JSG_THIS), (self), (jsg::Lock& js, auto&& exception) {
    maybePipeThrough = nullptr;
    return js.rejectedPromise<void>(kj::mv(exception));
  }));
  KJ_ASSERT_NONNULL(maybePipeThrough).markAsHandled();
  return kj::mv(transform.readable);
}

jsg::Promise<void> ReadableStream::pipeTo(
    jsg::Lock& js,
    jsg::Ref<WritableStream> destination,
    jsg::Optional<PipeToOptions> maybeOptions) {
  if (getController().isLockedToReader()) {
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

jsg::Promise<kj::Maybe<jsg::Value>> ReadableStream::nextFunction(
    jsg::Lock& js,
    AsyncIteratorState& state) {
  return state.reader->read(js).then(js,
      [reader = state.reader.addRef()](jsg::Lock& js, ReadResult result) mutable {
    if (result.done) {
      reader->releaseLock(js);
      return js.resolvedPromise(kj::Maybe<jsg::Value>(nullptr));
    }
    return js.resolvedPromise<kj::Maybe<jsg::Value>>(kj::mv(result.value));
  });
}

jsg::Promise<void> ReadableStream::returnFunction(
    jsg::Lock& js,
    AsyncIteratorState& state,
    jsg::Optional<jsg::Value> value) {
  if (state.reader.get() != nullptr) {
    auto reader = kj::mv(state.reader);
    if (!state.preventCancel) {
      auto promise = reader->cancel(js, value.map([&](jsg::Value& v) {
        return v.getHandle(js);
      }));
      reader->releaseLock(js);
      return promise.then(js, JSG_VISITABLE_LAMBDA((reader = kj::mv(reader)),
                                                    (reader), (jsg::Lock& js) {
        // Ensure that the reader is not garbage collected until the cancel promise resolves.
        return js.resolvedPromise();
      }));
    }

    reader->releaseLock(js);
  }
  return js.resolvedPromise();
}

jsg::Ref<ReadableStream> ReadableStream::detach(jsg::Lock& js) {
  JSG_REQUIRE(!isDisturbed(), TypeError, "The ReadableStream has already been read.");
  JSG_REQUIRE(!isLocked(), TypeError, "The ReadableStream has been locked to a reader.");
  KJ_SWITCH_ONEOF(controller) {
    KJ_CASE_ONEOF(c, kj::Own<ReadableStreamInternalController>) {
      return jsg::alloc<ReadableStream>(IoContext::current(),
          KJ_REQUIRE_NONNULL(c->removeSource(js)));
    }
    KJ_CASE_ONEOF(c, kj::Own<ReadableStreamJsController>) {
      auto stream = jsg::alloc<ReadableStream>(c->detach(js));
      return kj::mv(stream);
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<uint64_t> ReadableStream::tryGetLength(StreamEncoding encoding) {
  return getController().tryGetLength(encoding);
}

kj::Promise<DeferredProxy<void>> ReadableStream::pumpTo(
    jsg::Lock& js,
    kj::Own<WritableStreamSink> sink,
    bool end) {
  return kj::evalNow([&]() -> kj::Promise<DeferredProxy<void>> {
    KJ_REQUIRE(!isDisturbed(), "The ReadableStream has already been read.");
    KJ_REQUIRE(!isLocked(), "The ReadableStream has been locked to a reader.");

    KJ_SWITCH_ONEOF(controller) {
      KJ_CASE_ONEOF(c, kj::Own<ReadableStreamInternalController>) {
        auto source = KJ_ASSERT_NONNULL(c->removeSource(js));

        struct Holder: public kj::Refcounted {
          kj::Own<WritableStreamSink> sink;
          kj::Own<ReadableStreamSource> source;
          Holder(kj::Own<WritableStreamSink> sink,
                 kj::Own<ReadableStreamSource> source)
              : sink(kj::mv(sink)), source(kj::mv(source)) {}
        };

        auto holder = kj::refcounted<Holder>(kj::mv(sink), kj::mv(source));
        return holder->source->pumpTo(*holder->sink, end).then(
            [&holder=*holder](DeferredProxy<void> proxy) mutable -> DeferredProxy<void> {
          proxy.proxyTask = proxy.proxyTask.attach(kj::addRef(holder));
          return kj::mv(proxy);
        }, [&holder=*holder](kj::Exception&& ex) mutable {
          holder.sink->abort(kj::cp(ex));
          holder.source->cancel(kj::cp(ex));
          return kj::mv(ex);
        }).attach(kj::mv(holder));
      }
      KJ_CASE_ONEOF(c, kj::Own<ReadableStreamJsController>) {
        // It is important to note that the JavaScript-backed streams do not support
        // the deferred proxy optimization.
        return addNoopDeferredProxy(c->pumpTo(js, kj::mv(sink), end));
      }
    }
    KJ_UNREACHABLE;
  });
}

jsg::Ref<ReadableStream> ReadableStream::constructor(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSource> underlyingSource,
    jsg::Optional<StreamQueuingStrategy> queuingStrategy,
    CompatibilityFlags::Reader flags) {

  JSG_REQUIRE(flags.getStreamsJavaScriptControllers(),
               Error,
               "To use the new ReadableStream() constructor, enable the "
               "streams_enable_constructors feature flag.");

  auto stream = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>());
  static_cast<ReadableStreamJsController&>(
      stream->getController()).setup(js, kj::mv(underlyingSource), kj::mv(queuingStrategy));
  return kj::mv(stream);
}

jsg::Optional<uint32_t> ByteLengthQueuingStrategy::size(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeValue) {
  KJ_IF_MAYBE(value, maybeValue) {
    if ((*value)->IsArrayBuffer()) {
      auto buffer = value->As<v8::ArrayBuffer>();
      return buffer->ByteLength();
    } else if ((*value)->IsArrayBufferView()) {
      auto view = value->As<v8::ArrayBufferView>();
      return view->ByteLength();
    }
  }
  return nullptr;
}

}  // namespace workerd::api
