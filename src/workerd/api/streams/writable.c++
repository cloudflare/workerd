// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "writable.h"
#include <workerd/io/features.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/api/system-streams.h>

namespace workerd::api {

WritableStreamDefaultWriter::WritableStreamDefaultWriter()
    : ioContext(tryGetIoContext()) {}

WritableStreamDefaultWriter::~WritableStreamDefaultWriter() noexcept(false) {
  KJ_IF_SOME(stream, state.tryGet<Attached>()) {
    // Because this can be called during gc or other cleanup, it is important
    // that releasing the writer does not cause the closed promise be resolved
    // since that requires v8 heap allocations.
    stream->getController().releaseWriter(*this, kj::none);
  }
}

jsg::Ref<WritableStreamDefaultWriter> WritableStreamDefaultWriter::constructor(
    jsg::Lock& js,
    jsg::Ref<WritableStream> stream) {
  JSG_REQUIRE(!stream->isLocked(), TypeError,
               "This WritableStream is currently locked to a writer.");
  auto writer = jsg::alloc<WritableStreamDefaultWriter>();
  writer->lockToStream(js, *stream);
  return kj::mv(writer);
}

jsg::Promise<void> WritableStreamDefaultWriter::abort(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().abort(js, reason);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::attach(
    WritableStreamController& controller,
    jsg::Promise<void> closedPromise,
    jsg::Promise<void> readyPromise) {
  KJ_ASSERT(state.is<Initial>());
  state = controller.addRef();
  this->closedPromise = kj::mv(closedPromise);
  replaceReadyPromise(kj::mv(readyPromise));
}

jsg::Promise<void> WritableStreamDefaultWriter::close(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().close(js);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream has been closed."_kj));
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::detach() {
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

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamDefaultWriter::getClosed() {
  return KJ_ASSERT_NONNULL(closedPromise, "the writer was never attached to a stream");
}

kj::Maybe<int> WritableStreamDefaultWriter::getDesiredSize(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().getDesiredSize();
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(r, Released) {
      JSG_FAIL_REQUIRE(TypeError, "This WritableStream writer has been released.");
    }
  }
  KJ_UNREACHABLE;
}

jsg::MemoizedIdentity<jsg::Promise<void>>& WritableStreamDefaultWriter::getReady() {
  return KJ_ASSERT_NONNULL(readyPromise, "the writer was never attached to a stream");
}

void WritableStreamDefaultWriter::lockToStream(jsg::Lock& js, WritableStream& stream) {
  KJ_ASSERT(!stream.isLocked());
  KJ_ASSERT(stream.getController().lockWriter(js, *this));
}

void WritableStreamDefaultWriter::releaseLock(jsg::Lock& js) {
  // TODO(soon): Releasing the lock should cancel any pending writes.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      stream->getController().releaseWriter(*this, js);
      state.init<Released>();
      return;
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      // Do nothing in this case
      return;
    }
    KJ_CASE_ONEOF(r, Released) {
      // Do nothing in this case
      return;
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamDefaultWriter::replaceReadyPromise(jsg::Promise<void> readyPromise) {
  this->readyPromise = kj::mv(readyPromise);
}

jsg::Promise<void> WritableStreamDefaultWriter::write(jsg::Lock& js, v8::Local<v8::Value> chunk) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(i, Initial) {
      KJ_FAIL_ASSERT("this writer was never attached");
    }
    KJ_CASE_ONEOF(stream, Attached) {
      return stream->getController().write(js, chunk);
    }
    KJ_CASE_ONEOF(r, Released) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream writer has been released."_kj));
    }
    KJ_CASE_ONEOF(c, StreamStates::Closed) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream has been closed."_kj));
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsString WritableStream::inspectState(jsg::Lock& js) {
  if (controller->isErrored()) {
    return js.strIntern("errored");
  } else if (controller->isErroring(js) != kj::none) {
    return js.strIntern("erroring");
  } else if (controller->isClosedOrClosing()) {
    return js.strIntern("closed");
  } else {
    return js.strIntern("writable");
  }
}

bool WritableStream::inspectExpectsBytes() {
  return controller->isByteOriented();
}

void WritableStreamDefaultWriter::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(writable, state.tryGet<Attached>()) {
    visitor.visit(writable);
  }
  visitor.visit(closedPromise, readyPromise);
}

// ======================================================================================

WritableStream::WritableStream(
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<uint64_t> maybeHighWaterMark,
    kj::Maybe<jsg::Promise<void>> maybeClosureWaitable)
    : WritableStream(newWritableStreamInternalController(ioContext, kj::mv(sink),
        maybeHighWaterMark, kj::mv(maybeClosureWaitable))) {}

WritableStream::WritableStream(kj::Own<WritableStreamController> controller)
    : ioContext(tryGetIoContext()),
      controller(kj::mv(controller)) {
  getController().setOwnerRef(*this);
}

jsg::Ref<WritableStream> WritableStream::addRef() { return JSG_THIS; }

void WritableStream::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(getController());
}

bool WritableStream::isLocked() { return getController().isLockedToWriter(); }

WritableStreamController& WritableStream::getController() { return *controller; }

kj::Own<WritableStreamSink> WritableStream::removeSink(jsg::Lock& js) {
  return JSG_REQUIRE_NONNULL(
      getController().removeSink(js),
      TypeError,
      "This WritableStream does not have a WritableStreamSink");
}

jsg::Promise<void> WritableStream::abort(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().abort(js, reason);
}

jsg::Promise<void> WritableStream::close(jsg::Lock& js) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().close(js);
}

jsg::Promise<void> WritableStream::flush(jsg::Lock& js) {
  if (isLocked()) {
    return js.rejectedPromise<void>(
        js.v8TypeError("This WritableStream is currently locked to a writer."_kj));
  }
  return getController().flush(js);
}

jsg::Ref<WritableStreamDefaultWriter> WritableStream::getWriter(jsg::Lock& js) {
  return WritableStreamDefaultWriter::constructor(js, JSG_THIS);
}

jsg::Ref<WritableStream> WritableStream::constructor(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSink> underlyingSink,
    jsg::Optional<StreamQueuingStrategy> queuingStrategy) {
  JSG_REQUIRE(FeatureFlags::get(js).getStreamsJavaScriptControllers(),
               Error,
               "To use the new WritableStream() constructor, enable the "
               "streams_enable_constructors compatibility flag. "
               "Refer to the docs for more information: https://developers.cloudflare.com/workers/platform/compatibility-dates/#compatibility-flags");
  auto stream = jsg::alloc<WritableStream>(newWritableStreamJsController());
  stream->getController().setup(js, kj::mv(underlyingSink), kj::mv(queuingStrategy));
  return kj::mv(stream);
}

namespace {

// Wrapper around `WritableStreamSink` that makes it suitable for passing off to capnp RPC.
class WritableStreamRpcAdapter final: public capnp::ExplicitEndOutputStream {
public:
  WritableStreamRpcAdapter(kj::Own<WritableStreamSink> inner)
      : inner(kj::mv(inner)) {}
  ~WritableStreamRpcAdapter() noexcept(false) {
    weakRef->invalidate();
    doneFulfiller->fulfill();
  }

  // Returns a promise that resolves when the stream is dropped. If the promise is canceled before
  // that, the stream is revoked.
  kj::Promise<void> waitForCompletionOrRevoke() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    doneFulfiller = kj::mv(paf.fulfiller);

    return paf.promise.attach(kj::defer([weakRef = weakRef->addRef()]() mutable {
      KJ_IF_SOME(obj, weakRef->tryGet()) {
        // Stream is still alive, revoke it.
        if (!obj.canceler.isEmpty()) {
          obj.canceler.cancel(cancellationException());
        }
        obj.inner = kj::none;
      }
    }));
  }

  kj::Promise<void> write(const void* buffer, size_t size) override {
    return canceler.wrap(getInner().write(buffer, size));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return canceler.wrap(getInner().write(pieces));
  }

  // TODO(perf): We can't properly implement tryPumpFrom(), which means that Cap'n Proto will
  //   be unable to perform path shortening if the underlying stream turns out to be another capnp
  //   stream. This isn't a huge deal, but might be nice to enable someday. It may require
  //   significant refactoring of streams.

  kj::Promise<void> whenWriteDisconnected() override {
    // TODO(someday): WritableStreamSink doesn't give us a way to implement this.
    return kj::NEVER_DONE;
  }

  kj::Promise<void> end() override {
    return canceler.wrap(getInner().end());
  }

private:
  kj::Maybe<kj::Own<WritableStreamSink>> inner;
  kj::Canceler canceler;
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
  kj::Own<WeakRef<WritableStreamRpcAdapter>> weakRef =
      kj::refcounted<WeakRef<WritableStreamRpcAdapter>>(
          kj::Badge<WritableStreamRpcAdapter>(), *this);

  WritableStreamSink& getInner() {
    return *KJ_UNWRAP_OR(inner, {
      kj::throwFatalException(cancellationException());
    });
  }

  static kj::Exception cancellationException() {
    return JSG_KJ_EXCEPTION(DISCONNECTED, Error,
        "WritableStream received over RPC was disconnected because the remote execution context "
        "has endeded.");
  }
};

}  // namespace

void WritableStream::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  // Serialize by effectively creating a `JsRpcStub` around this object and serializing that.
  // Except we don't actually want to do _exactly_ that, because we do not want to actually create
  // a `JsRpcStub` locally. So do the important parts of `JsRpcStub::constructor()` followed by
  // `JsRpcStub::serialize()`.

  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "WritableStream can only be serialized for RPC.");
  auto externalHandler = dynamic_cast<RpcSerializerExternalHander*>(&handler);
  JSG_REQUIRE(externalHandler != nullptr, DOMDataCloneError,
      "WritableStream can only be serialized for RPC.");

  IoContext& ioctx = IoContext::current();

  // TODO(soon): Support JS-backed WritableStreams. Currently this only supports native streams
  //   and IdentityTransformStream, since only they are backed by WritableStreamSink.

  // NOTE: We're counting on `removeSink()`, to check that the stream is not locked and other
  //   common checks. It's important we don't modify the WritableStream before this call.
  auto sink = removeSink(js);
  auto encoding = sink->disownEncodingResponsibility();
  auto wrapper = kj::heap<WritableStreamRpcAdapter>(kj::mv(sink));

  // Make sure this stream will be revoked if the IoContext ends.
  ioctx.addTask(wrapper->waitForCompletionOrRevoke().attach(ioctx.registerPendingEvent()));

  auto capnpStream = ioctx.getByteStreamFactory().kjToCapnp(kj::mv(wrapper));

  externalHandler->write(
      [capnpStream = kj::mv(capnpStream), encoding]
      (rpc::JsValue::External::Builder builder) mutable {
    auto ws = builder.initWritableStream();
    ws.setByteStream(kj::mv(capnpStream));
    ws.setEncoding(encoding);
  });
}

jsg::Ref<WritableStream> WritableStream::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(deserializer.getExternalHandler(),
      "got WritableStream on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHander*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got WritableStream on non-RPC serialized object?");

  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isWritableStream(), "external table slot type doesn't match serialization tag");

  auto ws = reader.getWritableStream();
  auto encoding = ws.getEncoding();

  KJ_REQUIRE(static_cast<uint>(encoding) <
      capnp::Schema::from<StreamEncoding>().getEnumerants().size(),
      "unknown StreamEncoding received from peer");

  IoContext& ioctx = IoContext::current();
  auto stream = ioctx.getByteStreamFactory().capnpToKjExplicitEnd(ws.getByteStream());
  auto sink = newSystemStream(kj::mv(stream), encoding, ioctx);

  return jsg::alloc<WritableStream>(ioctx, kj::mv(sink));
}

void WritableStreamDefaultWriter::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(ref, state.tryGet<Attached>()) {
    tracker.trackField("attached", ref);
  }
  tracker.trackField("closedPromise", closedPromise);
  tracker.trackField("readyPromise", readyPromise);
}

void WritableStream::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("controller", controller);
}

}  // namespace workerd::api
