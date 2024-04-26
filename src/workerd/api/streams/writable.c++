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
      // In some edge cases, this writer is the last thing holding a strong
      // reference to the stream. Calling abort can cause the writers strong
      // reference to be cleared, so let's make sure we keep a reference to
      // the stream at least until the call to abort completes.
      auto ref = stream.addRef();
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
      // In some edge cases, this writer is the last thing holding a strong
      // reference to the stream. Calling close can cause the writers strong
      // reference to be cleared, so let's make sure we keep a reference to
      // the stream at least until the call to close completes.
      auto ref = stream.addRef();
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
      // In some edge cases, this writer is the last thing holding a strong
      // reference to the stream. Calling releaseWriter can cause the writers
      // strong reference to be cleared, so let's make sure we keep a reference
      // to the stream at least until the call to releaseLock completes.
      auto ref = stream.addRef();
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

void WritableStream::detach(jsg::Lock& js) {
  getController().detach(js);
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

// In order to support JavaScript-backed WritableStreams that do not have a backing
// WritableStreamSink, we need an alternative version of the WritableStreamRpcAdapter
// that will arrange to acquire the isolate lock when necessary to perform writes
// directly on the WritableStreamController. Note that this approach is necessarily
// a lot slower
class WritableStreamJsRpcAdapter final: public capnp::ExplicitEndOutputStream {
public:
  WritableStreamJsRpcAdapter(IoContext& context, jsg::Ref<WritableStreamDefaultWriter> writer)
      : context(context), writer(kj::mv(writer)) {}

  ~WritableStreamJsRpcAdapter() noexcept(false) {
    weakRef->invalidate();
    doneFulfiller->fulfill();

    // If the stream was not explicitly ended and the writer still exists at this point,
    // then we should trigger calling the abort algorithm on the stream. Sadly, there's a
    // bit of an incompetibility with kj::AsyncOutputStream and the standard definition of
    // WritableStream in that AsyncOutputStream has no specific way to explicitly signal that
    // the stream is being aborted due to a particular reason.
    //
    // On the remote side, because it is using a WritableStreamSink implementation, when that
    // side is aborted, all it does is record the reason and drop the stream. It does not
    // propagate the reason back to this side. So, we have to do the best we can here. Our
    // assumption is that once the stream is dropped, if it has not been explicitly ended and
    // the writer still exists, then the writer should be aborted. This is not perfect because
    // we cannot propgate the actual reason why it was aborted.
    //
    // Note also that there is no guarantee that the abort will actually run if the context
    // is being torn down. Some WritableStream implementations might use the abort algorithm
    // to clean things up or perform logging in the case of an error. Care needs to be taken
    // in this situation or the user code might end up with bugs. Need to see if there's a
    // better solution.
    //
    // TODO(someday): If the remote end can be updated to propagate the abort, then we can
    // hopefully improve the situation here.
    if (!ended) {
      KJ_IF_SOME(writer, this->writer) {
        context.addTask(context.run(
            [writer=kj::mv(writer), exception=cancellationException()]
            (Worker::Lock& lock) mutable {
          jsg::Lock& js = lock;
          auto ex = js.exceptionToJs(kj::mv(exception));
          return IoContext::current().awaitJs(lock,
              writer->abort(lock, ex.getHandle(js)));
        }));
      }
    }
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
        auto w = kj::mv(obj.writer);
        KJ_IF_SOME(writer, w) {
          obj.context.addTask(obj.context.run(
              [writer=kj::mv(writer), exception=cancellationException()]
              (Worker::Lock& lock) mutable {
            jsg::Lock& js = lock;
            auto ex = js.exceptionToJs(kj::mv(exception));
            return IoContext::current().awaitJs(lock,
                writer->abort(lock, ex.getHandle(js)));
          }));
        }
      }
    }));
  }

  kj::Promise<void> write(const void* buffer, size_t size) override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "Write after stream has been closed.");
    }
    if (size == 0) return kj::READY_NOW;
    kj::ArrayPtr<const kj::byte> ptr(reinterpret_cast<const kj::byte*>(buffer), size);
    return canceler.wrap(context.run([this, ptr](Worker::Lock& lock) mutable {
      auto& writer = getInner();
      auto source = KJ_ASSERT_NONNULL(jsg::BufferSource::tryAlloc(lock, ptr.size()));
      source.asArrayPtr().copyFrom(ptr);
      return context.awaitJs(lock, writer.write(lock, source.getHandle(lock)));
    }));
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "Write after stream has been closed.");
    }
    auto amount = 0;
    for (auto& piece : pieces) {
      amount += piece.size();
    }
    if (amount == 0) return kj::READY_NOW;
    return canceler.wrap(context.run([this, amount, pieces](Worker::Lock& lock) mutable {
      auto& writer = getInner();
      // Sadly, we have to allocate and copy here. Our received set of buffers are only
      // guaranteed to live until the returned promise is resolved, but the application code
      // may hold onto the ArrayBuffer for longer. We need to make sure that the backing store
      // for the ArrayBuffer remains valid.
      auto source = KJ_ASSERT_NONNULL(jsg::BufferSource::tryAlloc(lock, amount));
      auto ptr = source.asArrayPtr();
      for (auto& piece : pieces) {
        KJ_DASSERT(ptr.size() > 0);
        KJ_DASSERT(piece.size() <= ptr.size());
        if (piece.size() == 0) continue;
        ptr.slice(0, piece.size()).copyFrom(piece);
        ptr = ptr.slice(piece.size());
      }

      return context.awaitJs(lock, writer.write(lock, source.getHandle(lock)));
    }));
  }

  // TODO(perf): We can't properly implement tryPumpFrom(), which means that Cap'n Proto will
  //   be unable to perform path shortening if the underlying stream turns out to be another capnp
  //   stream. This isn't a huge deal, but might be nice to enable someday. It may require
  //   significant refactoring of streams.

  kj::Promise<void> whenWriteDisconnected() override {
    // TODO(soon): We might be able to support this by following the writer.closed promise,
    // which becomes resolved when the writer is used to close the stream, or rejects when
    // the stream has errored. However, currently, we don't have an easy way to do this.
    //
    // The Writer's getClosed() method returns a jsg::MemoizedIdentity<jsg::Promise<void>>.
    // jsg::MemoizedIdentity lazily converts the jsg::Promise into a v8::Promise once it
    // passes through the type wrapper. It does not give us any way to consistently get
    // at the underlying jsg::Promise<void> or the mapped v8::Promise. We would need to
    // capture a TypeHandler in here and convert each time to one or the other, then
    // attach our continuation. It's doable but a bit of a pain.
    //
    // For now, let's handle this the same as WritableStreamRpcAdapter and just return a
    // never done.
    return kj::NEVER_DONE;
  }

  kj::Promise<void> end() override {
    if (writer == kj::none) {
      return KJ_EXCEPTION(FAILED, "End after stream has been closed.");
    }
    ended = true;
    return canceler.wrap(context.run([this](Worker::Lock& lock) mutable {
      return context.awaitJs(lock, getInner().close(lock));
    }));
  }

private:
  IoContext& context;
  kj::Maybe<jsg::Ref<WritableStreamDefaultWriter>> writer;
  kj::Canceler canceler;
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
  kj::Own<WeakRef<WritableStreamJsRpcAdapter>> weakRef =
      kj::refcounted<WeakRef<WritableStreamJsRpcAdapter>>(
          kj::Badge<WritableStreamJsRpcAdapter>(), *this);
  bool ended = false;

  WritableStreamDefaultWriter& getInner() {
    KJ_IF_SOME(inner, writer) {
      return *inner;
    }
    kj::throwFatalException(cancellationException());
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

  KJ_IF_SOME(sink, getController().removeSink(js)) {
    // NOTE: We're counting on `removeSink()`, to check that the stream is not locked and other
    //   common checks. It's important we don't modify the WritableStream before this call.
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
  } else {
    // TODO(soon): Support disownEncodingResponsibility with JS-backed streams

    // NOTE: We're counting on `getWriter()` to check that the stream is not locked and other
    // common checks. It's important we don't modify the WritableStream before this call.
    auto wrapper = kj::heap<WritableStreamJsRpcAdapter>(ioctx, getWriter(js));

    // Make sure this stream will be revoked if the IoContext ends.
    ioctx.addTask(wrapper->waitForCompletionOrRevoke().attach(ioctx.registerPendingEvent()));

    auto capnpStream = ioctx.getByteStreamFactory().kjToCapnp(kj::mv(wrapper));

    externalHandler->write(
        [capnpStream = kj::mv(capnpStream)]
        (rpc::JsValue::External::Builder builder) mutable {
      auto ws = builder.initWritableStream();
      ws.setByteStream(kj::mv(capnpStream));
      ws.setEncoding(StreamEncoding::IDENTITY);
    });
  }
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
