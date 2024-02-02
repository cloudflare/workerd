// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include "queue.h"
#include <workerd/jsg/function.h>
#include <workerd/util/weak-refs.h>

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
  using Consumer = typename Self::QueueType::Consumer;
  using Entry = typename Self::QueueType::Entry;
  using StateListener = typename Self::QueueType::ConsumerImpl::StateListener;

  ReadableImpl(UnderlyingSource underlyingSource,
               StreamQueuingStrategy queuingStrategy);

  // Invokes the start algorithm to initialize the underlying source.
  void start(jsg::Lock& js, jsg::Ref<Self> self);

  // If the readable is not already closed or errored, initiates a cancellation.
  jsg::Promise<void> cancel(jsg::Lock& js,
                             jsg::Ref<Self> self,
                             v8::Local<v8::Value> maybeReason);

  // True if the readable is not closed, not errored, and close has not already been requested.
  bool canCloseOrEnqueue();

  // Invokes the cancel algorithm to let the underlying source know that the
  // readable has been canceled.
  void doCancel(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  // Close the queue if we are in a state where we can be closed.
  void close(jsg::Lock& js);

  // Push a chunk of data into the queue.
  void enqueue(jsg::Lock& js, kj::Own<Entry> entry, jsg::Ref<Self> self);

  void doClose(jsg::Lock& js);

  // If it isn't already errored or closed, errors the queue, causing all consumers to be errored
  // and detached.
  void doError(jsg::Lock& js, jsg::Value reason);

  // When a negative number is returned, indicates that we are above the highwatermark
  // and backpressure should be signaled.
  kj::Maybe<int> getDesiredSize();

  // Invokes the pull algorithm only if we're in a state where the queue the
  // queue is below the watermark and we actually need data right now.
  void pullIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  // True if any of the known consumers have pending reads waiting to be
  // fulfilled. This is the case if a read is received that cannot be
  // completely fulfilled by the current contents of the queue.
  bool hasPendingReadRequests();

  // True if the queue is current below the highwatermark.
  bool shouldCallPull();

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

private:
  struct Algorithms {
    kj::Maybe<jsg::Function<UnderlyingSource::StartAlgorithm>> start;
    kj::Maybe<jsg::Function<UnderlyingSource::PullAlgorithm>> pull;
    kj::Maybe<jsg::Function<UnderlyingSource::CancelAlgorithm>> cancel;
    kj::Maybe<jsg::Function<StreamQueuingStrategy::SizeAlgorithm>> size;

    Algorithms(UnderlyingSource underlyingSource, StreamQueuingStrategy queuingStrategy)
        : start(kj::mv(underlyingSource.start)),
          pull(kj::mv(underlyingSource.pull)),
          cancel(kj::mv(underlyingSource.cancel)),
          size(kj::mv(queuingStrategy.size)) {}

    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    void clear() {
      start = kj::none;
      pull = kj::none;
      cancel = kj::none;
      size = kj::none;
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(start, pull, cancel, size);
    }
  };

  using Queue = typename Self::QueueType;

  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Queue> state;
  Algorithms algorithms;

  bool disturbed = false;
  bool pullAgain = false;
  bool pulling = false;
  bool started = false;
  bool starting = false;
  size_t highWaterMark = 1;

  struct PendingCancel {
    kj::Maybe<jsg::Promise<void>::Resolver> fulfiller;
    jsg::Promise<void> promise;
    JSG_MEMORY_INFO(PendingCancel) {
      tracker.trackField("fulfiller", fulfiller);
      tracker.trackField("promise", promise);
    }
  };
  kj::Maybe<PendingCancel> maybePendingCancel;

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
    jsg::Value value;
    size_t size;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(resolver, value);
    }

    JSG_MEMORY_INFO(WriteRequest) {
      tracker.trackField("resolver", resolver);
      tracker.trackField("value", value);
    }
  };

  WritableImpl(kj::Own<WeakRef<WritableStreamJsController>> owner);

  jsg::Promise<void> abort(jsg::Lock& js,
                            jsg::Ref<Self> self,
                            v8::Local<v8::Value> reason);

  void advanceQueueIfNeeded(jsg::Lock& js, jsg::Ref<Self> self);

  jsg::Promise<void> close(jsg::Lock& js, jsg::Ref<Self> self);

  void dealWithRejection(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  WriteRequest dequeueWriteRequest();

  void doClose(jsg::Lock& js);

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  void error(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  void finishErroring(jsg::Lock& js, jsg::Ref<Self> self);

  void finishInFlightClose(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      kj::Maybe<v8::Local<v8::Value>> reason = kj::none);

  void finishInFlightWrite(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      kj::Maybe<v8::Local<v8::Value>> reason = kj::none);

  ssize_t getDesiredSize();

  bool isCloseQueuedOrInFlight();

  void rejectCloseAndClosedPromiseIfNeeded(jsg::Lock& js);

  kj::Maybe<WritableStreamJsController&> tryGetOwner();

  void setup(
      jsg::Lock& js,
      jsg::Ref<Self> self,
      UnderlyingSink underlyingSink,
      StreamQueuingStrategy queuingStrategy);

  // Puts the writable into an erroring state. This allows any in flight write or
  // close to complete before actually transitioning the writable.
  void startErroring(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason);

  // Notifies the Writer of the current backpressure state. If the amount of data queued
  // is equal to or above the highwatermark, then backpressure is applied.
  void updateBackpressure(jsg::Lock& js);

  // Writes a chunk to the Writable, possibly queueing the chunk in the internal buffer
  // if there are already other writes pending.
  jsg::Promise<void> write(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> value);

  // True if the writable is in a state where new chunks can be written
  bool isWritable() const;

  void visitForGc(jsg::GcVisitor& visitor);

  kj::StringPtr jsgGetMemoryName() const;
  size_t jsgGetMemorySelfSize() const;
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

private:

  struct Algorithms {
    kj::Maybe<jsg::Function<UnderlyingSink::AbortAlgorithm>> abort;
    kj::Maybe<jsg::Function<UnderlyingSink::CloseAlgorithm>> close;
    kj::Maybe<jsg::Function<UnderlyingSink::WriteAlgorithm>> write;
    kj::Maybe<jsg::Function<StreamQueuingStrategy::SizeAlgorithm>> size;

    Algorithms() {};
    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    void clear() {
      abort = kj::none;
      close = kj::none;
      size = kj::none;
      write = kj::none;
    }

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(write, close, abort, size);
    }
  };

  struct Writable {};

  kj::Own<WeakRef<WritableStreamJsController>> owner;
  jsg::Ref<AbortSignal> signal;
  kj::OneOf<StreamStates::Closed,
            StreamStates::Errored,
            StreamStates::Erroring,
            Writable> state = Writable();
  Algorithms algorithms;
  bool started = false;
  bool starting = false;
  bool backpressure = false;
  size_t highWaterMark = 1;

  std::deque<WriteRequest> writeRequests;
  size_t amountBuffered = 0;

  kj::Maybe<WriteRequest> inFlightWrite;
  kj::Maybe<jsg::Promise<void>::Resolver> inFlightClose;
  kj::Maybe<jsg::Promise<void>::Resolver> closeRequest;
  kj::Maybe<PendingAbort> maybePendingAbort;

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

  ReadableStreamDefaultController(UnderlyingSource underlyingSource,
                                  StreamQueuingStrategy queuingStrategy);
  ~ReadableStreamDefaultController() noexcept(false);

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js,
                            jsg::Optional<v8::Local<v8::Value>> maybeReason);

  void close(jsg::Lock& js);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();
  bool hasPendingReadRequests();

  void enqueue(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

  void pull(jsg::Lock& js);

  kj::Own<ValueQueue::Consumer> getConsumer(
      kj::Maybe<ValueQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableStreamDefaultController) {
    JSG_READONLY_INSTANCE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);

    JSG_TS_OVERRIDE(<R = any> {
      enqueue(chunk?: R): void;
    });
  }

  kj::Own<WeakRef<ReadableStreamDefaultController>> getWeakRef();

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
  }

private:
  kj::Maybe<IoContext&> ioContext;
  ReadableImpl impl;
  kj::Own<WeakRef<ReadableStreamDefaultController>> weakRef;

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
  ReadableStreamBYOBRequest(
      jsg::Lock& js,
      kj::Own<ByteQueue::ByobRequest> readRequest,
      jsg::Ref<ReadableByteStreamController> controller);

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamBYOBRequest);

  // getAtLeast is a non-standard Workers-specific extension that specifies
  // the minimum number of bytes the stream should fill into the view. It is
  // added to support the readAtLeast extension on the ReadableStreamBYOBReader.
  kj::Maybe<int> getAtLeast();

  kj::Maybe<jsg::V8Ref<v8::Uint8Array>> getView(jsg::Lock& js);

  void invalidate(jsg::Lock& js);

  void respond(jsg::Lock& js, int bytesWritten);

  void respondWithNewView(jsg::Lock& js, jsg::BufferSource view);

  JSG_RESOURCE_TYPE(ReadableStreamBYOBRequest) {
    JSG_READONLY_INSTANCE_PROPERTY(view, getView);
    JSG_METHOD(respond);
    JSG_METHOD(respondWithNewView);

    // atLeast is an Workers-specific extension used to support the
    // readAtLeast API.
    JSG_READONLY_INSTANCE_PROPERTY(atLeast, getAtLeast);
  }

  bool isPartiallyFulfilled();

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  struct Impl {
    kj::Own<ByteQueue::ByobRequest> readRequest;
    jsg::Ref<ReadableByteStreamController> controller;
    jsg::V8Ref<v8::Uint8Array> view;

    Impl(jsg::Lock& js,
         kj::Own<ByteQueue::ByobRequest> readRequest,
         jsg::Ref<ReadableByteStreamController> controller);

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

  ReadableByteStreamController(UnderlyingSource underlyingSource,
                               StreamQueuingStrategy queuingStrategy);

  void start(jsg::Lock& js);

  jsg::Promise<void> cancel(jsg::Lock& js,
                             jsg::Optional<v8::Local<v8::Value>> maybeReason);

  void close(jsg::Lock& js);

  void enqueue(jsg::Lock& js, jsg::BufferSource chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

  bool canCloseOrEnqueue();
  bool hasBackpressure();
  kj::Maybe<int> getDesiredSize();
  bool hasPendingReadRequests();

  kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> getByobRequest(jsg::Lock& js);

  void pull(jsg::Lock& js);

  kj::Own<ByteQueue::Consumer> getConsumer(
      kj::Maybe<ByteQueue::ConsumerImpl::StateListener&> stateListener);

  JSG_RESOURCE_TYPE(ReadableByteStreamController) {
    JSG_READONLY_INSTANCE_PROPERTY(byobRequest, getByobRequest);
    JSG_READONLY_INSTANCE_PROPERTY(desiredSize, getDesiredSize);
    JSG_METHOD(close);
    JSG_METHOD(enqueue);
    JSG_METHOD(error);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("impl", impl);
    tracker.trackField("maybeByobRequest", maybeByobRequest);
  }

private:
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

  explicit WritableStreamDefaultController(kj::Own<WeakRef<WritableStreamJsController>> owner);

  jsg::Promise<void> abort(jsg::Lock& js, v8::Local<v8::Value> reason);

  jsg::Promise<void> close(jsg::Lock& js);

  void error(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason);

  ssize_t getDesiredSize();

  jsg::Ref<AbortSignal> getSignal();

  kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js);

  bool isStarted() { return impl.started; }

  void setup(
      jsg::Lock& js,
      UnderlyingSink underlyingSink,
      StreamQueuingStrategy queuingStrategy);

  jsg::Promise<void> write(jsg::Lock& js, v8::Local<v8::Value> value);

  JSG_RESOURCE_TYPE(WritableStreamDefaultController) {
    JSG_READONLY_INSTANCE_PROPERTY(signal, getSignal);
    JSG_METHOD(error);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
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
// long after both the readable and writable sides have been gc'd.
//
// We do not want to create a strong reference cycle between the various
// controllers so we use weak refs within the transform controller to
// safely reference the readable and writable sides. If either side goes
// away cleanly (using the algorithms) the weak references are cleared.
// If either side goes away due to garbage collection while the transform
// controller is still alive, the weak references are cleared. The transform
// controller then safely handles the disappearance of either side.
class TransformStreamDefaultController: public jsg::Object {
public:
  TransformStreamDefaultController(jsg::Lock& js);

  void init(jsg::Lock& js,
            jsg::Ref<ReadableStream>& readable,
            jsg::Ref<WritableStream>& writable,
            jsg::Optional<Transformer> maybeTransformer);

  // The startPromise is used by both the readable and writable sides in their respective
  // start algorithms. The promise itself is resolved within the init function when the
  // transformers own start algorithm completes.
  inline jsg::Promise<void> getStartPromise(jsg::Lock& js) {
    return startPromise.promise.whenResolved(js);
  }

  kj::Maybe<int> getDesiredSize();

  void enqueue(jsg::Lock& js, v8::Local<v8::Value> chunk);

  void error(jsg::Lock& js, v8::Local<v8::Value> reason);

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

  jsg::Promise<void> write(jsg::Lock& js, v8::Local<v8::Value> chunk);
  jsg::Promise<void> abort(jsg::Lock& js, v8::Local<v8::Value> reason);
  jsg::Promise<void> close(jsg::Lock& js);
  jsg::Promise<void> pull(jsg::Lock& js);
  jsg::Promise<void> cancel(jsg::Lock& js, v8::Local<v8::Value> reason);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  struct Algorithms {
    kj::Maybe<jsg::Function<Transformer::TransformAlgorithm>> transform;
    kj::Maybe<jsg::Function<Transformer::FlushAlgorithm>> flush;

    Algorithms() {};
    Algorithms(Algorithms&& other) = default;
    Algorithms& operator=(Algorithms&& other) = default;

    inline void clear() {
      transform = kj::none;
      flush = kj::none;
    }

    inline void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(transform, flush);
    }
  };

  void errorWritableAndUnblockWrite(jsg::Lock& js,
                                    v8::Local<v8::Value> reason);
  jsg::Promise<void> performTransform(jsg::Lock& js,
                                       v8::Local<v8::Value> chunk);
  void setBackpressure(jsg::Lock& js, bool newBackpressure);

  kj::Maybe<IoContext&> ioContext;
  jsg::PromiseResolverPair<void> startPromise;

  kj::Maybe<ReadableStreamDefaultController&> tryGetReadableController();
  kj::Maybe<WritableStreamJsController&> tryGetWritableController();

  kj::Maybe<kj::Own<WeakRef<ReadableStreamDefaultController>>> maybeReadableController;
  kj::Maybe<kj::Own<WeakRef<WritableStreamJsController>>> maybeWritableController;
  Algorithms algorithms;
  bool backpressure = false;
  kj::Maybe<jsg::PromiseResolverPair<void>> maybeBackpressureChange;

  void visitForGc(jsg::GcVisitor& visitor);
};

}  // namespace workerd::api
