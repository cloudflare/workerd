// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include "readable.h"
#include "writable.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/state-machine.h>

#include <kj/refcount.h>

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
// stores a js Value with whatever value was used to error. When Closed, the
// ReadableStreamSource has been released.

// Likewise, the WritableStreamInternalController is always either Writable, Closed, or Errored.
// When the state is Writable, the controller has an associated WritableStreamSink. In either of
// the other two states, the sink has been released.

class WritableStreamInternalController;

class ReadableStreamInternalController: public ReadableStreamController, public kj::PtrTarget {
 public:
  using Readable = IoOwn<ReadableStreamSource>;

  explicit ReadableStreamInternalController(StreamStates::Closed closed)
      : state(State::create<StreamStates::Closed>()) {}
  explicit ReadableStreamInternalController(StreamStates::Errored errored)
      : state(State::create<StreamStates::Errored>(kj::mv(errored))) {}
  explicit ReadableStreamInternalController(Readable readable)
      : state(State::create<Readable>(kj::mv(readable))) {}

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamInternalController);

  ~ReadableStreamInternalController() noexcept(false) override;

  void setOwnerRef(kj::Weak<ReadableStream> stream) override;

  jsg::Ref<ReadableStream> addRef() override;

  bool isByteOriented() const override {
    return true;
  }

  kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js, kj::Maybe<ByobOptions> byobOptions) override;

  kj::Maybe<jsg::Promise<DrainingReadResult>> drainingRead(
      jsg::Lock& js, size_t maxRead = kj::maxValue) override;

  jsg::Promise<void> pipeTo(
      jsg::Lock& js, WritableStreamController& destination, PipeToOptions options) override;

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) override;

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

  bool lockReader(jsg::Lock& js, kj::Ptr<Reader> reader) override;

  void releaseReader(kj::Ptr<Reader> reader, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseReader in common.h for details on the use of maybeJs

  kj::Maybe<kj::Ptr<PipeController>> tryPipeLock() override;

  void releasePipeLock(jsg::Lock& js, kj::Maybe<jsg::JsValue> maybeError = kj::none) override;

  void visitForGc(jsg::GcVisitor& visitor) override;

  jsg::Promise<jsg::JsRef<jsg::JsArrayBuffer>> readAllBytes(jsg::Lock& js, uint64_t limit) override;
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
  void doCancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);
  void doClose(jsg::Lock& js);
  void doError(jsg::Lock& js, jsg::JsValue reason);

  class PipeLocked: public PipeController {
   public:
    static constexpr kj::StringPtr NAME KJ_UNUSED = "pipe-locked"_kj;
    PipeLocked(kj::Ptr<ReadableStreamInternalController> inner, IoOwn<kj::Canceler> pumpCanceler)
        : inner(kj::mv(inner)),
          pumpCanceler(kj::mv(pumpCanceler)) {}

    bool isClosed() override;

    kj::Maybe<jsg::JsValue> tryGetErrored(jsg::Lock& js) override;

    void close(jsg::Lock& js) override;

    void error(jsg::Lock& js, jsg::JsValue reason) override;

    kj::Maybe<kj::Promise<void>> tryPumpTo(kj::Ptr<WritableStreamSink> sink, bool end) override;

    void cancelPump(const kj::Exception& reason) {
      pumpCanceler->cancel(reason);
    }

    jsg::Promise<ReadResult> read(jsg::Lock& js) override;

    kj::Ptr<PipeController> getPtr() override {
      return addPtrToThis();
    }

   private:
    kj::Ptr<ReadableStreamInternalController> inner;
    IoOwn<kj::Canceler> pumpCanceler;
  };

  kj::Weak<ReadableStream> owner;

  // State machine for ReadableStreamInternalController:
  // Closed is terminal, Errored is implicitly terminal via ErrorState.
  // Readable is the active state (stream has data).
  using State = StateMachine<TerminalStates<StreamStates::Closed>,
      ErrorState<StreamStates::Errored>,
      ActiveState<Readable>,
      StreamStates::Closed,
      StreamStates::Errored,
      Readable>;
  State state;

  // Lock state machine for ReadableStreamInternalController:
  // All states can transition to any other state (no terminal states).
  //   Unlocked -> Locked (removeSink() or pumpTo() called)
  //   Unlocked -> ReaderLocked (lockReader() called)
  //   Unlocked -> PipeLocked (tryPipeLock() called)
  //   ReaderLocked -> Unlocked (releaseReader() called)
  //   PipeLocked -> Unlocked (releasePipeLock() called)
  //     Only the pipe machinery performs this transition, after dropping the
  //     kj::Ptr it holds to the PipeLocked state's PipeController. doClose() and
  //     doError() deliberately leave the pipe lock in place.
  //   Locked -> (remains until stream is done)
  using ReadLockState = StateMachine<Unlocked, Locked, PipeLocked, ReaderLocked>;
  ReadLockState readState = ReadLockState::create<Unlocked>();
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

  explicit WritableStreamInternalController(StreamStates::Closed closed)
      : state(State::create<StreamStates::Closed>()) {}
  explicit WritableStreamInternalController(StreamStates::Errored errored)
      : state(State::create<StreamStates::Errored>(kj::mv(errored))) {}
  explicit WritableStreamInternalController(kj::Own<WritableStreamSink> writable,
      kj::Maybe<kj::Own<ByteStreamObserver>> observer,
      kj::Maybe<uint64_t> maybeHighWaterMark = kj::none,
      kj::Maybe<jsg::Promise<void>> maybeClosureWaitable = kj::none)
      : state(State::create<IoOwn<Writable>>(
            IoContext::current().createObject<Writable>(kj::mv(writable)))),
        observer(kj::mv(observer)),
        maybeHighWaterMark(maybeHighWaterMark),
        maybeClosureWaitable(kj::mv(maybeClosureWaitable)) {}

  WritableStreamInternalController(WritableStreamInternalController&& other) = default;
  WritableStreamInternalController& operator=(WritableStreamInternalController&& other) = default;

  ~WritableStreamInternalController() noexcept(false) override;

  void setOwnerRef(kj::Weak<WritableStream> stream) override {
    owner = kj::mv(stream);
  }

  jsg::Ref<WritableStream> addRef() override;

  jsg::Promise<void> write(jsg::Lock& js, jsg::Optional<jsg::JsValue> value) override;

  jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false) override;

  jsg::Promise<void> flush(jsg::Lock& js, bool markAsHandled = false) override;

  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason) override;

  kj::Maybe<jsg::Promise<void>> tryPipeFrom(
      jsg::Lock& js, jsg::Ref<ReadableStream> source, PipeToOptions options) override;

  kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) override;
  void detach(jsg::Lock& js) override;

  kj::Maybe<int> getDesiredSize() override;

  bool isLockedToWriter() const override {
    return !writeState.is<Unlocked>();
  }

  bool lockWriter(jsg::Lock& js, kj::Ptr<Writer> writer) override;

  void releaseWriter(kj::Ptr<Writer> writer, kj::Maybe<jsg::Lock&> maybeJs) override;
  // See the comment for releaseWriter in common.h for details on the use of maybeJs

  kj::Maybe<jsg::JsValue> isErroring(jsg::Lock& js) override {
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
      jsg::JsValue reason,
      AbortOptions options = {.reject = false, .handled = false});
  void doClose(jsg::Lock& js);
  void doError(jsg::Lock& js, jsg::JsValue reason);
  void ensureWriting(jsg::Lock& js);

  // `syncDepth` counts consecutive synchronous loop continuations (writes completed via
  // tryWriteSync() and zero-length writes), bounding the recursion through
  // writeLoop() -> writeLoopAfterFrontOutputLock(). When the budget is exhausted, the next
  // write takes the asynchronous path, which resets the depth.
  jsg::Promise<void> writeLoop(jsg::Lock& js, IoContext& ioContext, size_t syncDepth = 0);
  jsg::Promise<void> writeLoopAfterFrontOutputLock(jsg::Lock& js, size_t syncDepth = 0);

  void drain(jsg::Lock& js, jsg::JsValue reason);
  void finishClose(jsg::Lock& js);
  void finishError(jsg::Lock& js, jsg::JsValue reason);
  jsg::Promise<void> closeImpl(jsg::Lock& js, bool markAsHandled);

  struct PipeLocked {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "pipe-locked"_kj;
    // The source ReadableStream this writable is being piped from. Weak because the
    // stream's lifetime belongs to JS/GC: the Pipe queue event holds the strong
    // (GC-visited) reference for the duration of the pipe, while this lock state can
    // briefly exist without it. Consumers must go through tryAddRef().
    jsg::WeakRef<ReadableStream> ref;
  };

  kj::Weak<WritableStream> owner;

  // State machine for WritableStreamInternalController:
  // Closed is terminal, Errored is implicitly terminal via ErrorState.
  // IoOwn<Writable> is the active state (stream is writable).
  using State = StateMachine<TerminalStates<StreamStates::Closed>,
      ErrorState<StreamStates::Errored>,
      ActiveState<IoOwn<Writable>>,
      StreamStates::Closed,
      StreamStates::Errored,
      IoOwn<Writable>>;
  State state;

  // Lock state machine for WritableStreamInternalController:
  // All states can transition to any other state (no terminal states).
  //   Unlocked -> Locked (removeSink() or detach() called)
  //   Unlocked -> WriterLocked (lockWriter() called)
  //   Unlocked -> PipeLocked (tryPipeFrom() called)
  //   WriterLocked -> Unlocked (releaseWriter() called)
  //   WriterLocked -> Locked (doClose/doError called - stream closed but writer still attached)
  //   PipeLocked -> Unlocked (pipe completes, or doClose/doError/drain during an
  //     active pipe; the source's pipe lock is released separately, by the pipe
  //     machinery's queue-teardown paths via Pipe::releaseSource())
  using WriteLockState = StateMachine<Unlocked, Locked, PipeLocked, WriterLocked>;
  WriteLockState writeState = WriteLockState::create<Unlocked>();

  kj::Maybe<kj::Own<ByteStreamObserver>> observer;

  kj::Maybe<PendingAbort> maybePendingAbort;

  uint64_t currentWriteBufferSize = 0;

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

  void adjustWriteBufferSize(jsg::Lock& js, int64_t amount);
  void updateBackpressure(jsg::Lock& js, bool backpressure);

  struct Write {
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    size_t totalBytes;
    kj::Array<const kj::byte> ownBytes;
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
  struct Pipe: kj::PtrTarget {
    // Shared handle used by the pipe loop's promise continuations. The Weak<Pipe>
    // detects whether the Pipe (a queue event owned by the destination controller) is
    // still alive.
    //
    // NOTE: The wrapper methods below must not hold a strong kj::Ptr<Pipe> across the
    // delegated call: several of the delegated methods can destroy the Pipe (e.g.
    // checkSignal() drains the destination queue, and any releaseSource() with a
    // cancel reason can run user JS that does the same). They use weakRef.tryGet() to
    // obtain a plain reference instead — the Weak protects against *entering* a dead
    // Pipe, and nothing touches the Pipe after the delegated call returns.
    struct State: public kj::Refcounted {
      jsg::Ref<WritableStream> owner;
      kj::Weak<Pipe> weakRef;

      State(jsg::Ref<WritableStream> owner, kj::Weak<Pipe> weakRef)
          : owner(kj::mv(owner)),
            weakRef(kj::mv(weakRef)) {}

      inline bool isAborted() const {
        return weakRef == nullptr;
      }
      bool checkSignal(jsg::Lock& js);
      jsg::Promise<void> pipeLoop(jsg::Lock& js);
      jsg::Promise<void> write(jsg::Lock& js, jsg::JsValue value);
      void releaseSource(jsg::Lock& js, kj::Maybe<jsg::JsValue> maybeError = kj::none);
      bool isSourceReleased();
      void tryErrorParent(jsg::Lock& js, jsg::JsValue reason);
      void tryFinishCloseParent(jsg::Lock& js);
      void tryFinishErrorParent(jsg::Lock& js, jsg::JsValue reason);
      void tryNoBytesError(jsg::Lock& js);
    };

    WritableStreamInternalController& parent;
    // Keeps the source ReadableStream (and therefore the PipeController that lives in
    // its lock state) alive for the duration of the pipe, and provides the handle
    // through which releaseSource() releases the source's pipe lock. Declared before
    // `source` so that the kj::Ptr is destroyed first: the PipeController must not be
    // destroyed (by the readable's death) while our pointer to it remains.
    jsg::Ref<ReadableStream> readable;
    kj::Maybe<kj::Ptr<ReadableStreamController::PipeController>> source;
    kj::Maybe<jsg::Promise<void>::Resolver> promise;
    struct Flags {
      uint8_t preventAbort : 1;
      uint8_t preventClose : 1;
      uint8_t preventCancel : 1;
      uint8_t perfettoTraceStarted : 1;
    };
    Flags flags{};
    kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;
    kj::Maybe<jsg::JsRef<jsg::JsValue>> capturedSourceError;

    Pipe(WritableStreamInternalController& parent,
        jsg::Ref<ReadableStream> readable,
        kj::Ptr<ReadableStreamController::PipeController> source,
        jsg::Promise<void>::Resolver promise,
        bool preventAbort,
        bool preventClose,
        bool preventCancel,
        kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal);

    ~Pipe() noexcept(false);

    KJ_DISALLOW_COPY_AND_MOVE(Pipe);

    kj::Rc<State> getState() {
      return kj::rc<State>(parent.addRef(), addWeakToThis());
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(readable, promise, maybeSignal, capturedSourceError);
    }

    void releaseSource(jsg::Lock& js, kj::Maybe<jsg::JsValue> maybeError = kj::none);
    bool checkSignal(jsg::Lock& js);
    jsg::Promise<void> pipeLoop(jsg::Lock& js);
    jsg::Promise<void> write(jsg::Lock& js, jsg::JsValue value);
    bool isSourceReleased() const {
      return source == kj::none;
    }
    void errorParent(jsg::Lock& js, jsg::JsValue reason);
    void finishCloseParent(jsg::Lock& js);
    void finishErrorParent(jsg::Lock& js, jsg::JsValue reason);
    void noBytesError(jsg::Lock& js);
    kj::Maybe<jsg::Promise<void>::Resolver> takePromise() {
      return kj::mv(promise);
    }

    // Memory info methods are defined out-of-line (in internal.c++) because tracking
    // the `readable` field requires ReadableStream to be a complete type, and this
    // header only sees its forward declaration.
    kj::StringPtr jsgGetMemoryName() const;
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };
  struct WriteEvent {
    kj::Maybe<IoOwn<kj::Promise<void>>> outputLock;  // must wait for this before actually writing
    kj::OneOf<Write, kj::Own<Pipe>, Close, Flush> event;

    bool isCloseOrFlush() const {
      return event.is<Close>() || event.is<Flush>();
    }

    bool isPipe() const {
      return event.is<kj::Own<Pipe>>();
    }

    JSG_MEMORY_INFO(WriteEvent) {
      if (outputLock != kj::none) {
        tracker.trackFieldWithSize("outputLock", sizeof(IoOwn<kj::Promise<void>>));
      }
      KJ_SWITCH_ONEOF(event) {
        KJ_CASE_ONEOF(w, Write) {
          tracker.trackField("inner", w);
        }
        KJ_CASE_ONEOF(p, kj::Own<Pipe>) {
          tracker.trackField("inner", *p);
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

  RingBuffer<WriteEvent, 8> queue;
};
}  // namespace workerd::api
