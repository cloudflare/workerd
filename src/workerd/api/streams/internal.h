// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include "writable.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>

#include <deque>

namespace workerd::api {

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

  explicit ReadableStreamInternalController(StreamStates::Closed closed): state(closed) {}
  explicit ReadableStreamInternalController(StreamStates::Errored errored)
      : state(kj::mv(errored)) {}
  explicit ReadableStreamInternalController(Readable readable): state(kj::mv(readable)) {}

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamInternalController);

  ~ReadableStreamInternalController() noexcept(false) override;

  void setOwnerRef(ReadableStream& stream) override {
    owner = stream;
  }

  jsg::Ref<ReadableStream> addRef() override;

  bool isByteOriented() const override {
    return true;
  }

  kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js, kj::Maybe<ByobOptions> byobOptions) override;

  jsg::Promise<void> pipeTo(
      jsg::Lock& js, WritableStreamController& destination, PipeToOptions options) override;

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) override;

  Tee tee(jsg::Lock& js) override;

  kj::Maybe<kj::Own<ReadableStreamSource>> removeSource(
      jsg::Lock& js, bool ignoreDisturbed = false);

  bool isClosedOrErrored() const override {
    return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
  }

  bool isClosed() const override {
    return state.is<StreamStates::Closed>();
  }

  bool isDisturbed() override {
    return disturbed;
  }

  bool isLockedToReader() const override {
    return !readState.is<Unlocked>();
  }

  bool lockReader(jsg::Lock& js, Reader& reader) override;

  void releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseReader in common.h for details on the use of maybeJs

  kj::Maybe<PipeController&> tryPipeLock(jsg::Ref<WritableStream> destination) override;

  void visitForGc(jsg::GcVisitor& visitor) override;

  jsg::Promise<jsg::BufferSource> readAllBytes(jsg::Lock& js, uint64_t limit) override;
  jsg::Promise<kj::String> readAllText(jsg::Lock& js, uint64_t limit) override;

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override;

  kj::Promise<DeferredProxy<void>> pumpTo(
      jsg::Lock& js, kj::Own<WritableStreamSink> sink, bool end) override;

  StreamEncoding getPreferredEncoding() override;

  kj::Own<ReadableStreamController> detach(jsg::Lock& js, bool ignoreDisturbed) override;

  void setPendingClosure() override {
    isPendingClosure = true;
  }

  kj::StringPtr jsgGetMemoryName() const override;
  size_t jsgGetMemorySelfSize() const override;
  void jsgGetMemoryInfo(jsg::MemoryTracker& info) const override;

 private:
  void doCancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);
  void doClose(jsg::Lock& js);
  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  class PipeLocked: public PipeController {
   public:
    PipeLocked(ReadableStreamInternalController& inner, jsg::Ref<WritableStream> ref)
        : inner(inner),
          ref(kj::mv(ref)) {}

    bool isClosed() override;

    kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) override;

    void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) override;

    void close(jsg::Lock& js) override;

    void error(jsg::Lock& js, v8::Local<v8::Value> reason) override;

    void release(jsg::Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeError = kj::none) override;

    kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) override;

    jsg::Promise<ReadResult> read(jsg::Lock& js) override;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(ref);
    }

    kj::StringPtr jsgGetMemoryName() const;
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& info) const;

   private:
    ReadableStreamInternalController& inner;
    jsg::Ref<WritableStream> ref;
  };

  kj::Maybe<ReadableStream&> owner;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Readable> state;
  kj::OneOf<Unlocked, Locked, PipeLocked, ReaderLocked> readState = Unlocked();
  bool disturbed = false;
  bool readPending = false;

  // Used by Sockets code to signal to the ReadableStream that it should error when read from
  // because the socket is currently being closed.
  bool isPendingClosure = false;

  friend class ReadableStream;
  friend class WritableStreamInternalController;
  friend class PipeLocked;
};

class WritableStreamInternalController: public WritableStreamController {
 public:
  struct Writable {
    kj::Own<WritableStreamSink> sink;
    kj::Canceler canceler;
    Writable(kj::Own<WritableStreamSink> sink): sink(kj::mv(sink)) {}
    void abort(kj::Exception&& ex);
  };

  explicit WritableStreamInternalController(StreamStates::Closed closed): state(closed) {}
  explicit WritableStreamInternalController(StreamStates::Errored errored)
      : state(kj::mv(errored)) {}
  explicit WritableStreamInternalController(kj::Own<WritableStreamSink> writable,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none)
      : state(IoContext::current().addObject(kj::heap<Writable>(kj::mv(writable)))),
        observer(kj::mv(observer)),
        maybeHighWaterMark(maybeHighWaterMark),
        maybeClosureWaitable(kj::mv(maybeClosureWaitable)) {}

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
      jsg::Lock& js, jsg::Ref<ReadableStream> source, PipeToOptions options) override;

  kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) override;
  void detach(jsg::Lock& js) override;

  kj::Maybe<int> getDesiredSize() override;

  bool isLockedToWriter() const override {
    return !writeState.is<Unlocked>();
  }

  bool lockWriter(jsg::Lock& js, Writer& writer) override;

  void releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseWriter in common.h for details on the use of maybeJs

  kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js) override {
    // TODO(later): The internal controller has no concept of an "erroring"
    // state, so for now we just return kj::none here.
    return kj::none;
  }

  void visitForGc(jsg::GcVisitor& visitor) override;

  void setHighWaterMark(uint64_t highWaterMark);

  bool isClosedOrClosing() override;
  bool isPiping();
  bool isErrored() override;

  inline bool isByteOriented() const override {
    return true;
  }

  void setPendingClosure() override {
    isPendingClosure = true;
  }

  kj::StringPtr jsgGetMemoryName() const override;
  size_t jsgGetMemorySelfSize() const override;
  void jsgGetMemoryInfo(jsg::MemoryTracker& info) const override;

 private:
  struct AbortOptions {
    bool reject = false;
    bool handled = false;
  };

  jsg::Promise<void> doAbort(jsg::Lock& js,
      v8::Local<v8::Value> reason,
      AbortOptions options = {.reject = false, .handled = false});
  void doClose(jsg::Lock& js);
  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);
  void ensureWriting(jsg::Lock& js);
  jsg::Promise<void> writeLoop(jsg::Lock& js, IoContext& ioContext);
  jsg::Promise<void> writeLoopAfterFrontOutputLock(jsg::Lock& js);

  void drain(jsg::Lock& js, v8::Local<v8::Value> reason);
  void finishClose(jsg::Lock& js);
  void finishError(jsg::Lock& js, v8::Local<v8::Value> reason);
  jsg::Promise<void> closeImpl(jsg::Lock& js, bool markAsHandled);

  struct PipeLocked {
    ReadableStream& ref;
  };

  kj::Maybe<WritableStream&> owner;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, IoOwn<Writable>> state;
  kj::OneOf<Unlocked, Locked, PipeLocked, WriterLocked> writeState = Unlocked();

  kj::Maybe<kj::Own<ByteStreamObserver>> observer;

  kj::Maybe<PendingAbort> maybePendingAbort;

  uint64_t currentWriteBufferSize = 0;
  bool warnAboutExcessiveBackpressure = true;
  size_t excessiveBackpressureWarningCount = 0;

  // The highWaterMark is the total amount of data currently buffered in
  // the controller waiting to be flushed out to the underlying WritableStreamSink.
  // It is used to implement backpressure signaling using desiredSize and the ready
  // promise on the writer.
  kj::Maybe<uint64_t> maybeHighWaterMark;

  // Used by Sockets code to ensure the connection is established before the associated
  // WritableStream is closed.
  kj::Maybe<jsg::Promise<void>> maybeClosureWaitable;
  bool waitingOnClosureWritableAlready = false;

  // Used by Sockets code to signal to the WritableStream that it should error when written to
  // because the socket is currently being closed.
  bool isPendingClosure = false;

  void increaseCurrentWriteBufferSize(jsg::Lock& js, uint64_t amount);
  void decreaseCurrentWriteBufferSize(jsg::Lock& js, uint64_t amount);
  void updateBackpressure(jsg::Lock& js, bool backpressure);

  struct Write {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    size_t totalBytes;
    jsg::V8Ref<v8::ArrayBuffer> ownBytes;
    kj::ArrayPtr<const kj::byte> bytes;

    JSG_MEMORY_INFO(Write) {
      tracker.trackField("resolver", promise);
      if (ownBytes != nullptr) {
        tracker.trackFieldWithSize("backing", totalBytes);
      }
    }
  };
  struct Close {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    JSG_MEMORY_INFO(Close) {
      tracker.trackField("promise", promise);
    }
  };
  struct Flush {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    JSG_MEMORY_INFO(Flush) {
      tracker.trackField("promise", promise);
    }
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

    JSG_MEMORY_INFO(Pipe) {
      tracker.trackField("resolver", promise);
      tracker.trackField("signal", maybeSignal);
    }
  };
  struct WriteEvent {
    kj::Maybe<IoOwn<kj::Promise<void>>> outputLock;  // must wait for this before actually writing
    kj::OneOf<Write, Pipe, Close, Flush> event;

    JSG_MEMORY_INFO(WriteEvent) {
      if (outputLock != kj::none) {
        tracker.trackFieldWithSize("outputLock", sizeof(IoOwn<kj::Promise<void>>));
      }
      KJ_SWITCH_ONEOF(event) {
        KJ_CASE_ONEOF(w, Write) {
          tracker.trackField("inner", w);
        }
        KJ_CASE_ONEOF(p, Pipe) {
          tracker.trackField("inner", p);
        }
        KJ_CASE_ONEOF(c, Close) {
          tracker.trackField("inner", c);
        }
        KJ_CASE_ONEOF(f, Flush) {
          tracker.trackField("inner", f);
        }
      }
    }
  };

  std::deque<WriteEvent> queue;
};

// An implementation of ReadableStreamSource and WritableStreamSink which communicates read and
// write requests via a OneOf.
//
// This class is also used as the implementation of FixedLengthStream, in which case `limit` is
// non-nullptr.
class IdentityTransformStreamImpl: public kj::Refcounted,
                                   public ReadableStreamSource,
                                   public WritableStreamSink {
  // TODO(soon): Reimplement this in terms of kj::OneWayPipe, so we can optimize pumpTo().

 public:
  explicit IdentityTransformStreamImpl(kj::Maybe<uint64_t> limit = kj::none): limit(limit) {}

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

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override;

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
