// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include "queue.h"

#include <workerd/jsg/jsg.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/state-machine.h>
#include <workerd/util/weak-refs.h>

namespace workerd {
class ByteStreamObserver;
}  // namespace workerd

namespace workerd::api {

// =======================================================================================
// ReadableStreamJsController, WritableStreamJsController, and the rest here define the
// implementation of JavaScript-backed ReadableStream and WritableStreams.
//
// A JavaScript-backed ReadableStream is backed by a ReadableStreamJsController that is either
// Closed, Errored, or in a Readable state. When readable, the controller owns either a
// ReadableStreamDefaultController or ReadableByteStreamController object that corresponds
// to the identically named interfaces in the streams spec. These objects are responsible
// for the bulk of the implementation detail, with the ReadableStreamJsController serving
// only as a bridge between it and the ReadableStream object itself.
//
//  * ReadableStream -> ReadableStreamJsController -> jsg::Ref<ReadableStreamDefaultController>
//  * ReadableStream -> ReadableStreamJsController -> jsg::Ref<ReadableByteStreamController>
//
// Contrast this with the implementation of internal streams using the
// ReadableStreamInternalController:
//
//  * ReadableStream -> ReadableStreamInternalController -> IoOwn<ReadableStreamSource>
//
// When user-code creates a JavaScript-backed ReadableStream using the `ReadableStream`
// object constructor, they pass along an object called an "underlying source" that provides
// JavaScript functions the ReadableStream will call to either initialize, close, or source
// the data for the stream:
//
//   const readable = new ReadableStream({
//     async start(controller) {
//       // Initialize the stream
//     },
//     async pull(controller) {
//       // Provide the stream data
//     },
//     async cancel(reason) {
//       // Cancel and de-initialize the stream
//     }
//   });
//
// By default, a JavaScript-backed ReadableStream is value-oriented -- that is, any JavaScript
// type can be passed through the stream. It is not limited to bytes only. The implementation
// of the pull method on the underlying source can push strings, booleans, numbers, even undefined
// as values that can be read from the stream. In such streams, the `controller` used internally
// (and owned by the ReadableStreamJsController) is the `ReadableStreamDefaultController`.
//
// To create a byte-oriented stream -- one that is capable only of working with bytes in the
// form of ArrayBufferViews (e.g. `Uint8Array`, `Uint16Array`, `DataView`, etc), the underlying
// source object passed into the `ReadableStream` constructor must have a property
// `'type' = 'bytes'`.
//
//   const readable = new ReadableStream({
//     type: 'bytes',
//     async start(controller) {
//       // Initialize the stream
//     },
//     async pull(controller) {
//       // Provide the stream data
//     },
//     async cancel(reason) {
//       // Cancel and de-initialize the stream
//     }
//   });
//
// From here on, we'll refer to these as either value streams or byte streams. And we'll refer to
// ReadableStreamDefaultController as simply "DefaultController", and ReadableByteStreamController
// as simply "ByobController".
//
// The DefaultController and ByobController each maintain an internal queue. When a read request
// is received, if there is enough data in the internal queue to fulfill the read request, then
// we do so. Otherwise, the controller will call the underlying source's pull method to ask it
// to provide data to fulfill the read request.
//
// A critical aspect of the implementation here is that for JavaScript-backed streams, the entire
// implementation never leaves the isolate lock, and we use JavaScript promises (via jsg::Promise)
// instead of kj::Promise's to keep the implementation from having to bounce back and forth between
// the two spaces. This means that with a JavaScript-backed ReadableStream, it is possible to read
// and fully consume the stream entirely from within JavaScript without ever engaging the kj event
// loop.
//
// When you tee() a JavaScript-backed ReadableStream, the stream is put into a locked state and
// the data is funneled out through two separate "branches" (two new `ReadableStream`s).
//
// When anything reads from a tee branch, the underlying controller is asked to read from the
// underlying source. When the underlying source responds to that read request, the
// data is forwarded to all of the known branches.
//
// The story for JavaScript-backed writable streams is similar. User code passes what the
// spec calls an "underlying sink" to the `WritableStream` object constructor. This provides
// functions that are used to receive stream data.
//
// const writable = new WritableStream({
//   async start(controller) {
//     // initialize
//   },
//   async write(chunk, controller) {
//     // process the written chunk
//   },
//   async abort(reason) {},
//   async close(reason) {},
// });
//
// It is important to note that JavaScript-backed WritableStream's are *always* value
// oriented. It is up to the implementation of the underlying sink to determine if it is
// capable of doing anything with whatever type of chunk it is given.
//
// JavaScript-backed WritableStreams are backed by the WritableStreamJsController and
// WritableStreamDefaultController objects:
//
//  WritableStream -> WritableStreamJsController -> jsg::Ref<WritableStreamDefaultController>
//
// All write operations on a JavaScript-backed WritableStream are processed within the
// isolate lock using JavaScript promises instead of kj::Promises.

class ReadableStreamJsController;
class WritableStreamJsController;

// =======================================================================================
// The ReadableImpl provides implementation that is common to both the
// ReadableStreamDefaultController and the ReadableByteStreamController.
template <class Self>
class ReadableImpl {
 public:
  using Consumer = Self::QueueType::Consumer;
  using Entry = Self::QueueType::Entry;
  using StateListener = Self::QueueType::ConsumerImpl::StateListener;

  ReadableImpl(
      jsg::Lock& js, kj::Own<UnderlyingSourceImpl> source, kj::Rc<WeakRef<Self>> weakController);

  // Invokes the start algorithm to initialize the underlying source.
  void start(jsg::Lock& js, jsg::Ref<Self> self);

  // If the readable is not already closed or errored, initiates a cancellation.
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue maybeReason);

  // True if the readable is not closed, not errored, and close has not already been requested.
  bool canCloseOrEnqueue();

  // Invokes the cancel algorithm to let the underlying source know that the
  // readable has been canceled.
  void doCancel(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue reason);

  // Close the queue if we are in a state where we can be closed.
  void close(jsg::Lock& js);

  // Push a chunk of data into the queue.
  void enqueue(jsg::Lock& js, kj::Rc<Entry> entry, jsg::Ref<Self> self);

  void doClose(jsg::Lock& js);

  // If it isn't already errored or closed, errors the queue, causing all consumers to be errored
  // and detached.
  void doError(jsg::Lock& js, jsg::JsValue reason);

  // When a negative number is returned, indicates that we are above the highwatermark
  // and backpressure should be signaled.
  kj::Maybe<int> getDesiredSize();

  // Invokes the pull algorithm only if we're in a state where the queue the
  // queue is below the watermark and we actually need data right now.
  void pullIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  // Like pullIfNeeded but bypasses the shouldCallPull() check. Used for draining reads
  // which need to pull all available data regardless of backpressure settings.
  void forcePullIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  // True if the queue is current below the highwatermark.
  bool shouldCallPull();

  // True if a pull is currently in progress (the pull promise is pending).
  // Used by draining reads to determine if pumping completed synchronously.
  bool isPulling() const {
    return flags.pulling;
  }

  // The consumer can be used to read from this readables queue so long as the queue
  // is open. The consumer instance may outlive the readable but will be put into
  // a closed state or errored state when the readable is destroyed.
  kj::Own<Consumer> getConsumer(kj::Maybe<StateListener&> listener);

  // The number of consumers that exist for this readable.
  size_t consumerCount();

  void visitForGc(jsg::GcVisitor& visitor);

  kj::StringPtr jsgGetMemoryName() const;
  size_t jsgGetMemorySelfSize() const;
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

  kj::Maybe<UnderlyingSourceImpl::Tee> tryTeeSource(uint64_t limit);
  kj::Maybe<kj::Own<ReadableStreamSource>> tryReleaseSource();
  bool isInternal() const;
  StreamEncoding getPreferredEncoding();
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

 private:
  using Queue = Self::QueueType;

  // State machine for ReadableImpl:
  // Queue is the active state where the stream can accept data
  // Closed and Errored are terminal states (cannot transition back to Queue)
  //   Queue -> Closed (close() or doCancel() called)
  //   Queue -> Errored (doError() called)
  using State = StateMachine<TerminalStates<StreamStates::Closed>,
      ErrorState<StreamStates::Errored>,
      ActiveState<Queue>,
      StreamStates::Closed,
      StreamStates::Errored,
      Queue>;
  State state;

  kj::Own<UnderlyingSourceImpl> underlyingSource;

  struct PendingCancel {
    kj::Maybe<jsg::Promise<void>::Resolver> fulfiller;
    jsg::Promise<void> promise;
    JSG_MEMORY_INFO(PendingCancel) {
      tracker.trackField("fulfiller", fulfiller);
      tracker.trackField("promise", promise);
    }
  };
  kj::Maybe<PendingCancel> maybePendingCancel;

  // Weak reference to the owning controller. Shared with persistent continuation
  // callbacks so they can detect when the controller has been destroyed and bail
  // out instead of dereferencing a dangling pointer.
  kj::Rc<WeakRef<Self>> weakController;

  // Persistent pull continuation — lazily initialized on first pull dispatch.
  // Reused across all subsequent pulls to avoid per-pull OpaqueWrappable,
  // v8::Function, and lambda heap allocations.
  struct PullContinuationCallbacks {
    ReadableImpl* impl;
    kj::Rc<WeakRef<Self>> weakController;

    void thenFunc(jsg::Lock& js) {
      if (weakController->tryGet() == kj::none) return;
      impl->onPullSuccess(js);
    }

    void catchFunc(jsg::Lock& js, jsg::Value reason) {
      if (weakController->tryGet() == kj::none) return;
      impl->onPullFailure(js, kj::mv(reason));
    }
  };
  using PullContinuationType = jsg::PersistentContinuation<PullContinuationCallbacks, void, void>;

  kj::Maybe<PullContinuationType> pullContinuation;

  PullContinuationType& getPullContinuation(jsg::Lock& js);
  void onPullSuccess(jsg::Lock& js);
  void onPullFailure(jsg::Lock& js, jsg::Value reason);

  // Keeps the controller alive via jsg::Ref during a pull operation.
  kj::Maybe<jsg::Ref<Self>> pullSelf;

  struct Flags {
    uint8_t pullAgain : 1 = 0;
    uint8_t pulling : 1 = 0;
    uint8_t started : 1 = 0;
    uint8_t starting : 1 = 0;
  };
  Flags flags{};

  friend Self;
};

// Utility that provides the core implementation of WritableStreamJsController,
// separated out for consistency with ReadableStreamJsController/ReadableImpl and
// to enable it to be more easily reused should new kinds of WritableStream
// controllers be introduced.
template <class Self>
class WritableImpl {
 public:
  using PendingAbort = WritableStreamController::PendingAbort;

  struct WriteRequest {
    jsg::Promise<void>::Resolver resolver;
    jsg::JsRef<jsg::JsValue> value;
    size_t size;
    bool flush = false;  // True if this is a flush sync point (no data to write).

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(resolver, value);
    }

    JSG_MEMORY_INFO(WriteRequest) {
      tracker.trackField("resolver", resolver);
      tracker.trackField("value", value);
    }
  };

  WritableImpl(jsg::Lock& js,
      WritableStream& owner,
      kj::Own<UnderlyingSinkImpl> sink,
      jsg::Ref<AbortSignal> abortSignal,
      kj::Rc<WeakRef<Self>> weakController);

  jsg::Promise<void> abort(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue reason);

  void advanceQueueIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  jsg::Promise<void> close(jsg::Lock& js, jsg::Ref<Self> self);

  void dealWithRejection(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue reason);

  WriteRequest dequeueWriteRequest();

  void doClose(jsg::Lock& js);

  void doError(jsg::Lock& js, jsg::JsValue reason);

  void error(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue reason);

  void finishErroring(jsg::Lock& js, jsg::Ref<Self> self);

  void finishInFlightClose(
      jsg::Lock& js, jsg::Ref<Self> self, kj::Maybe<jsg::JsValue> reason = kj::none);

  void finishInFlightWrite(
      jsg::Lock& js, jsg::Ref<Self> self, kj::Maybe<jsg::JsValue> reason = kj::none);

  ssize_t getDesiredSize();

  bool isCloseQueuedOrInFlight();

  void rejectCloseAndClosedPromiseIfNeeded(jsg::Lock& js);

  kj::Maybe<WritableStreamJsController&> tryGetOwner();

  void setup(jsg::Lock& js, jsg::Ref<Self> self);

  // Puts the writable into an erroring state. This allows any in flight write or
  // close to complete before actually transitioning the writable.
  void startErroring(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue reason);

  // Notifies the Writer of the current backpressure state. If the amount of data queued
  // is equal to or above the highwatermark, then backpressure is applied.
  void updateBackpressure(jsg::Lock& js);

  // Writes a chunk to the Writable, possibly queuing the chunk in the internal buffer
  // if there are already other writes pending.
  jsg::Promise<void> write(jsg::Lock& js, jsg::Ref<Self> self, jsg::JsValue value);

  // Inserts a flush sync point into the write queue. The returned promise resolves
  // when all preceding writes have completed. If nothing is in flight, resolves
  // immediately. Flush entries are represented as WriteRequests with empty value
  // and size 0.
  jsg::Promise<void> flush(jsg::Lock& js, jsg::Ref<Self> self, MarkAsHandled markAsHandled);

  // True if the writable is in a state where new chunks can be written
  bool isWritable() const;
  bool isInternal() const;
  kj::Maybe<kj::Own<WritableStreamSink>> tryReleaseSink();
  kj::Maybe<WritableStreamSink&> tryGetSink();

  void cancelPendingWrites(jsg::Lock& js, jsg::JsValue reason);

  void visitForGc(jsg::GcVisitor& visitor);

  kj::StringPtr jsgGetMemoryName() const;
  size_t jsgGetMemorySelfSize() const;
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  struct Writable {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "writable"_kj;
  };

  // State machine for WritableImpl:
  // Writable is the active state where the stream can accept writes
  // Erroring is a transitional state - waiting for in-flight ops before erroring
  // Closed and Errored are terminal states
  //   Writable -> Erroring (startErroring() called)
  //   Writable -> Closed (finishInFlightClose() succeeds)
  //   Erroring -> Errored (finishErroring() called)
  //   Erroring -> Closed (finishInFlightClose() succeeds - close wins)
  using State = StateMachine<TerminalStates<StreamStates::Closed>,
      ErrorState<StreamStates::Errored>,
      ActiveState<Writable>,
      StreamStates::Closed,
      StreamStates::Errored,
      StreamStates::Erroring,
      Writable>;

  // Sadly, we have to use a weak ref here rather than jsg::Ref. This is because
  // the jsg::Ref<WritableStream> (via its internal WritableStreamJsController)
  // holds a strong reference to the jsg::Ref<WritableStreamDefaultController> that
  // uses this WritableImpl. This creates a strong circular reference between jsg::Refs
  // that isn't allowed. GcTracing ends up with a stack overflow as the two jsg::Refs
  // try tracing each other.
  kj::Maybe<kj::Own<WeakRef<WritableStream>>> owner;
  jsg::Ref<AbortSignal> signal;
  State state = State::template create<Writable>();

  kj::Own<UnderlyingSinkImpl> underlyingSink;

  size_t amountBuffered = 0;

  RingBuffer<WriteRequest, 8> writeRequests;

  kj::Maybe<WriteRequest> inFlightWrite;

  // Batch write: when writev is used, multiple WriteRequests are in flight simultaneously.
  // Each has its own resolver that gets resolved/rejected when the batch completes.
  struct BatchWriteRequest {
    kj::Array<jsg::Promise<void>::Resolver> resolvers;
    size_t totalSize;

    void visitForGc(jsg::GcVisitor& visitor) {
      for (auto& resolver: resolvers) {
        visitor.visit(resolver);
      }
    }
  };
  kj::Maybe<BatchWriteRequest> inFlightBatchWrite;

  kj::Maybe<jsg::Promise<void>::Resolver> inFlightClose;
  kj::Maybe<jsg::Promise<void>::Resolver> closeRequest;
  kj::Maybe<kj::Own<PendingAbort>> maybePendingAbort;

  // Keeps the controller alive via jsg::Ref during an in-flight write operation.
  // Set when a write is dispatched, cleared when the write continuation fires.
  kj::Maybe<jsg::Ref<Self>> inFlightSelf;

  // Weak reference to the owning controller. Shared with persistent continuation
  // callbacks so they can detect when the controller has been destroyed.
  kj::Rc<WeakRef<Self>> weakController;

  // Persistent write continuation — lazily initialized on first write dispatch.
  // Reused across all subsequent writes to avoid per-write OpaqueWrappable,
  // v8::Function, and lambda heap allocations.
  struct WriteContinuationCallbacks {
    WritableImpl* impl;
    kj::Rc<WeakRef<Self>> weakController;

    jsg::Promise<void> thenFunc(jsg::Lock& js) {
      if (weakController->tryGet() == kj::none) return js.resolvedPromise();
      return impl->onWriteSuccess(js);
    }

    jsg::Promise<void> catchFunc(jsg::Lock& js, jsg::Value reason) {
      if (weakController->tryGet() == kj::none) return js.rejectedPromise<void>(kj::mv(reason));
      return impl->onWriteFailure(js, kj::mv(reason));
    }
  };
  using WriteContinuationType =
      jsg::PersistentContinuation<WriteContinuationCallbacks, void, jsg::Promise<void>>;

  kj::Maybe<WriteContinuationType> writeContinuation;

  WriteContinuationType& getWriteContinuation(jsg::Lock& js);
  jsg::Promise<void> onWriteSuccess(jsg::Lock& js);
  jsg::Promise<void> onWriteFailure(jsg::Lock& js, jsg::Value reason);

  // Persistent writev continuation — used when the underlying sink supports batch writes.
  struct WritevContinuationCallbacks {
    WritableImpl* impl;
    kj::Rc<WeakRef<Self>> weakController;

    jsg::Promise<void> thenFunc(jsg::Lock& js) {
      if (weakController->tryGet() == kj::none) return js.resolvedPromise();
      return impl->onWritevSuccess(js);
    }

    jsg::Promise<void> catchFunc(jsg::Lock& js, jsg::Value reason) {
      if (weakController->tryGet() == kj::none) return js.rejectedPromise<void>(kj::mv(reason));
      return impl->onWritevFailure(js, kj::mv(reason));
    }
  };
  using WritevContinuationType =
      jsg::PersistentContinuation<WritevContinuationCallbacks, void, jsg::Promise<void>>;

  kj::Maybe<WritevContinuationType> writevContinuation;

  WritevContinuationType& getWritevContinuation(jsg::Lock& js);
  jsg::Promise<void> onWritevSuccess(jsg::Lock& js);
  jsg::Promise<void> onWritevFailure(jsg::Lock& js, jsg::Value reason);

  // Persistent close continuation — same pattern as write continuation.
  struct CloseContinuationCallbacks {
    WritableImpl* impl;
    kj::Rc<WeakRef<Self>> weakController;

    void thenFunc(jsg::Lock& js) {
      if (weakController->tryGet() == kj::none) return;
      impl->onCloseSuccess(js);
    }

    void catchFunc(jsg::Lock& js, jsg::Value reason) {
      if (weakController->tryGet() == kj::none) {
        js.throwException(kj::mv(reason));
      }
      impl->onCloseFailure(js, kj::mv(reason));
    }
  };
  using CloseContinuationType = jsg::PersistentContinuation<CloseContinuationCallbacks, void, void>;

  kj::Maybe<CloseContinuationType> closeContinuation;

  // Persistent drain continuation — used for the microtask hop when the write queue
  // has more entries after a write completes. Avoids per-drain OpaqueWrappable and
  // v8::Function allocations.
  struct DrainContinuationCallbacks {
    WritableImpl* impl;
    kj::Rc<WeakRef<Self>> weakController;

    void operator()(jsg::Lock& js) {
      if (weakController->tryGet() == kj::none) return;
      impl->onDrainNext(js);
    }
  };
  using DrainContinuationType =
      jsg::PersistentThenContinuation<DrainContinuationCallbacks, void, void>;

  kj::Maybe<DrainContinuationType> drainContinuation;

  DrainContinuationType& getDrainContinuation(jsg::Lock& js);
  void onDrainNext(jsg::Lock& js);

  CloseContinuationType& getCloseContinuation(jsg::Lock& js);
  void onCloseSuccess(jsg::Lock& js);
  void onCloseFailure(jsg::Lock& js, jsg::Value reason);
  struct Flags {
    uint8_t started : 1 = 0;
    uint8_t starting : 1 = 0;
    uint8_t backpressure : 1 = 0;
    uint8_t pedanticWpt : 1 = 0;
    uint8_t specCompliantWriter : 1 = 0;
  };
  Flags flags{};

  friend Self;
};

// =======================================================================================

// ReadableStreamDefaultController is a JavaScript object defined by the streams specification.
// It is capable of streaming any JavaScript value through it, including typed arrays and
// array buffers, but treats all values as opaque. BYOB reads are not supported.
class ReadableStreamDefaultController: public jsg::Object {
 public:
  using QueueType = ValueQueue;
  using ReadableImpl = ReadableImpl<ReadableStreamDefaultController>;

  ReadableStreamDefaultController(jsg::Lock& js, kj::Own<UnderlyingSourceImpl> source);
  ~ReadableStreamDefaultController() noexcept(false);

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason);

  void close(jsg::Lock& js);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();

  void enqueue(jsg::Lock& js, jsg::Optional<jsg::JsValue> chunk);

  void error(jsg::Lock& js, jsg::JsValue reason);

  void pull(jsg::Lock& js);

  // Like pull(), but bypasses backpressure checks. Used for draining reads
  // which need to pull all available data regardless of highWaterMark.
  void forcePull(jsg::Lock& js);

  // True if a pull is currently in progress (the pull promise is pending).
  bool isPulling() const {
    return impl.isPulling();
  }

  kj::Own<ValueQueue::Consumer> getConsumer(
      kj::Maybe<ValueQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableStreamDefaultController) {
    JSG_READONLY_PROTOTYPE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);

    JSG_TS_OVERRIDE(<R = any> {
      enqueue(chunk?: R): void;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
  }

  kj::Maybe<StreamStates::Errored> getMaybeErrorState(jsg::Lock& js);

  // Clear algorithms and persistent continuations to break circular references.
  void clearAlgorithms();

  // Break the pullSelf ref cycle without clearing the underlying source.
  // Called from ReadableState destructor to allow the controller to be freed
  // while keeping the source available for any in-progress operations.
  void breakPullCycle() {
    impl.pullSelf = kj::none;
  }

  kj::Maybe<UnderlyingSourceImpl::Tee> tryTeeSource(uint64_t limit);
  kj::Maybe<kj::Own<ReadableStreamSource>> tryReleaseSource();
  bool isInternal() const;
  StreamEncoding getPreferredEncoding();
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

 private:
  kj::Rc<WeakRef<ReadableStreamDefaultController>> weakSelf;
  kj::Maybe<IoContext&> ioContext;
  ReadableImpl impl;

  void visitForGc(jsg::GcVisitor& visitor);
};

// The ReadableStreamBYOBRequest is provided by the ReadableByteStreamController
// and is used by user code to fill a view provided by a BYOB read request.
// Because we always support autoAllocateChunkSize in the ReadableByteStreamController,
// there will always be a ReadableStreamBYOBRequest available when there is a pending
// read.
//
// The ReadableStreamBYOBRequest is either in an attached or detached state.
// The request is detached when invalidate() is called. Attempts to use the request
// after it has been detached will fail.
//
// Note that the casing of the name (e.g. "BYOB" instead of the kj style "Byob") is
// dictated by the streams specification since the class name is used as the exported
// object name.
class ReadableStreamBYOBRequest: public jsg::Object {
 public:
  ReadableStreamBYOBRequest(jsg::Lock& js,
      kj::Own<ByteQueue::ByobRequest> readRequest,
      kj::Rc<WeakRef<ReadableByteStreamController>> controller);

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamBYOBRequest);

  // getAtLeast is a non-standard Workers-specific extension that specifies
  // the minimum number of bytes the stream should fill into the view. It is
  // added to support the readAtLeast extension on the ReadableStreamBYOBReader.
  kj::Maybe<int> getAtLeast();

  kj::Maybe<jsg::JsUint8Array> getView(jsg::Lock& js);

  void invalidate(jsg::Lock& js);

  void respond(jsg::Lock& js, int bytesWritten);

  void respondWithNewView(jsg::Lock& js, jsg::JsBufferSource view);

  JSG_RESOURCE_TYPE(ReadableStreamBYOBRequest) {
    JSG_READONLY_PROTOTYPE_PROPERTY(view, getView);
    JSG_METHOD(respond);
    JSG_METHOD(respondWithNewView);

    // atLeast is an Workers-specific extension used to support the
    // readAtLeast API.
    JSG_READONLY_PROTOTYPE_PROPERTY(atLeast, getAtLeast);
  }

  bool isPartiallyFulfilled(jsg::Lock& js);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  struct Impl {
    kj::Own<ByteQueue::ByobRequest> readRequest;
    kj::Rc<WeakRef<ReadableByteStreamController>> controller;
    jsg::JsRef<jsg::JsUint8Array> view;

    size_t originalBufferByteLength;
    size_t originalByteOffsetPlusBytesFilled;

    Impl(jsg::Lock& js,
        kj::Own<ByteQueue::ByobRequest> readRequest,
        kj::Rc<WeakRef<ReadableByteStreamController>> controller);

    void updateView(jsg::Lock& js);
  };

  kj::Maybe<IoContext&> ioContext;
  kj::Maybe<Impl> maybeImpl;

  void visitForGc(jsg::GcVisitor& visitor);
};

// ReadableByteStreamController is a JavaScript object defined by the streams specification.
// It is capable of only streaming byte data through it in the form of typed arrays.
// BYOB reads are supported.
class ReadableByteStreamController: public jsg::Object {
 public:
  using QueueType = ByteQueue;
  using ReadableImpl = ReadableImpl<ReadableByteStreamController>;

  ReadableByteStreamController(jsg::Lock& js, kj::Own<UnderlyingSourceImpl> source);
  ~ReadableByteStreamController() noexcept(false);

  jsg::Ref<ReadableByteStreamController> getSelf() {
    return JSG_THIS;
  }

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason);

  void close(jsg::Lock& js);

  void enqueue(jsg::Lock& js, jsg::JsBufferSource chunk);

  void error(jsg::Lock& js, jsg::JsValue reason);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();

  kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> getByobRequest(jsg::Lock& js);

  void pull(jsg::Lock& js);

  // Like pull(), but bypasses backpressure checks. Used for draining reads
  // which need to pull all available data regardless of highWaterMark.
  void forcePull(jsg::Lock& js);

  // True if a pull is currently in progress (the pull promise is pending).
  bool isPulling() const {
    return impl.isPulling();
  }

  kj::Own<ByteQueue::Consumer> getConsumer(
      kj::Maybe<ByteQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableByteStreamController) {
    JSG_READONLY_PROTOTYPE_PROPERTY(byobRequest, getByobRequest);
    JSG_READONLY_PROTOTYPE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
    tracker.trackField("maybeByobRequest", maybeByobRequest);
  }

  // Clear algorithms and persistent continuations to break circular references.
  void clearAlgorithms();

  // Break the pullSelf ref cycle without clearing the underlying source.
  void breakPullCycle() {
    impl.pullSelf = kj::none;
  }

  kj::Maybe<UnderlyingSourceImpl::Tee> tryTeeSource(uint64_t limit);
  kj::Maybe<kj::Own<ReadableStreamSource>> tryReleaseSource();
  bool isInternal() const;
  StreamEncoding getPreferredEncoding();
  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding);

 private:
  kj::Rc<WeakRef<ReadableByteStreamController>> weakSelf;
  kj::Maybe<IoContext&> ioContext;
  ReadableImpl impl;
  kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> maybeByobRequest;

  void visitForGc(jsg::GcVisitor& visitor);

  friend class ReadableStreamBYOBRequest;
  friend class ReadableStreamJsController;
};

// =======================================================================================

// The WritableStreamDefaultController is an object defined by the stream specification.
// Writable streams are always value oriented. It is up the underlying sink implementation
// to determine whether it is capable of handling whatever type of JavaScript object it
// is given.
class WritableStreamDefaultController: public jsg::Object {
 public:
  using WritableImpl = WritableImpl<WritableStreamDefaultController>;

  explicit WritableStreamDefaultController(jsg::Lock& js,
      WritableStream& owner,
      kj::Own<UnderlyingSinkImpl> sink,
      jsg::Ref<AbortSignal> abortSignal);

  ~WritableStreamDefaultController() noexcept(false);

  jsg::Promise<void> abort(jsg::Lock& js, jsg::JsValue reason);

  jsg::Promise<void> close(jsg::Lock& js);

  void error(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  kj::Maybe<ssize_t> getDesiredSize();

  jsg::Ref<AbortSignal> getSignal();

  kj::Maybe<jsg::JsValue> isErroring(jsg::Lock& js);

  // Returns true if the stream is in the erroring state. Unlike the overload
  // that takes a lock, this method does not require a lock since it doesn't
  // return the error reason.
  bool isErroring() const;

  bool isStarted() {
    return impl.flags.started;
  }

  bool hasBackpressure() {
    return impl.flags.backpressure;
  }

  void setup(jsg::Lock& js);

  jsg::Promise<void> write(jsg::Lock& js, jsg::JsValue value);

  jsg::Promise<void> flush(jsg::Lock& js, MarkAsHandled markAsHandled);

  JSG_RESOURCE_TYPE(WritableStreamDefaultController) {
    JSG_READONLY_PROTOTYPE_PROPERTY(signal, getSignal);
    JSG_METHOD(error);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  void cancelPendingWrites(jsg::Lock& js, jsg::JsValue reason);

  // Clear algorithms to break circular references during destruction
  void clearAlgorithms();

  bool isInternal() const;
  kj::Maybe<kj::Own<WritableStreamSink>> tryReleaseSink();
  kj::Maybe<WritableStreamSink&> tryGetSink();

 private:
  kj::Rc<WeakRef<WritableStreamDefaultController>> weakSelf;
  kj::Maybe<IoContext&> ioContext;
  WritableImpl impl;

  void visitForGc(jsg::GcVisitor& visitor);
};

// =======================================================================================

// The relationship between the TransformStreamDefaultController and the
// readable/writable streams associated with it can be complicated.
// Strong references to the TransformStreamDefaultController are held by
// the *algorithms* passed into the readable and writable streams using
// JSG_VISITABLE_LAMBDAs. When those algorithms are cleared, the strong
// references holding the TransformStreamDefaultController are freed.
// However, user code can do silly things like hold the Transform controller
// long after both the readable and writable sides have been GC'ed.
class TransformStreamDefaultController: public jsg::Object {
 public:
  TransformStreamDefaultController(jsg::Lock& js, kj::Own<TransformerImpl> transformer);
  ~TransformStreamDefaultController() noexcept(false);

  void init(jsg::Lock& js, jsg::Ref<ReadableStream>& readable, jsg::Ref<WritableStream>& writable);

  // The startPromise is used by both the readable and writable sides in their respective
  // start algorithms. The promise itself is resolved within the init function when the
  // transformers own start algorithm completes.
  inline jsg::Promise<void> getStartPromise(jsg::Lock& js) {
    return startPromise.promise.whenResolved(js);
  }

  kj::Maybe<int> getDesiredSize();

  void enqueue(jsg::Lock& js, jsg::JsValue chunk);

  void error(jsg::Lock& js, jsg::JsValue reason);

  void terminate(jsg::Lock& js);

  JSG_RESOURCE_TYPE(TransformStreamDefaultController) {
    JSG_READONLY_PROTOTYPE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);
    JSG_METHOD(terminate);

    JSG_TS_OVERRIDE(<O = any> {
      enqueue(chunk?: O): void;
    });
  }

  jsg::Promise<void> write(jsg::Lock& js, jsg::JsValue chunk);
  jsg::Promise<void> writev(jsg::Lock& js, kj::Array<jsg::JsRef<jsg::JsValue>> chunks);
  jsg::Promise<void> abort(jsg::Lock& js, jsg::JsValue reason);
  jsg::Promise<void> close(jsg::Lock& js);
  jsg::Promise<void> pull(jsg::Lock& js);
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::JsValue reason);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  void errorWritableAndUnblockWrite(jsg::Lock& js, jsg::JsValue reason);
  jsg::Promise<void> performTransform(jsg::Lock& js, jsg::JsValue chunk);
  jsg::Promise<void> performTransformv(jsg::Lock& js, kj::Array<jsg::JsRef<jsg::JsValue>> chunks);
  void setBackpressure(jsg::Lock& js, UpdateBackpressure newBackpressure);

  kj::Rc<WeakRef<TransformStreamDefaultController>> weakSelf;
  kj::Maybe<IoContext&> ioContext;
  jsg::PromiseResolverPair<void> startPromise;
  kj::Own<TransformerImpl> transformer;

  kj::Maybe<ReadableStreamDefaultController&> tryGetReadableController();
  kj::Maybe<WritableStreamJsController&> tryGetWritableController();

  kj::Maybe<jsg::JsValue> getReadableErrorState(jsg::Lock& js);

  // Currently, JS-backed transform streams only support value-oriented streams.
  // In the future, that may change and this will need to become a kj::OneOf
  // that includes a ReadableByteStreamController.
  kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> readable;
  kj::Maybe<jsg::Ref<WritableStream>> writable;
  kj::Maybe<jsg::Promise<void>> maybeFinish;

  struct Flags {
    uint8_t finishStarted : 1 = 0;
    uint8_t backpressure : 1 = 0;
    uint8_t fixupBackpressure : 1 = 0;
  };
  Flags flags{};
  kj::Maybe<jsg::PromiseResolverPair<void>> maybeBackpressureChange;

  // Persistent transform continuation — used by performTransform() for the hot
  // write-through path. The success callback is a no-op, the failure callback
  // errors the stream.
  struct TransformContinuationCallbacks {
    // Strong reference keeps the controller alive through the callback.
    // This creates a cycle (controller → continuation → wrappable → callbacks → Ref),
    // which is broken when the PersistentContinuation is cleared (destructor).
    jsg::Ref<TransformStreamDefaultController> ref;

    jsg::Promise<void> thenFunc(jsg::Lock& js) {
      return js.resolvedPromise();
    }

    jsg::Promise<void> catchFunc(jsg::Lock& js, jsg::Value reason) {
      auto handle = jsg::JsValue(reason.getHandle(js));
      ref->error(js, handle);
      return js.rejectedPromise<void>(handle);
    }
  };
  using TransformContinuationType =
      jsg::PersistentContinuation<TransformContinuationCallbacks, void, jsg::Promise<void>>;

  kj::Maybe<TransformContinuationType> transformContinuation;

  TransformContinuationType& getTransformContinuation(
      jsg::Lock& js, jsg::Ref<TransformStreamDefaultController> self);

  void visitForGc(jsg::GcVisitor& visitor);
};

// =======================================================================================

jsg::Ref<WritableStream> newInternalWritableStream(jsg::Lock& js,
    IoContext& ioContext,
    kj::Own<WritableStreamSink> sink,
    kj::Maybe<kj::Own<ByteStreamObserver>> observer = kj::none,
    kj::Maybe<uint64_t> maybeHighWaterMark = kj::none);
jsg::Ref<ReadableStream> newInternalReadableStream(
    jsg::Lock& js, IoContext& ioContext, kj::Own<ReadableStreamSource> source);

}  // namespace workerd::api
