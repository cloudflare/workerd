// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"

#include <workerd/util/weak-refs.h>

namespace workerd::api {

class WritableStreamDefaultWriter: public jsg::Object, public WritableStreamController::Writer {
public:
  explicit WritableStreamDefaultWriter();

  ~WritableStreamDefaultWriter() noexcept(false) override;

  // JavaScript API

  static jsg::Ref<WritableStreamDefaultWriter> constructor(
      jsg::Lock& js, jsg::Ref<WritableStream> stream);

  jsg::MemoizedIdentity<jsg::Promise<void>>& getClosed();
  jsg::MemoizedIdentity<jsg::Promise<void>>& getReady();
  kj::Maybe<int> getDesiredSize(jsg::Lock& js);

  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  // Closes the stream. All present write requests will complete, but future write requests will
  // be rejected with a TypeError to the effect of "This writable stream has been closed."
  // `reason` will be passed to the underlying sink's close algorithm -- if this writable stream
  // is one side of a transform stream, then its close algorithm causes the transform's readable
  // side to become closed.
  //
  // Note: According to my reading of the Streams spec, if `writer.close()` is called on a
  //   transform stream while the readable side has readable chunks in its queue, those chunks get
  //   lost. This seems like a bug to me. Why would we wait for all present write requests to
  //   complete on this side if we don't care that they're actually read?
  jsg::Promise<void> close(jsg::Lock& js);

  jsg::Promise<void> write(jsg::Lock& js, v8::Local<v8::Value> chunk);
  void releaseLock(jsg::Lock& js);

  JSG_RESOURCE_TYPE(WritableStreamDefaultWriter, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(closed, getClosed);
      JSG_READONLY_PROTOTYPE_PROPERTY(ready, getReady);
      JSG_READONLY_PROTOTYPE_PROPERTY(desiredSize, getDesiredSize);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(closed, getClosed);
      JSG_READONLY_INSTANCE_PROPERTY(ready, getReady);
      JSG_READONLY_INSTANCE_PROPERTY(desiredSize, getDesiredSize);
    }
    JSG_METHOD(abort);
    JSG_METHOD(close);
    JSG_METHOD(write);
    JSG_METHOD(releaseLock);

    JSG_TS_OVERRIDE(<W = any> {
      write(chunk?: W): Promise<void>;
    });
  }

  // Internal API

  void attach(WritableStreamController& controller,
      jsg::Promise<void> closedPromise,
      jsg::Promise<void> readyPromise) override;

  void detach() override;

  void lockToStream(jsg::Lock& js, WritableStream& stream);

  void replaceReadyPromise(jsg::Promise<void> readyPromise) override;

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  struct Initial {};
  // While a Writer is attached to a WritableStream, it holds a strong reference to the
  // WritableStream to prevent it from being GC'd so long as the Writer is available.
  // Once the writer is closed, released, or GC'd the reference to the WritableStream
  // is cleared and the WritableStream can be GC'd if there are no other references to
  // it being held anywhere. If the writer is still attached to the WritableStream when
  // it is destroyed, the WritableStream's reference to the writer is cleared but the
  // WritableStream remains in the "writer locked" state, per the spec.
  using Attached = jsg::Ref<WritableStream>;
  struct Released {};

  kj::Maybe<IoContext&> ioContext;
  kj::OneOf<Initial, Attached, Released, StreamStates::Closed> state = Initial();

  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> closedPromise;
  kj::Maybe<jsg::MemoizedIdentity<jsg::Promise<void>>> readyPromise;

  void visitForGc(jsg::GcVisitor& visitor);
};

class WritableStream: public jsg::Object {
public:
  explicit WritableStream(IoContext& ioContext,
      kj::Own<WritableStreamSink> sink,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none);

  explicit WritableStream(kj::Own<WritableStreamController> controller);
  ~WritableStream() noexcept(false) {
    weakRef->invalidate();
  }

  WritableStreamController& getController();

  jsg::Ref<WritableStream> addRef();

  // Remove and return the underlying implementation of this WritableStream. Throw a TypeError if
  // this WritableStream is locked or closed, otherwise this WritableStream becomes immediately
  // locked and closed. If this writable stream is errored, throw the stored error.
  // TODO(cleanup): There are a couple of places where we need to convert to using detach()
  // or the inner removeSink (on WritableStreamController) before we can remove this method.
  virtual KJ_DEPRECATED("Use detach() instead") kj::Own<WritableStreamSink> removeSink(
      jsg::Lock& js);
  virtual void detach(jsg::Lock& js);

  // ---------------------------------------------------------------------------
  // JS interface

  static jsg::Ref<WritableStream> constructor(jsg::Lock& js,
      jsg::Optional<UnderlyingSink> underlyingSink,
      jsg::Optional<StreamQueuingStrategy> queuingStrategy);

  bool isLocked();

  // Errors the stream. All present and future read requests are rejected with a TypeError to the
  // effect of "This writable stream has been requested to abort." `reason` will be passed to the
  // underlying sink's abort algorithm -- if this writable stream is one side of a transform stream,
  // then its abort algorithm causes the transform's readable side to become errored with `reason`.
  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  jsg::Promise<void> close(jsg::Lock& js);
  jsg::Promise<void> flush(jsg::Lock& js);

  jsg::Ref<WritableStreamDefaultWriter> getWriter(jsg::Lock& js);

  jsg::JsString inspectState(jsg::Lock& js);
  bool inspectExpectsBytes();

  JSG_RESOURCE_TYPE(WritableStream, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(locked, isLocked);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(locked, isLocked);
    }
    JSG_METHOD(abort);
    JSG_METHOD(close);
    JSG_METHOD(getWriter);

    JSG_INSPECT_PROPERTY(state, inspectState);
    JSG_INSPECT_PROPERTY(expectsBytes, inspectExpectsBytes);

    JSG_TS_OVERRIDE(<W = any> {
      getWriter(): WritableStreamDefaultWriter<W>;
    });
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);
  static jsg::Ref<WritableStream> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::WRITABLE_STREAM);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  kj::Maybe<IoContext&> ioContext;
  kj::Own<WritableStreamController> controller;
  kj::Own<WeakRef<WritableStream>> weakRef =
      kj::refcounted<WeakRef<WritableStream>>(kj::Badge<WritableStream>(), *this);

  kj::Own<WeakRef<WritableStream>> addWeakRef() {
    return weakRef->addRef();
  }

  void visitForGc(jsg::GcVisitor& visitor);

  template <typename T>
  friend class WritableImpl;
};

}  // namespace workerd::api
