// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include <workerd/io/io-context.h>

namespace workerd::api {

using kj::byte;

// ReadableStreamSource and WritableStreamSink
//
// These are implementation interfaces for ReadableStream and WritableStream. If you just need to
// use a ReadableStream or WritableStream, you can safely skip reading this. If you need to
// implement a new kind of stream, read on.

// In the original Workers streams implementation, a ReadableStream would have a
// ReadableStreamSource backing it. Likewise, a WritableStream would have a WritableStreamSink.
// The ReadableStreamSource and WritableStreamSink are kj heap objects that provide a thin
// wrapper on internal native stream sources originating from within the Workers runtime.
//
// With implementation of full streams standard support, we introduce the new abstraction APIs
// ReadableStreamController and WritableStreamController, which will provide the underlying
// implementation for both ReadableStream and WritableStream, respectively.
//
// When creating a new kind of *internal* ReadableStream, where the data is originating internally
// from a kj stream, you will still implement the ReadableStreamSource API, just as before.
// Likewise, when creating a new kind of *internal* WritableStream, where the data destination is
// a kj stream, you will implement the WritableStreamSink API.

class ReadableStreamSource {
public:
  virtual kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) = 0;

  virtual kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end);
  // The ReadableStreamSource version of pumpTo() has no `amount` parameter, since the Streams spec
  // only defines pumping everything.
  //
  // If `end` is true, then `output.end()` will be called after pumping. Note that it's especially
  // important to take advantage of this when using deferred proxying since calling `end()`
  // directly might attempt to use the `IoContext` to call `registerPendingEvent()`.

  virtual kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

  kj::Promise<kj::Array<byte>> readAllBytes(uint64_t limit);
  kj::Promise<kj::String> readAllText(uint64_t limit);

  virtual void cancel(kj::Exception reason);
  // Hook to inform this ReadableStreamSource that the ReadableStream has been canceled. This only
  // really means anything to TransformStreams, which are supposed to propagate the error to the
  // writable side, and custom ReadableStreams, which we don't implement yet.
  //
  // NOTE: By "propagate the error back to the writable stream", I mean: if the WritableStream is in
  //   the Writable state, set it to the Errored state and reject its closed fulfiller with
  //   `reason`. I'm not sure how I'm going to do this yet.
  //
  // TODO(conform): Should return promise.
  //
  // TODO(conform): `reason` should be allowed to be any JS value, and not just an exception.
  //   That is, something silly like `stream.cancel(42)` should be allowed and trigger a
  //   rejection with the integer `42`.

  struct Tee {
    kj::Own<ReadableStreamSource> branches[2];
  };

  virtual kj::Maybe<Tee> tryTee(uint64_t limit);
  // Implement this if your ReadableStreamSource has a better way to tee a stream than the naive
  // method, which relies upon `tryRead()`. The default implementation returns nullptr.
};

class WritableStreamSink {
public:
  virtual kj::Promise<void> write(const void* buffer, size_t size)
      KJ_WARN_UNUSED_RESULT = 0;
  virtual kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces)
      KJ_WARN_UNUSED_RESULT = 0;

  virtual kj::Promise<void> end() KJ_WARN_UNUSED_RESULT = 0;
  // Must call to flush and finish the stream.

  virtual kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end);

  virtual void abort(kj::Exception reason) = 0;
  // TODO(conform): abort() should return a promise after which closed fulfillers should be
  //   rejected. This may necessitate an "erroring" state.
};

// =======================================================================================
// The ReadableStreamInternalController and WritableStreamInternalController provide the
// internal (original) implementation of the ReadableStream/WritableStream objects and are
// each backed by the ReadableStreamSource and WritableStreamSink respectively. Every stream
// implementation that originates from *within* the Workers runtime will use these.
//
// It is important to understand that the behavior of these are not entirely compliant with
// the streams specification.

// The ReadableStreamInternalController is always in one of three states: Readable, Closed,
// or Errored. When the state is Readable, the controller has an associated ReadableStreamSource.
// When the state is Errored, the ReadableStreamSource has been released and the controller
// stores a jsg::Value with whatever value was used to error. When Closed, the
// ReadableStreamSource has been released.

// Likewise, the WritableStreamInternalController is always either Writable, Closed, or Errored.
// When the state is Writable, the controller has an associated WritableStreamSink. In either of
// the other two states, the sink has been released.

class WritableStreamInternalController;

class ReadableStreamInternalController: public ReadableStreamController {
public:
  using Readable = IoOwn<ReadableStreamSource>;

  explicit ReadableStreamInternalController(StreamStates::Closed closed)
      : state(closed) {}
  explicit ReadableStreamInternalController(StreamStates::Errored errored)
      : state(kj::mv(errored)) {}
  explicit ReadableStreamInternalController(Readable readable)
      : state(kj::mv(readable)) {}

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamInternalController);

  ~ReadableStreamInternalController() noexcept(false) override;

  void setOwnerRef(ReadableStream& stream) override {
    owner = stream;
  }

  jsg::Ref<ReadableStream> addRef() override;

  bool isByteOriented() const override { return true; }

  kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js,
      kj::Maybe<ByobOptions> byobOptions) override;

  jsg::Promise<void> pipeTo(jsg::Lock& js,
                             WritableStreamController& destination,
                             PipeToOptions options) override;

  jsg::Promise<void> cancel(jsg::Lock& js,
                             jsg::Optional<v8::Local<v8::Value>> reason) override;

  Tee tee(jsg::Lock& js) override;

  kj::Maybe<kj::Own<ReadableStreamSource>> removeSource(jsg::Lock& js, bool ignoreDisturbed=false);

  bool isClosedOrErrored() const override {
    return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
  }

  bool isDisturbed() override { return disturbed; }

  bool isLockedToReader() const override { return !readState.is<Unlocked>(); }

  bool lockReader(jsg::Lock& js, Reader& reader) override;

  void releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseReader in common.h for details on the use of maybeJs

  kj::Maybe<PipeController&> tryPipeLock(jsg::Ref<WritableStream> destination) override;

  void visitForGc(jsg::GcVisitor& visitor) override;

  jsg::Promise<kj::Array<byte>> readAllBytes(jsg::Lock& js, uint64_t limit) override;
  jsg::Promise<kj::String> readAllText(jsg::Lock& js, uint64_t limit) override;

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override;

private:
  void doCancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);
  void doClose();
  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  class PipeLocked : public PipeController {
  public:
    PipeLocked(
        ReadableStreamInternalController& inner,
        jsg::Ref<WritableStream> ref)
        : inner(inner),
          ref(kj::mv(ref)) {}

    bool isClosed() override;

    kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) override;

    void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) override;

    void close() override;

    void error(jsg::Lock& js, v8::Local<v8::Value> reason) override;

    void release(jsg::Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeError = nullptr) override;

    kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) override;

    jsg::Promise<ReadResult> read(jsg::Lock& js) override;

    void visitForGc(jsg::GcVisitor& visitor) { visitor.visit(ref); }

  private:
    ReadableStreamInternalController& inner;
    jsg::Ref<WritableStream> ref;
  };

  kj::Maybe<ReadableStream&> owner;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Readable> state;
  kj::OneOf<Unlocked, Locked, PipeLocked, ReaderLocked> readState = Unlocked();
  bool disturbed = false;
  bool readPending = false;

  friend class ReadableStream;
  friend class WritableStreamInternalController;
  friend class PipeLocked;
};

kj::Own<ReadableStreamInternalController> newReadableStreamInternalController(
    IoContext& ioContext,
    kj::Own<ReadableStreamSource> source);

class WritableStreamInternalController: public WritableStreamController {
public:
  using Writable = IoOwn<WritableStreamSink>;

  explicit WritableStreamInternalController(StreamStates::Closed closed)
      : state(closed) {}
  explicit WritableStreamInternalController(StreamStates::Errored errored)
      : state(kj::mv(errored)) {}
  explicit WritableStreamInternalController(Writable writable,
                                            kj::Maybe<uint64_t> maybeHighWaterMark = nullptr)
      : state(kj::mv(writable)),
        maybeHighWaterMark(maybeHighWaterMark) {
}

  WritableStreamInternalController(WritableStreamInternalController&& other) = default;
  WritableStreamInternalController& operator=(WritableStreamInternalController&& other) = default;

  ~WritableStreamInternalController() noexcept(false) override;

  void setOwnerRef(WritableStream& stream) override {
    owner = stream;
  }

  jsg::Ref<WritableStream> addRef() override;

  jsg::Promise<void> write(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> value) override;

  jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false) override;

  jsg::Promise<void> flush(jsg::Lock& js, bool markAsHandled = false) override;

  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) override;

  kj::Maybe<jsg::Promise<void>> tryPipeFrom(
      jsg::Lock& js,
      jsg::Ref<ReadableStream> source,
      PipeToOptions options) override;

  kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) override;

  kj::Maybe<int> getDesiredSize() override;

  bool isLockedToWriter() const override { return !writeState.is<Unlocked>(); }

  bool lockWriter(jsg::Lock& js, Writer& writer) override;

  void releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseWriter in common.h for details on the use of maybeJs

  kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js) override {
    // TODO(later): The internal controller has no concept of an "erroring"
    // state, so for now we just return nullptr here.
    return nullptr;
  }

  void visitForGc(jsg::GcVisitor& visitor) override;

  void setHighWaterMark(uint64_t highWaterMark);

private:
  bool isClosedOrClosing();

  struct AbortOptions {
    bool reject = false;
    bool handled = false;
  };

  jsg::Promise<void> doAbort(jsg::Lock& js,
                              v8::Local<v8::Value> reason,
                              AbortOptions options = {
                                .reject = false,
                                .handled = false
                              });
  void doClose();
  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);
  void ensureWriting(jsg::Lock& js);
  jsg::Promise<void> writeLoop(jsg::Lock& js, IoContext& ioContext);
  jsg::Promise<void> writeLoopAfterFrontOutputLock(jsg::Lock& js);

  void drain(jsg::Lock& js, v8::Local<v8::Value> reason);
  void finishClose(jsg::Lock& js);
  void finishError(jsg::Lock& js, v8::Local<v8::Value> reason);

  struct PipeLocked {
    ReadableStream& ref;
  };

  kj::Maybe<WritableStream&> owner;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Writable> state;
  kj::OneOf<Unlocked, Locked, PipeLocked, WriterLocked> writeState = Unlocked();

  kj::Maybe<PendingAbort> maybePendingAbort;

  uint64_t currentWriteBufferSize = 0;
  kj::Maybe<uint64_t> maybeHighWaterMark;
  // The highWaterMark is the total amount of data currently buffered in
  // the controller waiting to be flushed out to the underlying WritableStreamSink.
  // It is used to implement backpressure signaling using desiredSize and the ready
  // promise on the writer.

  void increaseCurrentWriteBufferSize(jsg::Lock& js, uint64_t amount);
  void decreaseCurrentWriteBufferSize(jsg::Lock& js, uint64_t amount);
  void updateBackpressure(jsg::Lock& js, bool backpressure);

  struct Write {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    std::shared_ptr<v8::BackingStore> ownBytes;
    kj::ArrayPtr<const kj::byte> bytes;
    kj::Maybe<jsg::Ref<WritableStream>> ref;
  };
  struct Close {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
  };
  struct Flush {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
  };
  struct Pipe {
    WritableStreamInternalController& parent;
    ReadableStreamController::PipeController& source;
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    bool preventAbort;
    bool preventClose;
    bool preventCancel;
    kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;

    bool checkSignal(jsg::Lock& js);
    jsg::Promise<void> pipeLoop(jsg::Lock& js);
    jsg::Promise<void> write(v8::Local<v8::Value> value);
  };
  struct WriteEvent {
    kj::Maybe<IoOwn<kj::Promise<void>>> outputLock;  // must wait for this before actually writing
    kj::OneOf<Write, Pipe, Close, Flush> event;
  };

  std::deque<WriteEvent> queue;
};

class IdentityTransformStreamImpl: public kj::Refcounted,
                                   public ReadableStreamSource,
                                   public WritableStreamSink {
  // An implementation of ReadableStreamSource and WritableStreamSink which communicates read and
  // write requests via a OneOf.
  //
  // This class is also used as the implementation of FixedLengthStream, in which case `limit` is
  // non-nullptr.
  //
  // TODO(soon): Reimplement this in terms of kj::OneWayPipe, so we can optimize pumpTo().

public:
  explicit IdentityTransformStreamImpl(kj::Maybe<uint64_t> limit = nullptr)
      : limit(limit) {}

  ~IdentityTransformStreamImpl() noexcept(false) {
    // Due to the different natures of JS and C++ disposal, there is no point in enforcing the limit
    // for a FixedLengthStream here.
    //
    // 1. Creating but not using a `new FixedLengthStream(n)` should not be an error, and ought not
    //    to logspam us.
    // 2. Chances are high that by the time this object gets destroyed, it's too late to tell the
    //    user about the failure.
  }

  // ReadableStreamSource implementation -------------------------------------------------

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;

  kj::Promise<size_t> tryReadInternal(void* buffer, size_t maxBytes);

  kj::Promise<DeferredProxy<void>> pumpTo(WritableStreamSink& output, bool end) override;

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override;

  void cancel(kj::Exception reason) override;

  // WritableStreamSink implementation ---------------------------------------------------

  kj::Promise<void> write(const void* buffer, size_t size) override;

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  kj::Promise<void> end() override;

  void abort(kj::Exception reason) override;

private:
  kj::Promise<size_t> readHelper(kj::ArrayPtr<kj::byte> bytes);

  kj::Promise<void> writeHelper(kj::ArrayPtr<const kj::byte> bytes);

  kj::Maybe<uint64_t> limit;

  struct ReadRequest {
    kj::ArrayPtr<kj::byte> bytes;
    // WARNING: `bytes` may be invalid if fulfiller->isWaiting() returns false! (This indicates the
    //   read was canceled.)

    kj::Own<kj::PromiseFulfiller<size_t>> fulfiller;
  };

  struct WriteRequest {
    kj::ArrayPtr<const kj::byte> bytes;
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
  };

  struct Idle {};

  kj::OneOf<Idle, ReadRequest, WriteRequest, kj::Exception, StreamStates::Closed> state = Idle();
};

}  // namespace workerd::api
