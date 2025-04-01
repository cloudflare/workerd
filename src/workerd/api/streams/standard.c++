// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "standard.h"

#include "readable.h"
#include "writable.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/weak-refs.h>

#include <kj/debug.h>
#include <kj/vector.h>

namespace workerd::api {

using DefaultController = jsg::Ref<ReadableStreamDefaultController>;
using ByobController = jsg::Ref<ReadableByteStreamController>;

namespace {
struct ValueReadable;
struct ByteReadable;
}  // namespace

// =======================================================================================
// The Unlocked, Locked, ReaderLocked, and WriterLocked structs
// are used to track the current lock status of JavaScript-backed streams.
// All readable and writable streams begin in the Unlocked state. When a
// reader or writer are attached, the streams will transition into the
// ReaderLocked or WriterLocked state. When the reader is released, those
// will transition back to Unlocked.
//
// When a readable is piped to a writable, both will enter the PipeLocked state.
// (PipeLocked is defined within the ReadableLockImpl and WritableLockImpl classes
// below) When the pipe completes, both will transition back to Unlocked.
//
// When a ReadableStreamJsController is tee()'d, it will enter the locked state.

namespace {

// A utility class used by ReadableStreamJsController
// for implementing the reader lock in a consistent way (without duplicating any code).
template <typename Controller>
class ReadableLockImpl {
 public:
  using PipeController = ReadableStreamController::PipeController;
  using Reader = ReadableStreamController::Reader;

  bool isLockedToReader() const {
    return !state.template is<Unlocked>();
  }

  bool lockReader(jsg::Lock& js, Controller& self, Reader& reader);

  // See the comment for releaseReader in common.h for details on the use of maybeJs
  void releaseReader(Controller& self, Reader& reader, kj::Maybe<jsg::Lock&> maybeJs);

  bool lock();

  void onClose(jsg::Lock& js);
  void onError(jsg::Lock& js, v8::Local<v8::Value> reason);

  kj::Maybe<PipeController&> tryPipeLock(Controller& self, jsg::Ref<WritableStream> destination);

  void visitForGc(jsg::GcVisitor& visitor);

  kj::StringPtr jsgGetMemoryName() const {
    return "ReadableLockImpl"_kjc;
  }
  size_t jsgGetMemorySelfSize() const {
    return sizeof(ReadableLockImpl);
  }
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(locked, Locked) {}
      KJ_CASE_ONEOF(unlocked, Unlocked) {}
      KJ_CASE_ONEOF(pipeLocked, PipeLocked) {
        tracker.trackField("pipeLocked", pipeLocked);
      }
      KJ_CASE_ONEOF(readerLocked, ReaderLocked) {
        tracker.trackField("readerLocked", readerLocked);
      }
    }
  }

 private:
  class PipeLocked final: public PipeController {
   public:
    explicit PipeLocked(Controller& inner, jsg::Ref<WritableStream> ref)
        : inner(inner),
          writableStreamRef(kj::mv(ref)) {}

    bool isClosed() override {
      return inner.state.template is<StreamStates::Closed>();
    }

    kj::Maybe<v8::Local<v8::Value>> tryGetErrored(jsg::Lock& js) override {
      KJ_IF_SOME(errored, inner.state.template tryGet<StreamStates::Errored>()) {
        return errored.getHandle(js);
      }
      return kj::none;
    }

    void cancel(jsg::Lock& js, v8::Local<v8::Value> reason) override {
      // Cancel here returns a Promise but we do not need to propagate it.
      // We can safely drop it on the floor here.
      auto promise KJ_UNUSED = inner.cancel(js, reason);
    }

    void close(jsg::Lock& js) override {
      inner.doClose(js);
    }

    void error(jsg::Lock& js, v8::Local<v8::Value> reason) override {
      inner.doError(js, reason);
    }

    void release(jsg::Lock& js, kj::Maybe<v8::Local<v8::Value>> maybeError = kj::none) override {
      KJ_IF_SOME(error, maybeError) {
        cancel(js, error);
      }
      inner.lock.state.template init<Unlocked>();
    }

    kj::Maybe<kj::Promise<void>> tryPumpTo(WritableStreamSink& sink, bool end) override;

    jsg::Promise<ReadResult> read(jsg::Lock& js) override;

    void visitForGc(jsg::GcVisitor& visitor);

    JSG_MEMORY_INFO(PipeLocked) {
      tracker.trackField("writableStreamRef", writableStreamRef);
    }

   private:
    Controller& inner;
    jsg::Ref<WritableStream> writableStreamRef;

    friend Controller;
  };

  kj::OneOf<Locked, PipeLocked, ReaderLocked, Unlocked> state = Unlocked();
  friend Controller;
};

// A utility class used by WritableStreamJsController to implement the writer lock
// mechanism. Extracted for consistency with ReadableStreamJsController and to
// eventually allow it to be shared also with WritableStreamInternalController.
template <typename Controller>
class WritableLockImpl {
 public:
  using Writer = WritableStreamController::Writer;

  bool isLockedToWriter() const;

  bool lockWriter(jsg::Lock& js, Controller& self, Writer& writer);

  // See the comment for releaseWriter in common.h for details on the use of maybeJs
  void releaseWriter(Controller& self, Writer& writer, kj::Maybe<jsg::Lock&> maybeJs);

  void visitForGc(jsg::GcVisitor& visitor);

  bool pipeLock(WritableStream& owner, jsg::Ref<ReadableStream> source, PipeToOptions& options);
  void releasePipeLock();

  JSG_MEMORY_INFO(WritableLockImpl) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(unlocked, Unlocked) {}
      KJ_CASE_ONEOF(locked, Locked) {}
      KJ_CASE_ONEOF(writerLocked, WriterLocked) {
        tracker.trackField("writerLocked", writerLocked);
      }
      KJ_CASE_ONEOF(pipeLocked, PipeLocked) {
        tracker.trackField("pipeLocked", pipeLocked);
      }
    }
  }

 private:
  struct PipeLocked {
    ReadableStreamController::PipeController& source;
    jsg::Ref<ReadableStream> readableStreamRef;
    bool preventAbort;
    bool preventCancel;
    bool preventClose;
    bool pipeThrough;
    kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;

    kj::Maybe<jsg::Promise<void>> checkSignal(jsg::Lock& js, Controller& self);

    JSG_MEMORY_INFO(PipeLocked) {
      tracker.trackField("readableStreamRef", readableStreamRef);
      tracker.trackField("signal", maybeSignal);
    }
  };
  kj::OneOf<Unlocked, Locked, WriterLocked, PipeLocked> state = Unlocked();

  inline kj::Maybe<PipeLocked&> tryGetPipe() {
    KJ_IF_SOME(locked, state.template tryGet<PipeLocked>()) {
      return locked;
    }
    return kj::none;
  }

  friend Controller;
};

// ======================================================================================

template <typename Controller>
bool ReadableLockImpl<Controller>::lock() {
  if (isLockedToReader()) {
    return false;
  }

  state.template init<Locked>();
  return true;
}

template <typename Controller>
bool ReadableLockImpl<Controller>::lockReader(jsg::Lock& js, Controller& self, Reader& reader) {
  if (isLockedToReader()) {
    return false;
  }

  auto prp = js.newPromiseAndResolver<void>();
  prp.promise.markAsHandled(js);

  auto lock = ReaderLocked(reader, kj::mv(prp.resolver));

  if (self.state.template is<StreamStates::Closed>()) {
    maybeResolvePromise(js, lock.getClosedFulfiller());
  } else KJ_IF_SOME(errored, self.state.template tryGet<StreamStates::Errored>()) {
    maybeRejectPromise<void>(js, lock.getClosedFulfiller(), errored.getHandle(js));
  }

  state = kj::mv(lock);
  reader.attach(self, kj::mv(prp.promise));
  return true;
}

template <typename Controller>
void ReadableLockImpl<Controller>::releaseReader(
    Controller& self, Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_SOME(locked, state.template tryGet<ReaderLocked>()) {
    KJ_ASSERT(&locked.getReader() == &reader);

    KJ_IF_SOME(js, maybeJs) {
      auto reason = js.typeError("This ReadableStream reader has been released."_kj);
      KJ_SWITCH_ONEOF(self.state) {
        KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
        KJ_CASE_ONEOF(errored, StreamStates::Errored) {}
        KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
          consumer->cancelPendingReads(js, reason);
        }
        KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
          consumer->cancelPendingReads(js, reason);
        }
      }
      maybeRejectPromise<void>(js, locked.getClosedFulfiller(), reason);
    }

    // Keep the locked.clear() after the isolate and hasPendingReadRequests check above.
    // Clearing will release the references and we don't want to do that if the
    // hasPendingReadRequests check fails.
    locked.clear();

    // When maybeJs is nullptr, that means releaseReader was called when the reader is
    // being deconstructed and not as the result of explicitly calling releaseLock and
    // we do not have an isolate lock. In that case, we don't want to change the lock
    // state itself. Moving the lock above will free the lock state while keeping the
    // ReadableStream marked as locked.
    if (maybeJs != kj::none) {
      state.template init<Unlocked>();
    }
  }
}

template <typename Controller>
kj::Maybe<ReadableStreamController::PipeController&> ReadableLockImpl<Controller>::tryPipeLock(
    Controller& self, jsg::Ref<WritableStream> destination) {
  if (isLockedToReader()) {
    return kj::none;
  }
  state.template init<PipeLocked>(self, kj::mv(destination));
  return state.template get<PipeLocked>();
}

template <typename Controller>
void ReadableLockImpl<Controller>::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, Unlocked) {}
    KJ_CASE_ONEOF(locked, PipeLocked) {
      visitor.visit(locked);
    }
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      visitor.visit(locked);
    }
  }
}

template <typename Controller>
void ReadableLockImpl<Controller>::onClose(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      try {
        maybeResolvePromise(js, locked.getClosedFulfiller());
      } catch (jsg::JsExceptionThrown&) {
        // Resolving the promise could end up throwing an exception in some cases,
        // causing a jsg::JsExceptionThrown to be thrown. At this point, however,
        // we are already in the process of closing the stream and an error at this
        // point is not recoverable. Log and move on.
        LOG_NOSENTRY(ERROR, "Error resolving ReadableStream reader closed promise");
      };
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      state.template init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, Unlocked) {}
  }
}

template <typename Controller>
void ReadableLockImpl<Controller>::onError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      try {
        maybeRejectPromise<void>(js, locked.getClosedFulfiller(), reason);
      } catch (jsg::JsExceptionThrown&) {
        // Rejecting the promise could end up throwing an exception in some cases,
        // causing a jsg::JsExceptionThrown to be thrown. At this point, however,
        // we are already in the process of closing the stream and an error at this
        // point is not recoverable. Log and move on.
        LOG_NOSENTRY(ERROR, "Error rejecting ReadableStream reader closed promise");
      }
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      state.template init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, Unlocked) {}
  }
}

template <typename Controller>
kj::Maybe<kj::Promise<void>> ReadableLockImpl<Controller>::PipeLocked::tryPumpTo(
    WritableStreamSink& sink, bool end) {
  // We return nullptr here because this controller does not support kj's pumpTo.
  return kj::none;
}

template <typename Controller>
jsg::Promise<ReadResult> ReadableLockImpl<Controller>::PipeLocked::read(jsg::Lock& js) {
  return KJ_ASSERT_NONNULL(inner.read(js, kj::none));
}

template <typename Controller>
void ReadableLockImpl<Controller>::PipeLocked::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(writableStreamRef);
}

// ======================================================================================

template <typename Controller>
bool WritableLockImpl<Controller>::isLockedToWriter() const {
  return !state.template is<Unlocked>();
}

template <typename Controller>
bool WritableLockImpl<Controller>::lockWriter(jsg::Lock& js, Controller& self, Writer& writer) {
  if (isLockedToWriter()) {
    return false;
  }

  auto closedPrp = js.newPromiseAndResolver<void>();
  closedPrp.promise.markAsHandled(js);
  auto readyPrp = js.newPromiseAndResolver<void>();
  readyPrp.promise.markAsHandled(js);

  auto lock = WriterLocked(writer, kj::mv(closedPrp.resolver), kj::mv(readyPrp.resolver));

  if (self.state.template is<StreamStates::Closed>()) {
    maybeResolvePromise(js, lock.getClosedFulfiller());
    maybeResolvePromise(js, lock.getReadyFulfiller());
  } else KJ_IF_SOME(errored, self.state.template tryGet<StreamStates::Errored>()) {
    maybeRejectPromise<void>(js, lock.getClosedFulfiller(), errored.getHandle(js));
    maybeRejectPromise<void>(js, lock.getReadyFulfiller(), errored.getHandle(js));
  } else {
    if (self.isStarted()) {
      maybeResolvePromise(js, lock.getReadyFulfiller());
    }
  }

  state = kj::mv(lock);
  writer.attach(self, kj::mv(closedPrp.promise), kj::mv(readyPrp.promise));
  return true;
}

template <typename Controller>
void WritableLockImpl<Controller>::releaseWriter(
    Controller& self, Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) {
  auto& locked = state.template get<WriterLocked>();
  KJ_ASSERT(&locked.getWriter() == &writer);
  KJ_IF_SOME(js, maybeJs) {
    KJ_SWITCH_ONEOF(self.state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {}
      KJ_CASE_ONEOF(controller, jsg::Ref<WritableStreamDefaultController>) {
        controller->cancelPendingWrites(
            js, js.typeError("This WritableStream writer has been released."_kjc));
      }
    }

    maybeRejectPromise<void>(js, locked.getClosedFulfiller(),
        js.v8TypeError("This WritableStream writer has been released."_kjc));
  }
  locked.clear();

  // When maybeJs is nullptr, that means releaseWriter was called when the writer is
  // being deconstructed and not as the result of explicitly calling releaseLock and
  // we do not have an isolate lock. In that case, we don't want to change the lock
  // state itself. Moving the lock above will free the lock state while keeping the
  // WritableStream marked as locked.
  if (maybeJs != kj::none) {
    state.template init<Unlocked>();
  }
}

template <typename Controller>
bool WritableLockImpl<Controller>::pipeLock(
    WritableStream& owner, jsg::Ref<ReadableStream> source, PipeToOptions& options) {
  if (isLockedToWriter()) {
    return false;
  }

  auto& sourceLock = KJ_ASSERT_NONNULL(source->getController().tryPipeLock(owner.addRef()));

  state.template init<PipeLocked>(PipeLocked{
    .source = sourceLock,
    .readableStreamRef = kj::mv(source),
    .preventAbort = options.preventAbort.orDefault(false),
    .preventCancel = options.preventCancel.orDefault(false),
    .preventClose = options.preventClose.orDefault(false),
    .pipeThrough = options.pipeThrough,
    .maybeSignal = kj::mv(options.signal),
  });
  return true;
}

template <typename Controller>
void WritableLockImpl<Controller>::releasePipeLock() {
  if (state.template is<PipeLocked>()) {
    state.template init<Unlocked>();
  }
}

template <typename Controller>
void WritableLockImpl<Controller>::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(locked, Unlocked) {}
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, WriterLocked) {
      visitor.visit(locked);
    }
    KJ_CASE_ONEOF(locked, PipeLocked) {
      visitor.visit(locked.readableStreamRef);
      KJ_IF_SOME(signal, locked.maybeSignal) {
        visitor.visit(signal);
      }
    }
  }
}

template <typename Controller>
kj::Maybe<jsg::Promise<void>> WritableLockImpl<Controller>::PipeLocked::checkSignal(
    jsg::Lock& js, Controller& self) {
  KJ_IF_SOME(signal, maybeSignal) {
    if ((signal)->getAborted()) {
      auto reason = signal->getReason(js);
      if (!preventCancel) {
        source.release(js, v8::Local<v8::Value>(reason));
      } else {
        source.release(js);
      }
      if (!preventAbort) {
        return self.abort(js, reason).then(js, JSG_VISITABLE_LAMBDA((this, reason = reason.addRef(js), ref = self.addRef()), (reason, ref), (jsg::Lock& js) {
          return rejectedMaybeHandledPromise<void>(js, reason.getHandle(js), pipeThrough);
        }));
      }
      return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
    }
  }
  return kj::none;
}

auto maybeAddFunctor(jsg::Lock& js, auto promise, auto onSuccess, auto onFailure) {
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    return promise.then(
        js, ioContext.addFunctor(kj::mv(onSuccess)), ioContext.addFunctor(kj::mv(onFailure)));
  } else {
    return promise.then(js, kj::mv(onSuccess), kj::mv(onFailure));
  }
}

jsg::Promise<void> maybeRunAlgorithm(
    jsg::Lock& js, auto& maybeAlgorithm, auto&& onSuccess, auto&& onFailure, auto&&... args) {
  // The algorithm is a JavaScript function mapped through jsg::Function.
  // It is expected to return a Promise mapped via jsg::Promise. If the
  // function returns synchronously, the jsg::Promise wrapper ensures
  // that it is properly mapped to a jsg::Promise, but if the Promise
  // throws synchronously, we have to convert that synchronous throw
  // into a proper rejected jsg::Promise.
  KJ_IF_SOME(algorithm, maybeAlgorithm) {
    // We need two layers of tryCatch here, unfortunately. The inner layer
    // covers the algorithm implementation itself and is our typical error
    // handling path. It ensures that if the algorithm throws an exception,
    // that is properly converted in to a rejected promise that is *then*
    // handled by the onFailure handler that is passed in. The outer tryCatch
    // handles the rare and generally unexpected failure of the calls to
    // .then() itself, which can throw JS exceptions synchronously in certain
    // rare cases. For those we return a rejected promise but do not call the
    // onFailure case since such errors are generally indicative of a fatal
    // condition in the isolate (e.g. out of memory, other fatal exception, etc).
    return js.tryCatch([&] {
      if (IoContext::hasCurrent()) {
        auto& ioContext = IoContext::current();
        return js
            .tryCatch([&] { return algorithm(js, kj::fwd<decltype(args)>(args)...); },
                [&](jsg::Value&& exception) { return js.rejectedPromise<void>(kj::mv(exception)); })
            .then(js, ioContext.addFunctor(kj::mv(onSuccess)),
                ioContext.addFunctor(kj::mv(onFailure)));
      } else {
        return js
            .tryCatch([&] { return algorithm(js, kj::fwd<decltype(args)>(args)...); },
                [&](jsg::Value&& exception) {
          return js.rejectedPromise<void>(kj::mv(exception));
        }).then(js, kj::mv(onSuccess), kj::mv(onFailure));
      }
    }, [&](jsg::Value&& exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  }

  // If the algorithm does not exist, we just handle it as a success and move on.
  onSuccess(js);
  return js.resolvedPromise();
}

int getHighWaterMark(
    const UnderlyingSource& underlyingSource, const StreamQueuingStrategy& queuingStrategy) {
  bool isBytes = underlyingSource.type.map([](auto& s) { return s == "bytes"; }).orDefault(false);
  return queuingStrategy.highWaterMark.orDefault(isBytes ? 0 : 1);
}

}  // namespace

// It is possible for the controller state to be released synchronously while
// we are in the middle of a read. When that happens we need to defer the actual
// close/error state change until the read call is complete. deferControllerStateChange
// handles this for us by making sure that pendingReadCount is
// incremented until the read operation completes and deferring
// a state change until it is 0
template <typename Controller>
jsg::Promise<ReadResult> deferControllerStateChange(jsg::Lock& js,
    Controller& controller,
    kj::FunctionParam<jsg::Promise<ReadResult>()> readCallback) {
  bool decrementCount = true;
  // The readCallback and the controller.doClose(..) and controller.doError(...)
  // methods, as well as the methods can trigger JavaScript errors to be thrown
  // synchronously in some cases. We want to make sure non-fatal errors cause the
  // stream to error and only fatal cases bubble up.
  return js.tryCatch([&] {
    controller.pendingReadCount++;
    auto result = readCallback();
    decrementCount = false;
    --controller.pendingReadCount;

    KJ_ASSERT(!js.v8Isolate->IsExecutionTerminating());

    if (!controller.isReadPending()) {
      KJ_IF_SOME(state, controller.maybePendingState) {
        KJ_SWITCH_ONEOF(state) {
          KJ_CASE_ONEOF(closed, StreamStates::Closed) {
            controller.doClose(js);
          }
          KJ_CASE_ONEOF(errored, StreamStates::Errored) {
            controller.doError(js, errored.getHandle(js));
          }
        }
        controller.maybePendingState = kj::none;
      }
    }

    return kj::mv(result);
  }, [&](jsg::Value exception) -> jsg::Promise<ReadResult> {
    if (decrementCount) --controller.pendingReadCount;
    controller.doError(js, exception.getHandle(js));
    controller.maybePendingState = kj::none;
    return js.rejectedPromise<ReadResult>(kj::mv(exception));
  });
}

// The ReadableStreamJsController provides the implementation of custom
// ReadableStreams backed by a user-code provided Underlying Source. The implementation
// is fairly complicated and defined entirely by the streams specification.
//
// Another important thing to understand is that there are two types of JavaScript
// backed ReadableStreams: value-oriented, and byte-oriented.
//
// When user code uses the `new ReadableStream(underlyingSource)` constructor, the
// underlyingSource argument may have a `type` property, the value of which is either
// `undefined`, the empty string, or the string value `'bytes'`. If the underlyingSource
// argument is not given, the default value of `type` is `undefined`. If `type` is
// `undefined` or the empty string, the ReadableStream is value-oriented. If `type` is
// exactly equal to `'bytes'`, the ReadableStream is byte-oriented.
//
// For value-oriented streams, any JavaScript value can be pushed through the stream,
// and the stream will only support use of the ReadableStreamDefaultReader to consume
// the stream data.
//
// For byte-oriented streams, only byte data (as provided by `ArrayBufferView`s) can
// be pushed through the stream. All byte-oriented streams support using both
// ReadableStreamDefaultReader and ReadableStreamBYOBReader to consume the stream
// data.
//
// When the ReadableStreamJsController::setup() method is called the type
// of stream is determined, and the controller will create an instance of either
// jsg::Ref<ReadableStreamDefaultController> or jsg::Ref<ReadableByteStreamController>.
// These are the objects that are actually passed on to the user-code's Underlying Source
// implementation.
class ReadableStreamJsController final: public ReadableStreamController {
 public:
  using ReadableLockImpl = ReadableLockImpl<ReadableStreamJsController>;

  KJ_DISALLOW_COPY_AND_MOVE(ReadableStreamJsController);

  explicit ReadableStreamJsController();
  explicit ReadableStreamJsController(StreamStates::Closed closed);
  explicit ReadableStreamJsController(StreamStates::Errored errored);
  explicit ReadableStreamJsController(jsg::Lock& js, ValueReadable& consumer);
  explicit ReadableStreamJsController(jsg::Lock& js, ByteReadable& consumer);

  jsg::Ref<ReadableStream> addRef() override;

  void setup(jsg::Lock& js,
      jsg::Optional<UnderlyingSource> maybeUnderlyingSource,
      jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) override;

  // Signals that this ReadableStream is no longer interested in the underlying
  // data source. Whether this cancels the underlying data source also depends
  // on whether or not there are other ReadableStreams still attached to it.
  // This operation is terminal. Once called, even while the returned Promise
  // is still pending, the ReadableStream will be no longer usable and any
  // data still in the queue will be dropped. Pending read requests will be
  // rejected if a reason is given, or resolved with no data otherwise.
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) override;

  void doClose(jsg::Lock& js);

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  bool canCloseOrEnqueue();
  bool hasBackpressure();

  bool isByteOriented() const override;

  bool isDisturbed() override;

  bool isClosedOrErrored() const override;

  bool isClosed() const override;

  bool isLockedToReader() const override;

  bool lockReader(jsg::Lock& js, Reader& reader) override;

  kj::Maybe<v8::Local<v8::Value>> isErrored(jsg::Lock& js);

  kj::Maybe<int> getDesiredSize();

  jsg::Promise<void> pipeTo(
      jsg::Lock& js, WritableStreamController& destination, PipeToOptions options) override;

  kj::Promise<DeferredProxy<void>> pumpTo(
      jsg::Lock& js, kj::Own<WritableStreamSink>, bool end) override;

  kj::Maybe<jsg::Promise<ReadResult>> read(
      jsg::Lock& js, kj::Maybe<ByobOptions> byobOptions) override;

  // See the comment for releaseReader in common.h for details on the use of maybeJs
  void releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) override;

  void setOwnerRef(ReadableStream& stream) override;

  Tee tee(jsg::Lock& js) override;

  kj::Maybe<PipeController&> tryPipeLock(jsg::Ref<WritableStream> destination) override;

  void visitForGc(jsg::GcVisitor& visitor) override;

  kj::Maybe<kj::OneOf<DefaultController, ByobController>> getController();

  jsg::Promise<jsg::BufferSource> readAllBytes(jsg::Lock& js, uint64_t limit) override;
  jsg::Promise<kj::String> readAllText(jsg::Lock& js, uint64_t limit) override;

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding encoding) override;

  kj::Own<ReadableStreamController> detach(jsg::Lock& js, bool ignoreDisturbed) override;

  void setPendingClosure() override {
    KJ_UNIMPLEMENTED("only implemented for WritableStreamInternalController");
  }

  kj::StringPtr jsgGetMemoryName() const override;
  size_t jsgGetMemorySelfSize() const override;
  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override;

 private:
  // If the stream was created within the scope of a request, we want to treat it as I/O
  // and make sure it is not advanced from the scope of a different request.
  kj::Maybe<IoContext&> ioContext;
  kj::Maybe<ReadableStream&> owner;

  kj::OneOf<StreamStates::Closed,
      StreamStates::Errored,
      kj::Own<ValueReadable>,
      kj::Own<ByteReadable>>
      state = StreamStates::Closed();

  kj::Maybe<uint64_t> expectedLength = kj::none;

  // The lock state is separate because a closed or errored stream can still be locked.
  ReadableLockImpl lock;

  bool disturbed = false;
  size_t pendingReadCount = 0;
  kj::Maybe<kj::OneOf<StreamStates::Closed, StreamStates::Errored>> maybePendingState = kj::none;

  template <typename T>
  jsg::Promise<T> readAll(jsg::Lock& js, uint64_t limit);

  void setPendingState(kj::OneOf<StreamStates::Closed, StreamStates::Errored> pending) {
    if (maybePendingState == kj::none) {
      maybePendingState = kj::mv(pending);
    }
  }

  bool isReadPending() const {
    return pendingReadCount > 0;
  }

  friend ReadableLockImpl;
  friend ReadableLockImpl::PipeLocked;

  template <typename Controller>
  friend jsg::Promise<ReadResult> deferControllerStateChange(jsg::Lock& js,
      Controller& controller,
      kj::FunctionParam<jsg::Promise<ReadResult>()> readCallback);
};

// The WritableStreamJsController provides the implementation of custom
// WritableStream's backed by a user-code provided Underlying Sink. The implementation
// is fairly complicated and defined entirely by the streams specification.
class WritableStreamJsController final: public WritableStreamController {
 public:
  using WritableLockImpl = WritableLockImpl<WritableStreamJsController>;

  using Controller = jsg::Ref<WritableStreamDefaultController>;

  explicit WritableStreamJsController();

  explicit WritableStreamJsController(StreamStates::Closed closed);

  explicit WritableStreamJsController(StreamStates::Errored errored);

  KJ_DISALLOW_COPY_AND_MOVE(WritableStreamJsController);

  jsg::Promise<void> abort(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) override;

  jsg::Ref<WritableStream> addRef() override;

  jsg::Promise<void> close(jsg::Lock& js, bool markAsHandled = false) override;

  jsg::Promise<void> flush(jsg::Lock& js, bool markAsHandled = false) override {
    KJ_UNIMPLEMENTED("expected WritableStreamInternalController implementation to be enough");
  }

  void doClose(jsg::Lock& js);

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason);

  kj::Maybe<int> getDesiredSize() override;

  kj::Maybe<v8::Local<v8::Value>> isErroring(jsg::Lock& js) override;
  kj::Maybe<v8::Local<v8::Value>> isErroredOrErroring(jsg::Lock& js);

  bool isLocked() const;

  bool isLockedToWriter() const override;

  bool isStarted();

  inline bool isWritable() const {
    return state.is<Controller>();
  }

  bool lockWriter(jsg::Lock& js, Writer& writer) override;

  void maybeRejectReadyPromise(jsg::Lock& js, v8::Local<v8::Value> reason);

  void maybeResolveReadyPromise(jsg::Lock& js);

  // See the comment for releaseWriter in common.h for details on the use of maybeJs
  void releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) override;

  kj::Maybe<kj::Own<WritableStreamSink>> removeSink(jsg::Lock& js) override;
  void detach(jsg::Lock& js) override;

  void setOwnerRef(WritableStream& stream) override;

  void setup(jsg::Lock& js,
      jsg::Optional<UnderlyingSink> maybeUnderlyingSink,
      jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) override;

  kj::Maybe<jsg::Promise<void>> tryPipeFrom(
      jsg::Lock& js, jsg::Ref<ReadableStream> source, PipeToOptions options) override;

  void updateBackpressure(jsg::Lock& js, bool backpressure);

  jsg::Promise<void> write(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> value) override;

  void visitForGc(jsg::GcVisitor& visitor) override;

  bool isClosedOrClosing() override;
  bool isErrored() override;

  inline bool isByteOriented() const override {
    return false;
  }

  void setPendingClosure() override {
    KJ_UNIMPLEMENTED("only implemented for WritableStreamInternalController");
  }

  kj::StringPtr jsgGetMemoryName() const override;
  size_t jsgGetMemorySelfSize() const override;
  void jsgGetMemoryInfo(jsg::MemoryTracker& info) const override;

 private:
  jsg::Promise<void> pipeLoop(jsg::Lock& js);

  kj::Maybe<IoContext&> ioContext;
  kj::Maybe<WritableStream&> owner;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Controller> state = StreamStates::Closed();
  WritableLockImpl lock;
  kj::Maybe<jsg::Promise<void>> maybeAbortPromise;

  friend WritableLockImpl;
};

kj::Own<ReadableStreamController> newReadableStreamJsController() {
  return kj::heap<ReadableStreamJsController>();
}

kj::Own<WritableStreamController> newWritableStreamJsController() {
  return kj::heap<WritableStreamJsController>();
}

template <typename Self>
ReadableImpl<Self>::ReadableImpl(
    UnderlyingSource underlyingSource, StreamQueuingStrategy queuingStrategy)
    : state(Queue(getHighWaterMark(underlyingSource, queuingStrategy))),
      algorithms(kj::mv(underlyingSource), kj::mv(queuingStrategy)) {}

template <typename Self>
void ReadableImpl<Self>::start(jsg::Lock& js, jsg::Ref<Self> self) {
  KJ_ASSERT(!started && !starting);
  starting = true;

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    started = true;
    starting = false;
    pullIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
        started = true;
        starting = false;
        doError(js, kj::mv(reason));
      });

  maybeRunAlgorithm(js, algorithms.start, kj::mv(onSuccess), kj::mv(onFailure), kj::mv(self));
  algorithms.start = kj::none;
}

template <typename Self>
size_t ReadableImpl<Self>::consumerCount() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return 0;
    }
    KJ_CASE_ONEOF(queue, Queue) {
      return queue.getConsumerCount();
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
jsg::Promise<void> ReadableImpl<Self>::cancel(
    jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // We are already closed. There's nothing to cancel.
      // This shouldn't happen but we handle the case anyway, just to be safe.
      return js.resolvedPromise();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      // We are already errored. There's nothing to cancel.
      // This shouldn't happen but we handle the case anyway, just to be safe.
      return js.rejectedPromise<void>(errored.getHandle(js));
    }
    KJ_CASE_ONEOF(queue, Queue) {
      size_t consumerCount = queue.getConsumerCount();
      if (consumerCount > 1) {
        // If there is more than 1 consumer, then we just return here with an
        // immediately resolved promise. The consumer will remove itself,
        // canceling its interest in the underlying source but we do not yet
        // want to cancel the underlying source since there are still other
        // consumers that want data.
        return js.resolvedPromise();
      }

      // Otherwise, there should be exactly one consumer at this point.
      KJ_ASSERT(consumerCount == 1);
      KJ_IF_SOME(pendingCancel, maybePendingCancel) {
        // If we're already waiting for cancel to complete, just return the
        // already existing pending promise.
        // This shouldn't happen but we handle the case anyway, just to be safe.
        return pendingCancel.promise.whenResolved(js);
      }

      auto prp = js.newPromiseAndResolver<void>();
      maybePendingCancel = PendingCancel{
        .fulfiller = kj::mv(prp.resolver),
        .promise = kj::mv(prp.promise),
      };
      auto promise = KJ_ASSERT_NONNULL(maybePendingCancel).promise.whenResolved(js);
      doCancel(js, kj::mv(self), reason);
      return kj::mv(promise);
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
bool ReadableImpl<Self>::canCloseOrEnqueue() {
  return state.template is<Queue>();
}

// doCancel() is triggered by cancel() being called, which is an explicit signal from
// the ReadableStream that we don't care about the data this controller provides any
// more. We don't need to notify the consumers because we presume they already know
// that they called cancel. What we do want to do here, tho, is close the implementation
// and trigger the cancel algorithm.
template <typename Self>
void ReadableImpl<Self>::doCancel(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  state.template init<StreamStates::Closed>();

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    doClose(js);
    KJ_IF_SOME(pendingCancel, maybePendingCancel) {
    maybeResolvePromise(js, pendingCancel.fulfiller);
    } else {
    // Else block to avert dangling else compiler warning.
    }
  });
  auto onFailure = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
        // We do not call doError() here because there's really no point. Everything
        // that cares about the state of this controller impl has signaled that it
        // no longer cares and has gone away.
        doClose(js);
        KJ_IF_SOME(pendingCancel, maybePendingCancel) {
        maybeRejectPromise<void>(js, pendingCancel.fulfiller, reason.getHandle(js));
        } else {
        // Else block to avert dangling else compiler warning.
        }
      });

  maybeRunAlgorithm(js, algorithms.cancel, kj::mv(onSuccess), kj::mv(onFailure), reason);
}

template <typename Self>
void ReadableImpl<Self>::enqueue(jsg::Lock& js, kj::Own<Entry> entry, jsg::Ref<Self> self) {
  JSG_REQUIRE(canCloseOrEnqueue(), TypeError, "This ReadableStream is closed.");
  KJ_DEFER(pullIfNeeded(js, kj::mv(self)));
  auto& queue = state.template get<Queue>();
  queue.push(js, kj::mv(entry));
}

template <typename Self>
void ReadableImpl<Self>::close(jsg::Lock& js) {
  JSG_REQUIRE(canCloseOrEnqueue(), TypeError, "This ReadableStream is closed.");
  auto& queue = state.template get<Queue>();

  if (queue.hasPartiallyFulfilledRead()) {
    auto error =
        js.v8Ref(js.v8TypeError("This ReadableStream was closed with a partial read pending."));
    doError(js, error.addRef(js));
    js.throwException(kj::mv(error));
    return;
  }

  queue.close(js);

  state.template init<StreamStates::Closed>();
  doClose(js);
}

template <typename Self>
void ReadableImpl<Self>::doClose(jsg::Lock& js) {
  // The state should have already been set to closed.
  KJ_ASSERT(state.template is<StreamStates::Closed>());
  algorithms.clear();
}

template <typename Self>
void ReadableImpl<Self>::doError(jsg::Lock& js, jsg::Value reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // We're already closed, so we really don't care if there was an error. Do nothing.
      return;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      // We're already errored, so we really don't care if there was an error. Do nothing.
      return;
    }
    KJ_CASE_ONEOF(queue, Queue) {
      queue.error(js, reason.addRef(js));
      state = kj::mv(reason);
      algorithms.clear();
      return;
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
kj::Maybe<int> ReadableImpl<Self>::getDesiredSize() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(queue, Queue) {
      return queue.desiredSize();
    }
  }
  KJ_UNREACHABLE;
}

// We should call pull if any of the consumers known to the queue have read requests or
// we haven't yet signalled backpressure.
template <typename Self>
bool ReadableImpl<Self>::shouldCallPull() {
  return canCloseOrEnqueue() &&
      (state.template get<Queue>().wantsRead() || getDesiredSize().orDefault(0) > 0);
}

template <typename Self>
void ReadableImpl<Self>::pullIfNeeded(jsg::Lock& js, jsg::Ref<Self> self) {
  // Determining if we need to pull is fairly complicated. All of the following
  // must hold true:
  if (!shouldCallPull()) {
    return;
  }

  if (pulling) {
    pullAgain = true;
    return;
  }
  KJ_ASSERT(!pullAgain);
  pulling = true;

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    pulling = false;
    if (pullAgain) {
    pullAgain = false;
    pullIfNeeded(js, kj::mv(self));
    }
  });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
        pulling = false;
        doError(js, kj::mv(reason));
      });

  maybeRunAlgorithm(js, algorithms.pull, kj::mv(onSuccess), kj::mv(onFailure), self.addRef());
}

template <typename Self>
void ReadableImpl<Self>::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      visitor.visit(errored);
    }
    KJ_CASE_ONEOF(queue, Queue) {
      visitor.visit(queue);
    }
  }
  KJ_IF_SOME(pendingCancel, maybePendingCancel) {
    visitor.visit(pendingCancel.fulfiller, pendingCancel.promise);
  }
  visitor.visit(algorithms);
}

template <typename Self>
kj::Own<typename ReadableImpl<Self>::Consumer> ReadableImpl<Self>::getConsumer(
    kj::Maybe<ReadableImpl<Self>::StateListener&> listener) {
  auto& queue = state.template get<Queue>();
  return kj::heap<typename ReadableImpl<Self>::Consumer>(queue, listener);
}

// ======================================================================================

template <typename Self>
WritableImpl<Self>::WritableImpl(jsg::Lock& js, WritableStream& owner)
    : owner(owner.addWeakRef()),
      signal(js.alloc<AbortSignal>()) {}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::abort(
    jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  signal->triggerAbort(js, jsg::JsValue(reason));

  // We have to check this again after the AbortSignal is triggered.
  if (state.template is<StreamStates::Closed>() || state.template is<StreamStates::Errored>()) {
    return js.resolvedPromise();
  }

  KJ_IF_SOME(pendingAbort, maybePendingAbort) {
    // Notice here that, per the spec, the reason given in this call of abort is
    // intentionally ignored if there is already an abort pending.
    return pendingAbort->whenResolved(js);
  }

  bool wasAlreadyErroring = false;
  if (state.template is<StreamStates::Erroring>()) {
    wasAlreadyErroring = true;
    reason = js.v8Undefined();
  }

  KJ_DEFER(if (!wasAlreadyErroring) { startErroring(js, kj::mv(self), reason); });

  maybePendingAbort = kj::heap<PendingAbort>(js, reason, wasAlreadyErroring);
  return KJ_ASSERT_NONNULL(maybePendingAbort)->whenResolved(js);
}

template <typename Self>
kj::Maybe<WritableStreamJsController&> WritableImpl<Self>::tryGetOwner() {
  KJ_IF_SOME(o, owner) {
    return o->tryGet().map([](WritableStream& owner) -> WritableStreamJsController& {
      return static_cast<WritableStreamJsController&>(owner.getController());
    });
  }
  return kj::none;
}

template <typename Self>
ssize_t WritableImpl<Self>::getDesiredSize() {
  return highWaterMark - amountBuffered;
}

template <typename Self>
void WritableImpl<Self>::advanceQueueIfNeeded(jsg::Lock& js, jsg::Ref<Self> self) {
  if (!started || inFlightWrite != kj::none) {
    return;
  }
  KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());

  if (state.template is<StreamStates::Erroring>()) {
    return finishErroring(js, kj::mv(self));
  }

  if (writeRequests.empty()) {
    if (closeRequest != kj::none) {
      KJ_ASSERT(inFlightClose == kj::none);
      KJ_ASSERT_NONNULL(closeRequest);
      inFlightClose = kj::mv(closeRequest);

      auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
          (jsg::Lock& js) { finishInFlightClose(js, kj::mv(self)); });

      auto onFailure = JSG_VISITABLE_LAMBDA(
          (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
            finishInFlightClose(js, kj::mv(self), reason.getHandle(js));
          });

      maybeRunAlgorithm(js, algorithms.close, kj::mv(onSuccess), kj::mv(onFailure));
    }
    return;
  }

  KJ_ASSERT(inFlightWrite == kj::none);
  auto req = dequeueWriteRequest();
  auto value = req.value.addRef(js);
  auto size = req.size;
  inFlightWrite = kj::mv(req);

  auto onSuccess =
      JSG_VISITABLE_LAMBDA((this, self = self.addRef(), size), (self), (jsg::Lock& js) {
        amountBuffered -= size;
        finishInFlightWrite(js, self.addRef());
        KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());
        if (!isCloseQueuedOrInFlight() && isWritable()) {
        updateBackpressure(js);
        }
        advanceQueueIfNeeded(js, kj::mv(self));
      });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef(), size), (self), (jsg::Lock& js, jsg::Value reason) {
        amountBuffered -= size;
        finishInFlightWrite(js, kj::mv(self), reason.getHandle(js));
      });

  maybeRunAlgorithm(js, algorithms.write, kj::mv(onSuccess), kj::mv(onFailure), value.getHandle(js),
      self.addRef());
}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::close(jsg::Lock& js, jsg::Ref<Self> self) {
  KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());
  JSG_REQUIRE(
      !isCloseQueuedOrInFlight(), TypeError, "Cannot close a writer that is already being closed");
  auto prp = js.newPromiseAndResolver<void>();
  closeRequest = kj::mv(prp.resolver);

  if (backpressure && isWritable()) {
    KJ_IF_SOME(owner, tryGetOwner()) {
      owner.maybeResolveReadyPromise(js);
    }
  }

  advanceQueueIfNeeded(js, kj::mv(self));

  return kj::mv(prp.promise);
}

template <typename Self>
void WritableImpl<Self>::dealWithRejection(
    jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  if (isWritable()) {
    return startErroring(js, kj::mv(self), reason);
  }
  KJ_ASSERT(state.template is<StreamStates::Erroring>());
  finishErroring(js, kj::mv(self));
}

template <typename Self>
typename WritableImpl<Self>::WriteRequest WritableImpl<Self>::dequeueWriteRequest() {
  auto write = kj::mv(writeRequests.front());
  writeRequests.pop_front();
  return kj::mv(write);
}

template <typename Self>
void WritableImpl<Self>::doClose(jsg::Lock& js) {
  KJ_ASSERT(closeRequest == kj::none);
  KJ_ASSERT(inFlightClose == kj::none);
  KJ_ASSERT(inFlightWrite == kj::none);
  KJ_ASSERT(maybePendingAbort == kj::none);
  KJ_ASSERT(writeRequests.empty());
  state.template init<StreamStates::Closed>();
  algorithms.clear();

  KJ_IF_SOME(owner, tryGetOwner()) {
    owner.doClose(js);
  }
}

template <typename Self>
void WritableImpl<Self>::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_ASSERT(closeRequest == kj::none);
  KJ_ASSERT(inFlightClose == kj::none);
  KJ_ASSERT(inFlightWrite == kj::none);
  KJ_ASSERT(maybePendingAbort == kj::none);
  KJ_ASSERT(writeRequests.empty());
  state = js.v8Ref(reason);
  algorithms.clear();

  KJ_IF_SOME(owner, tryGetOwner()) {
    owner.doError(js, reason);
  }
}

template <typename Self>
void WritableImpl<Self>::error(jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  if (isWritable()) {
    algorithms.clear();
    startErroring(js, kj::mv(self), reason);
  }
}

template <typename Self>
void WritableImpl<Self>::finishErroring(jsg::Lock& js, jsg::Ref<Self> self) {
  auto erroring = kj::mv(KJ_ASSERT_NONNULL(state.template tryGet<StreamStates::Erroring>()));
  auto reason = erroring.reason.getHandle(js);
  KJ_ASSERT(inFlightWrite == kj::none);
  KJ_ASSERT(inFlightClose == kj::none);
  state.template init<StreamStates::Errored>(kj::mv(erroring.reason));

  while (!writeRequests.empty()) {
    dequeueWriteRequest().resolver.reject(js, reason);
  }
  KJ_ASSERT(writeRequests.empty());

  KJ_IF_SOME(pendingAbort, maybePendingAbort) {
    if (pendingAbort->reject) {
      pendingAbort->fail(js, reason);
      return rejectCloseAndClosedPromiseIfNeeded(js);
    }

    auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
      auto& pendingAbort = KJ_ASSERT_NONNULL(maybePendingAbort);
      pendingAbort->reject = false;
      pendingAbort->complete(js);
      rejectCloseAndClosedPromiseIfNeeded(js);
    });

    auto onFailure = JSG_VISITABLE_LAMBDA(
        (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
          auto& pendingAbort = KJ_ASSERT_NONNULL(maybePendingAbort);
          pendingAbort->fail(js, reason.getHandle(js));
          rejectCloseAndClosedPromiseIfNeeded(js);
        });

    maybeRunAlgorithm(js, algorithms.abort, kj::mv(onSuccess), kj::mv(onFailure), reason);
    return;
  }
  rejectCloseAndClosedPromiseIfNeeded(js);
}

template <typename Self>
void WritableImpl<Self>::finishInFlightClose(
    jsg::Lock& js, jsg::Ref<Self> self, kj::Maybe<v8::Local<v8::Value>> maybeReason) {
  algorithms.clear();
  KJ_ASSERT_NONNULL(inFlightClose);
  KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());

  KJ_IF_SOME(reason, maybeReason) {
    maybeRejectPromise<void>(js, inFlightClose, reason);

    KJ_IF_SOME(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
      pendingAbort->fail(js, reason);
    }

    return dealWithRejection(js, kj::mv(self), reason);
  }

  maybeResolvePromise(js, inFlightClose);

  if (state.template is<StreamStates::Erroring>()) {
    KJ_IF_SOME(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
      pendingAbort->reject = false;
      pendingAbort->complete(js);
    }
  }
  KJ_ASSERT(maybePendingAbort == kj::none);

  state.template init<StreamStates::Closed>();
  doClose(js);
}

template <typename Self>
void WritableImpl<Self>::finishInFlightWrite(
    jsg::Lock& js, jsg::Ref<Self> self, kj::Maybe<v8::Local<v8::Value>> maybeReason) {
  auto& write = KJ_ASSERT_NONNULL(inFlightWrite);

  KJ_IF_SOME(reason, maybeReason) {
    write.resolver.reject(js, reason);
    inFlightWrite = kj::none;
    KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());
    return dealWithRejection(js, kj::mv(self), reason);
  }

  write.resolver.resolve(js);
  inFlightWrite = kj::none;
}

template <typename Self>
bool WritableImpl<Self>::isCloseQueuedOrInFlight() {
  return closeRequest != kj::none || inFlightClose != kj::none;
}

template <typename Self>
void WritableImpl<Self>::rejectCloseAndClosedPromiseIfNeeded(jsg::Lock& js) {
  algorithms.clear();
  auto reason = KJ_ASSERT_NONNULL(state.template tryGet<StreamStates::Errored>()).getHandle(js);
  maybeRejectPromise<void>(js, closeRequest, reason);
  PendingAbort::dequeue(maybePendingAbort);
  doError(js, reason);
}

template <typename Self>
void WritableImpl<Self>::setup(jsg::Lock& js,
    jsg::Ref<Self> self,
    UnderlyingSink underlyingSink,
    StreamQueuingStrategy queuingStrategy) {
  KJ_ASSERT(!started && !starting);
  starting = true;

  highWaterMark = queuingStrategy.highWaterMark.orDefault(1);
  auto startAlgorithm = kj::mv(underlyingSink.start);
  algorithms.write = kj::mv(underlyingSink.write);
  algorithms.close = kj::mv(underlyingSink.close);
  algorithms.abort = kj::mv(underlyingSink.abort);
  algorithms.size = kj::mv(queuingStrategy.size);

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());

    if (isWritable()) {
    // Only resolve the ready promise if an abort is not pending.
    // It will have been rejected already.
    KJ_IF_SOME(owner, tryGetOwner()) {
    owner.maybeResolveReadyPromise(js);
    } else {
    // Else block to avert dangling else compiler warning.
    }
    }

    started = true;
    starting = false;
    advanceQueueIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
        auto handle = reason.getHandle(js);
        KJ_ASSERT(isWritable() || state.template is<StreamStates::Erroring>());
        KJ_IF_SOME(owner, tryGetOwner()) {
        owner.maybeRejectReadyPromise(js, handle);
        } else {
        // Else block to avert dangling else compiler warning.
        }
        started = true;
        starting = false;
        dealWithRejection(js, kj::mv(self), handle);
      });

  backpressure = getDesiredSize() < 0;

  maybeRunAlgorithm(js, startAlgorithm, kj::mv(onSuccess), kj::mv(onFailure), self.addRef());
}

template <typename Self>
void WritableImpl<Self>::startErroring(
    jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> reason) {
  KJ_ASSERT(isWritable());
  KJ_IF_SOME(owner, tryGetOwner()) {
    owner.maybeRejectReadyPromise(js, reason);
  }
  state.template init<StreamStates::Erroring>(js.v8Ref(reason));
  if (inFlightWrite == kj::none && inFlightClose == kj::none && started) {
    finishErroring(js, kj::mv(self));
  }
}

template <typename Self>
void WritableImpl<Self>::updateBackpressure(jsg::Lock& js) {
  KJ_ASSERT(isWritable());
  KJ_ASSERT(!isCloseQueuedOrInFlight());
  bool bp = getDesiredSize() < 0;

  // We use a variable multiplier here in order to prevent the warning from being too
  // spammy in the default case. The default high water mark for a standard writable stream
  // is 1, which means we'd end up emitting a warning every time the buffer size is greater
  // than 2, which is not very helpful. Instead, for any highWaterMark < 10, we'll configure
  // a multiplier of 10, and for any highWaterMark >= 10, we'll configure a multiplier of 2.
  // This is fairly arbitrary and may need to be tuned further.
  int warningMultiplier = highWaterMark <= 10 ? 10 : 2;

  if (warnAboutExcessiveBackpressure && (amountBuffered >= warningMultiplier * highWaterMark)) {
    excessiveBackpressureWarningCount++;
    auto warning = kj::str("A WritableStream is experiencing excessive backpressure. "
                           "The current write buffer size is ",
        amountBuffered, ", which is greater than or equal to ", warningMultiplier,
        " times the high water mark of ", highWaterMark,
        ". Streams that consistently exceed the configured high water ",
        "mark may cause excessive memory usage. ", "(Count ", excessiveBackpressureWarningCount,
        ")");
    js.logWarning(warning);
    warnAboutExcessiveBackpressure = false;
  }

  if (!bp) warnAboutExcessiveBackpressure = true;

  if (bp != backpressure) {
    backpressure = bp;
    KJ_IF_SOME(owner, tryGetOwner()) {
      owner.updateBackpressure(js, backpressure);
    }
  }
}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::write(
    jsg::Lock& js, jsg::Ref<Self> self, v8::Local<v8::Value> value) {

  size_t size = 1;
  KJ_IF_SOME(sizeFunc, algorithms.size) {
    kj::Maybe<jsg::Value> failure;
    js.tryCatch([&] { size = sizeFunc(js, value); }, [&](jsg::Value exception) {
      startErroring(js, self.addRef(), exception.getHandle(js));
      failure = kj::mv(exception);
    });
    KJ_IF_SOME(exception, failure) {
      return js.rejectedPromise<void>(kj::mv(exception));
    }
  }

  KJ_IF_SOME(error, state.template tryGet<StreamStates::Errored>()) {
    return js.rejectedPromise<void>(error.addRef(js));
  }

  if (isCloseQueuedOrInFlight() || state.template is<StreamStates::Closed>()) {
    return js.rejectedPromise<void>(js.v8TypeError("This ReadableStream is closed."_kj));
  }

  KJ_IF_SOME(erroring, state.template tryGet<StreamStates::Erroring>()) {
    return js.rejectedPromise<void>(erroring.reason.addRef(js));
  }

  KJ_ASSERT(isWritable());

  auto prp = js.newPromiseAndResolver<void>();
  writeRequests.push_back(WriteRequest{
    .resolver = kj::mv(prp.resolver),
    .value = js.v8Ref(value),
    .size = size,
  });
  amountBuffered += size;

  updateBackpressure(js);
  advanceQueueIfNeeded(js, kj::mv(self));
  return kj::mv(prp.promise);
}

template <typename Self>
void WritableImpl<Self>::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(writable, Writable) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      visitor.visit(error);
    }
    KJ_CASE_ONEOF(erroring, StreamStates::Erroring) {
      visitor.visit(erroring.reason);
    }
  }
  visitor.visit(inFlightWrite, inFlightClose, closeRequest, algorithms, signal);
  KJ_IF_SOME(pendingAbort, maybePendingAbort) {
    visitor.visit(*pendingAbort);
  }
  visitor.visitAll(writeRequests);
}

template <typename Self>
bool WritableImpl<Self>::isWritable() const {
  return state.template is<Writable>();
}

template <typename Self>
void WritableImpl<Self>::cancelPendingWrites(jsg::Lock& js, jsg::JsValue reason) {
  for (auto& write: writeRequests) {
    write.resolver.reject(js, reason);
  }
  writeRequests.clear();
}

// ======================================================================================

namespace {
template <typename Controller, typename Queue>
struct ReadableState {
  Controller controller;
  kj::Own<typename Queue::Consumer> consumer;
  ReadableStreamJsController& owner;

  ReadableState(Controller controller,
      kj::Own<typename Queue::Consumer> consumer,
      ReadableStreamJsController& owner)
      : controller(kj::mv(controller)),
        consumer(kj::mv(consumer)),
        owner(owner) {}

  ReadableState(Controller controller,
      typename Queue::ConsumerImpl::StateListener& listener,
      ReadableStreamJsController& owner)
      : ReadableState(controller.addRef(), controller->getConsumer(listener), owner) {}

  ReadableState clone(jsg::Lock& js,
      typename Queue::ConsumerImpl::StateListener& listener,
      ReadableStreamJsController& owner) {
    return ReadableState(controller.addRef(), consumer->clone(js, listener), owner);
  }
};

struct ValueReadable final: private api::ValueQueue::ConsumerImpl::StateListener {

  using State = ReadableState<DefaultController, ValueQueue>;
  kj::Maybe<State> state;

  JSG_MEMORY_INFO(ValueReadable) {
    KJ_IF_SOME(s, state) {
      tracker.trackField("controller", s.controller);
      tracker.trackField("consumer", s.consumer);
    }
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(s, state) {
      visitor.visit(s.controller, *s.consumer);
    }
  }

  ValueReadable(DefaultController controller, ReadableStreamJsController& owner)
      : state(State(kj::mv(controller), *this, owner)) {}

  ValueReadable(jsg::Lock& js, ReadableStreamJsController& owner, ValueReadable& other)
      : state(KJ_ASSERT_NONNULL(other.state).clone(js, *this, owner)) {}

  KJ_DISALLOW_COPY_AND_MOVE(ValueReadable);

  void cancelPendingReads(jsg::Lock& js, jsg::JsValue reason) {
    KJ_IF_SOME(s, state) {
      s.consumer->cancelPendingReads(js, reason);
    }
  }

  kj::Own<ValueReadable> clone(jsg::Lock& js, ReadableStreamJsController& owner) {
    // A single ReadableStreamDefaultController can have multiple consumers.
    // When the ValueReadable constructor is used, the new consumer is added
    // and starts to receive new data that becomes enqueued. When clone
    // is used, any state currently held by this consumer is copied to the
    // new consumer.
    return kj::heap<ValueReadable>(js, owner, *this);
  }

  jsg::Promise<ReadResult> read(jsg::Lock& js) {
    KJ_IF_SOME(s, state) {
      auto prp = js.newPromiseAndResolver<ReadResult>();
      s.consumer->read(js,
          ValueQueue::ReadRequest{
            .resolver = kj::mv(prp.resolver),
          });
      return kj::mv(prp.promise);
    }

    // We are canceled! There's nothing to do.
    return js.resolvedPromise(ReadResult{.done = true});
  }

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    // When a ReadableStream is canceled, the expected behavior is that the underlying
    // controller is notified and the cancel algorithm on the underlying source is
    // called. When there are multiple ReadableStreams sharing consumption of a
    // controller, however, it should act as a shared pointer of sorts, canceling
    // the underlying controller only when the last reader is canceled.
    // Here, we rely on the controller implementing the correct behavior since it owns
    // the queue that knows about all of the attached consumers.
    KJ_IF_SOME(s, state) {
      s.consumer->cancel(js, maybeReason);
      auto promise = s.controller->cancel(js, kj::mv(maybeReason));
      state = kj::none;
      return kj::mv(promise);
    }

    return js.resolvedPromise();
  }

  void onConsumerClose(jsg::Lock& js) override {
    // Called by the consumer when a state change to closed happens.
    // We need to notify the owner. Note that the owner may drop this
    // readable in doClose so it is not safe to access anything on this
    // after calling doClose.
    KJ_IF_SOME(s, state) {
      s.owner.doClose(js);
    }
  }

  void onConsumerError(jsg::Lock& js, jsg::Value reason) override {
    // Called by the consumer when a state change to errored happens.
    // We need to notify the owner. Note that the owner may drop this
    // readable in doClose so it is not safe to access anything on this
    // after calling doError.
    KJ_IF_SOME(s, state) {
      s.owner.doError(js, reason.getHandle(js));
    }
  }

  void onConsumerWantsData(jsg::Lock& js) override {
    // Called by the consumer when it has a queued pending read and needs
    // data to be provided to fulfill it. We need to notify the controller
    // to initiate pulling to provide the data.
    KJ_IF_SOME(s, state) {
      s.controller->pull(js);
    }
  }

  kj::Maybe<int> getDesiredSize() {
    KJ_IF_SOME(s, state) {
      return s.controller->getDesiredSize();
    }
    return kj::none;
  }

  bool canCloseOrEnqueue() {
    return state.map([](State& s) { return s.controller->canCloseOrEnqueue(); }).orDefault(false);
  }

  kj::Maybe<DefaultController> getControllerRef() {
    return state.map([](State& s) { return s.controller.addRef(); });
  }
};

struct ByteReadable final: private api::ByteQueue::ConsumerImpl::StateListener {

  using State = ReadableState<ByobController, ByteQueue>;
  kj::Maybe<State> state;
  int autoAllocateChunkSize;

  JSG_MEMORY_INFO(ByteReadable) {
    KJ_IF_SOME(s, state) {
      tracker.trackField("controller", s.controller);
      tracker.trackField("consumer", s.consumer);
    }
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_SOME(s, state) {
      visitor.visit(s.controller, *s.consumer);
    }
  }

  ByteReadable(
      ByobController controller, ReadableStreamJsController& owner, int autoAllocateChunkSize)
      : state(State(kj::mv(controller), *this, owner)),
        autoAllocateChunkSize(autoAllocateChunkSize) {}

  ByteReadable(jsg::Lock& js, ReadableStreamJsController& owner, ByteReadable& other)
      : state(KJ_ASSERT_NONNULL(other.state).clone(js, *this, owner)),
        autoAllocateChunkSize(other.autoAllocateChunkSize) {}

  KJ_DISALLOW_COPY_AND_MOVE(ByteReadable);

  void cancelPendingReads(jsg::Lock& js, jsg::JsValue reason) {
    KJ_IF_SOME(s, state) {
      s.consumer->cancelPendingReads(js, reason);
    }
  }

  // A single ReadableByteStreamController can have multiple consumers.
  // When the ByteReadable constructor is used, the new consumer is added
  // and starts to receive new data that becomes enqueued. When clone
  // is used, any state currently held by this consumer is copied to the
  // new consumer.
  kj::Own<ByteReadable> clone(jsg::Lock& js, ReadableStreamJsController& owner) {
    return kj::heap<ByteReadable>(js, owner, *this);
  }

  jsg::Promise<ReadResult> read(
      jsg::Lock& js, kj::Maybe<ReadableStreamController::ByobOptions> byobOptions) {
    KJ_IF_SOME(s, state) {
      auto prp = js.newPromiseAndResolver<ReadResult>();

      KJ_IF_SOME(byob, byobOptions) {
        jsg::BufferSource source(js, byob.bufferView.getHandle(js));
        // If atLeast is not given, then by default it is the element size of the view
        // that we were given. If atLeast is given, we make sure that it is aligned
        // with the element size. No matter what, atLeast cannot be less than 1.
        auto atLeast = kj::max(source.getElementSize(), byob.atLeast.orDefault(1));
        atLeast = kj::max(1, atLeast - (atLeast % source.getElementSize()));
        s.consumer->read(js,
            ByteQueue::ReadRequest(kj::mv(prp.resolver),
                {
                  .store = jsg::BufferSource(js, source.detach(js)),
                  .atLeast = atLeast,
                  .type = ByteQueue::ReadRequest::Type::BYOB,
                }));
      } else {
        KJ_IF_SOME(store, jsg::BufferSource::tryAlloc(js, autoAllocateChunkSize)) {
          // Ensure that the handle is created here so that the size of the buffer
          // is accounted for in the isolate memory tracking.
          s.consumer->read(js,
              ByteQueue::ReadRequest(kj::mv(prp.resolver),
                  {
                    .store = kj::mv(store),
                    .type = ByteQueue::ReadRequest::Type::BYOB,
                  }));
        } else {
          prp.resolver.reject(js, js.v8Error("Failed to allocate buffer for read."));
        }
      }

      return kj::mv(prp.promise);
    }

    // We are canceled! There's nothing else to do.
    KJ_IF_SOME(byob, byobOptions) {
      // If a BYOB buffer was given, we need to give it back wrapped in a TypedArray
      // whose size is set to zero.
      jsg::BufferSource source(js, byob.bufferView.getHandle(js));
      auto store = source.detach(js);
      store.consume(store.size());
      return js.resolvedPromise(ReadResult{
        .value = js.v8Ref(store.createHandle(js)),
        .done = true,
      });
    } else {
      return js.resolvedPromise(ReadResult{.done = true});
    }
  }

  // When a ReadableStream is canceled, the expected behavior is that the underlying
  // controller is notified and the cancel algorithm on the underlying source is
  // called. When there are multiple ReadableStreams sharing consumption of a
  // controller, however, it should act as a shared pointer of sorts, canceling
  // the underlying controller only when the last reader is canceled.
  // Here, we rely on the controller implementing the correct behavior since it owns
  // the queue that knows about all of the attached consumers.
  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    KJ_IF_SOME(s, state) {
      s.consumer->cancel(js, maybeReason);
      auto promise = s.controller->cancel(js, kj::mv(maybeReason));
      state = kj::none;
      return kj::mv(promise);
    }

    return js.resolvedPromise();
  }

  void onConsumerClose(jsg::Lock& js) override {
    // Note that the owner may drop this readable in doClose so it
    // is not safe to access anything on this after calling doClose.
    KJ_IF_SOME(s, state) {
      s.owner.doClose(js);
    }
  }

  void onConsumerError(jsg::Lock& js, jsg::Value reason) override {
    // Note that the owner may drop this readable in doClose so it
    // is not safe to access anything on this after calling doError.
    KJ_IF_SOME(s, state) {
      s.owner.doError(js, reason.getHandle(js));
    };
  }

  // Called by the consumer when it has a queued pending read and needs
  // data to be provided to fulfill it. We need to notify the controller
  // to initiate pulling to provide the data.
  void onConsumerWantsData(jsg::Lock& js) override {
    KJ_IF_SOME(s, state) {
      s.controller->pull(js);
    }
  }

  kj::Maybe<int> getDesiredSize() {
    KJ_IF_SOME(s, state) {
      return s.controller->getDesiredSize();
    }
    return kj::none;
  }

  bool canCloseOrEnqueue() {
    return state.map([](State& s) { return s.controller->canCloseOrEnqueue(); }).orDefault(false);
  }

  kj::Maybe<ByobController> getControllerRef() {
    return state.map([](State& state) { return state.controller.addRef(); });
  }
};
}  // namespace

// =======================================================================================

ReadableStreamDefaultController::ReadableStreamDefaultController(
    UnderlyingSource underlyingSource, StreamQueuingStrategy queuingStrategy)
    : ioContext(tryGetIoContext()),
      impl(kj::mv(underlyingSource), kj::mv(queuingStrategy)) {}

kj::Maybe<StreamStates::Errored> ReadableStreamDefaultController::getMaybeErrorState(
    jsg::Lock& js) {
  KJ_IF_SOME(errored, impl.state.tryGet<StreamStates::Errored>()) {
    return errored.addRef(js);
  }
  return kj::none;
}

void ReadableStreamDefaultController::start(jsg::Lock& js) {
  impl.start(js, JSG_THIS);
}

bool ReadableStreamDefaultController::canCloseOrEnqueue() {
  return impl.canCloseOrEnqueue();
}

bool ReadableStreamDefaultController::hasBackpressure() {
  return !impl.shouldCallPull();
}

kj::Maybe<int> ReadableStreamDefaultController::getDesiredSize() {
  return impl.getDesiredSize();
}

void ReadableStreamDefaultController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

jsg::Promise<void> ReadableStreamDefaultController::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault([&] { return js.v8Undefined(); }));
}

void ReadableStreamDefaultController::close(jsg::Lock& js) {
  impl.close(js);
}

void ReadableStreamDefaultController::enqueue(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> chunk) {
  auto value = chunk.orDefault(js.v8Undefined());

  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError, "Unable to enqueue");

  size_t size = 1;
  bool errored = false;
  KJ_IF_SOME(sizeFunc, impl.algorithms.size) {
    js.tryCatch([&] { size = sizeFunc(js, value); }, [&](jsg::Value exception) {
      impl.doError(js, kj::mv(exception));
      errored = true;
    });
  }

  if (!errored) {
    impl.enqueue(js, kj::heap<ValueQueue::Entry>(js.v8Ref(value), size), JSG_THIS);
  }
}

void ReadableStreamDefaultController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  impl.doError(js, js.v8Ref(reason));
}

// When a consumer receives a read request, but does not have the data available to
// fulfill the request, the consumer will call pull on the controller to pull that
// data if needed.
void ReadableStreamDefaultController::pull(jsg::Lock& js) {
  impl.pullIfNeeded(js, JSG_THIS);
}

kj::Own<ValueQueue::Consumer> ReadableStreamDefaultController::getConsumer(
    kj::Maybe<ValueQueue::ConsumerImpl::StateListener&> stateListener) {
  return impl.getConsumer(stateListener);
}

// ======================================================================================

ReadableStreamBYOBRequest::Impl::Impl(
    jsg::Lock& js, kj::Own<ByteQueue::ByobRequest> readRequest, ByobController controller)
    : readRequest(kj::mv(readRequest)),
      controller(kj::mv(controller)),
      view(js.v8Ref(this->readRequest->getView(js))) {}

void ReadableStreamBYOBRequest::Impl::updateView(jsg::Lock& js) {
  jsg::check(view.getHandle(js)->Buffer()->Detach(v8::Local<v8::Value>()));
  view = js.v8Ref(readRequest->getView(js));
}

void ReadableStreamBYOBRequest::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(impl, maybeImpl) {
    visitor.visit(impl.view, impl.controller);
  }
}

ReadableStreamBYOBRequest::ReadableStreamBYOBRequest(
    jsg::Lock& js, kj::Own<ByteQueue::ByobRequest> readRequest, ByobController controller)
    : ioContext(tryGetIoContext()),
      maybeImpl(Impl(js, kj::mv(readRequest), kj::mv(controller))) {}

kj::Maybe<int> ReadableStreamBYOBRequest::getAtLeast() {
  KJ_IF_SOME(impl, maybeImpl) {
    return impl.readRequest->getAtLeast();
  }
  return kj::none;
}

kj::Maybe<jsg::V8Ref<v8::Uint8Array>> ReadableStreamBYOBRequest::getView(jsg::Lock& js) {
  KJ_IF_SOME(impl, maybeImpl) {
    return impl.view.addRef(js);
  }
  return kj::none;
}

void ReadableStreamBYOBRequest::invalidate(jsg::Lock& js) {
  KJ_IF_SOME(impl, maybeImpl) {
    // If the user code happened to have retained a reference to the view or
    // the buffer, we need to detach it so that those references cannot be used
    // to modify or observe modifications.
    jsg::check(impl.view.getHandle(js)->Buffer()->Detach(v8::Local<v8::Value>()));
    impl.controller->maybeByobRequest = kj::none;
  }
  maybeImpl = kj::none;
}

void ReadableStreamBYOBRequest::respond(jsg::Lock& js, int bytesWritten) {
  auto& impl = JSG_REQUIRE_NONNULL(
      maybeImpl, TypeError, "This ReadableStreamBYOBRequest has been invalidated.");
  JSG_REQUIRE(impl.view.getHandle(js)->ByteLength() > 0, TypeError,
      "Cannot respond with a zero-length or detached view");
  if (!impl.controller->canCloseOrEnqueue()) {
    JSG_REQUIRE(
        bytesWritten == 0, TypeError, "The bytesWritten must be zero after the stream is closed.");
    KJ_ASSERT(impl.readRequest->isInvalidated());
    invalidate(js);
  } else {
    bool shouldInvalidate = false;
    if (impl.readRequest->isInvalidated() && impl.controller->impl.consumerCount() >= 1) {
      // While this particular request may be invalidated, there are still
      // other branches we can push the data to. Let's do so.
      jsg::BufferSource source(js, impl.view.getHandle(js));
      auto entry = kj::heap<ByteQueue::Entry>(jsg::BufferSource(js, source.detach(js)));
      impl.controller->impl.enqueue(js, kj::mv(entry), impl.controller.addRef());
    } else {
      JSG_REQUIRE(bytesWritten > 0, TypeError,
          "The bytesWritten must be more than zero while the stream is open.");
      if (impl.readRequest->respond(js, bytesWritten)) {
        // The read request was fulfilled, we need to invalidate.
        shouldInvalidate = true;
      } else {
        // The response did not fulfill the minimum requirements of the read.
        // We do not want to invalidate the read request and we need to update the
        // view so that on the next read the view will be properly adjusted.
        impl.updateView(js);
      }
    }
    impl.controller->pull(js);
    if (shouldInvalidate) {
      invalidate(js);
    }
  }
}

void ReadableStreamBYOBRequest::respondWithNewView(jsg::Lock& js, jsg::BufferSource view) {
  auto& impl = JSG_REQUIRE_NONNULL(
      maybeImpl, TypeError, "This ReadableStreamBYOBRequest has been invalidated.");
  if (!impl.controller->canCloseOrEnqueue()) {
    JSG_REQUIRE(view.size() == 0, TypeError,
        "The view byte length must be zero after the stream is closed.");
    KJ_ASSERT(impl.readRequest->isInvalidated());
    invalidate(js);
  } else {
    bool shouldInvalidate = false;
    if (impl.readRequest->isInvalidated() && impl.controller->impl.consumerCount() >= 1) {
      // While this particular request may be invalidated, there are still
      // other branches we can push the data to. Let's do so.
      auto entry = kj::heap<ByteQueue::Entry>(jsg::BufferSource(js, view.detach(js)));
      impl.controller->impl.enqueue(js, kj::mv(entry), impl.controller.addRef());
    } else {
      JSG_REQUIRE(view.size() > 0, TypeError,
          "The view byte length must be more than zero while the stream is open.");
      if (impl.readRequest->respondWithNewView(js, kj::mv(view))) {
        // The read request was fulfilled, we need to invalidate.
        shouldInvalidate = true;
      } else {
        // The response did not fulfill the minimum requirements of the read.
        // We do not want to invalidate the read request and we need to update the
        // view so that on the next read the view will be properly adjusted.
        impl.updateView(js);
      }
    }

    impl.controller->pull(js);
    if (shouldInvalidate) {
      invalidate(js);
    }
  }
}

bool ReadableStreamBYOBRequest::isPartiallyFulfilled() {
  KJ_IF_SOME(impl, maybeImpl) {
    return impl.readRequest->isPartiallyFulfilled();
  }
  return false;
}

// ======================================================================================

ReadableByteStreamController::ReadableByteStreamController(
    UnderlyingSource underlyingSource, StreamQueuingStrategy queuingStrategy)
    : ioContext(tryGetIoContext()),
      impl(kj::mv(underlyingSource), kj::mv(queuingStrategy)) {}

void ReadableByteStreamController::start(jsg::Lock& js) {
  impl.start(js, JSG_THIS);
}

bool ReadableByteStreamController::canCloseOrEnqueue() {
  return impl.canCloseOrEnqueue();
}

bool ReadableByteStreamController::hasBackpressure() {
  return !impl.shouldCallPull();
}

kj::Maybe<int> ReadableByteStreamController::getDesiredSize() {
  return impl.getDesiredSize();
}

void ReadableByteStreamController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(maybeByobRequest, impl);
}

jsg::Promise<void> ReadableByteStreamController::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  KJ_IF_SOME(byobRequest, maybeByobRequest) {
    if (impl.consumerCount() == 1) {
      byobRequest->invalidate(js);
    }
  }
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault(js.v8Undefined()));
}

void ReadableByteStreamController::close(jsg::Lock& js) {
  KJ_IF_SOME(byobRequest, maybeByobRequest) {
    JSG_REQUIRE(!byobRequest->isPartiallyFulfilled(), TypeError,
        "This ReadableStream was closed with a partial read pending.");
  }
  impl.close(js);
}

void ReadableByteStreamController::enqueue(jsg::Lock& js, jsg::BufferSource chunk) {
  JSG_REQUIRE(chunk.size() > 0, TypeError, "Cannot enqueue a zero-length ArrayBuffer.");
  JSG_REQUIRE(chunk.canDetach(js), TypeError, "The provided ArrayBuffer must be detachable.");
  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError, "This ReadableByteStreamController is closed.");

  KJ_IF_SOME(byobRequest, maybeByobRequest) {
    KJ_IF_SOME(view, byobRequest->getView(js)) {
      JSG_REQUIRE(view.getHandle(js)->ByteLength() > 0, TypeError,
          "The byobRequest.view is zero-length or was detached");
    }
    byobRequest->invalidate(js);
  }

  impl.enqueue(js, kj::heap<ByteQueue::Entry>(jsg::BufferSource(js, chunk.detach(js))), JSG_THIS);
}

void ReadableByteStreamController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  impl.doError(js, js.v8Ref(reason));
}

kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> ReadableByteStreamController::getByobRequest(
    jsg::Lock& js) {
  if (maybeByobRequest == kj::none) {
    KJ_IF_SOME(queue, impl.state.tryGet<ByteQueue>()) {
      KJ_IF_SOME(pendingByob, queue.nextPendingByobReadRequest()) {
        maybeByobRequest = js.alloc<ReadableStreamBYOBRequest>(js, kj::mv(pendingByob), JSG_THIS);
      }
    } else {
      return kj::none;
    }
  }

  return maybeByobRequest.map(
      [&](jsg::Ref<ReadableStreamBYOBRequest>& req) { return req.addRef(); });
}

// When a consumer receives a read request, but does not have the data available to
// fulfill the request, the consumer will call pull on the controller to pull that
// data if needed.
void ReadableByteStreamController::pull(jsg::Lock& js) {
  impl.pullIfNeeded(js, JSG_THIS);
}

kj::Own<ByteQueue::Consumer> ReadableByteStreamController::getConsumer(
    kj::Maybe<ByteQueue::ConsumerImpl::StateListener&> stateListener) {
  return impl.getConsumer(stateListener);
}

// ======================================================================================

ReadableStreamJsController::ReadableStreamJsController(): ioContext(tryGetIoContext()) {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Closed closed)
    : ioContext(tryGetIoContext()),
      state(closed) {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Errored errored)
    : ioContext(tryGetIoContext()),
      state(kj::mv(errored)) {}

ReadableStreamJsController::ReadableStreamJsController(jsg::Lock& js, ValueReadable& consumer)
    : ioContext(tryGetIoContext()),
      state(consumer.clone(js, *this)) {}

ReadableStreamJsController::ReadableStreamJsController(jsg::Lock& js, ByteReadable& consumer)
    : ioContext(tryGetIoContext()),
      state(consumer.clone(js, *this)) {}

jsg::Ref<ReadableStream> ReadableStreamJsController::addRef() {
  return KJ_REQUIRE_NONNULL(owner).addRef();
}

jsg::Promise<void> ReadableStreamJsController::cancel(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  disturbed = true;

  const auto doCancel = [&](auto& consumer) {
    auto reason = js.v8Ref(maybeReason.orDefault([&] { return js.v8Undefined(); }));
    KJ_DEFER(state.init<StreamStates::Closed>());
    return consumer->cancel(js, reason.getHandle(js));
  };

  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return js.resolvedPromise();
      }
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        return js.rejectedPromise<void>(errored.addRef(js));
      }
    }
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<void>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      return doCancel(consumer);
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return doCancel(consumer);
    }
  }

  KJ_UNREACHABLE;
}

// Finalizes the closed state of this ReadableStream. The connection to the underlying
// controller is released with no further action. Importantly, this method is triggered
// by the underlying controller as a result of that controller closing or being canceled.
// We detach ourselves from the underlying controller by releasing the ValueReadable or
// ByteReadable in the state and changing that to closed.
// We also clean up other state here.
void ReadableStreamJsController::doClose(jsg::Lock& js) {
  if (isReadPending()) {
    setPendingState(StreamStates::Closed());
  } else {
    state.init<StreamStates::Closed>();
    lock.onClose(js);
  }
}

// As with doClose(), doError() finalizes the error state of this ReadableStream.
// The connection to the underlying controller is released with no further action.
// This method is triggered by the underlying controller as a result of that controller
// erroring. We detach ourselves from the underlying controller by releasing the ValueReadable
// or ByteReadable in the state and changing that to errored.
// We also clean up other state here.
void ReadableStreamJsController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  if (isReadPending()) {
    setPendingState(js.v8Ref(reason));
  } else {
    state.init<StreamStates::Errored>(js.v8Ref(reason));
    lock.onError(js, reason);
  }
}

bool ReadableStreamJsController::isByteOriented() const {
  return state.is<kj::Own<ByteReadable>>();
}

bool ReadableStreamJsController::isClosedOrErrored() const {
  if (maybePendingState != kj::none) {
    return true;
  }
  return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
}

bool ReadableStreamJsController::isClosed() const {
  KJ_IF_SOME(s, maybePendingState) {
    return s.is<StreamStates::Closed>();
  }
  return state.is<StreamStates::Closed>();
}

bool ReadableStreamJsController::isDisturbed() {
  return disturbed;
}

bool ReadableStreamJsController::isLockedToReader() const {
  return lock.isLockedToReader();
}

bool ReadableStreamJsController::lockReader(jsg::Lock& js, Reader& reader) {
  return lock.lockReader(js, *this, reader);
}

jsg::Promise<void> ReadableStreamJsController::pipeTo(
    jsg::Lock& js, WritableStreamController& destination, PipeToOptions options) {
  KJ_DASSERT(!isLockedToReader());
  KJ_DASSERT(!destination.isLockedToWriter());

  disturbed = true;
  KJ_IF_SOME(promise, destination.tryPipeFrom(js, addRef(), kj::mv(options))) {
    return kj::mv(promise);
  }

  return js.rejectedPromise<void>(
      js.v8TypeError("This ReadableStream cannot be piped to this WritableStream"_kj));
}

kj::Maybe<jsg::Promise<ReadResult>> ReadableStreamJsController::read(
    jsg::Lock& js, kj::Maybe<ByobOptions> maybeByobOptions) {
  disturbed = true;

  KJ_IF_SOME(byobOptions, maybeByobOptions) {
    byobOptions.detachBuffer = true;
    auto view = byobOptions.bufferView.getHandle(js);
    if (!view->Buffer()->IsDetachable()) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("Unabled to use non-detachable ArrayBuffer."_kj));
    }

    if (view->ByteLength() == 0 || view->Buffer()->ByteLength() == 0) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("Unable to use a zero-length ArrayBuffer."_kj));
    }

    if (state.is<StreamStates::Closed>() || maybePendingState != kj::none) {
      KJ_IF_SOME(pendingState, maybePendingState) {
        KJ_SWITCH_ONEOF(pendingState) {
          KJ_CASE_ONEOF(closed, StreamStates::Closed) {
            // Fall through to the BYOB read case below.
          }
          KJ_CASE_ONEOF(errored, StreamStates::Errored) {
            return js.rejectedPromise<ReadResult>(errored.addRef(js));
          }
        }
      }
      // If it is a BYOB read, then the spec requires that we return an empty
      // view of the same type provided, that uses the same backing memory
      // as that provided, but with zero-length.
      auto source = jsg::BufferSource(js, byobOptions.bufferView.getHandle(js));
      auto store = source.detach(js);
      store.consume(store.size());
      return js.resolvedPromise(ReadResult{
        .value = js.v8Ref(store.createHandle(js)),
        .done = true,
      });
    }
  }

  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        // The closed state for BYOB reads is handled in the maybeByobOptions check above.
        KJ_ASSERT(maybeByobOptions == kj::none);
        return js.resolvedPromise(ReadResult{.done = true});
      }
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        return js.rejectedPromise<ReadResult>(errored.addRef(js));
      }
    }
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // The closed state for BYOB reads is handled in the maybeByobOptions check above.
      KJ_ASSERT(maybeByobOptions == kj::none);
      return js.resolvedPromise(ReadResult{.done = true});
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<ReadResult>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      // The ReadableStreamDefaultController does not support ByobOptions.
      // It should never happen, but let's make sure.
      KJ_ASSERT(maybeByobOptions == kj::none);
      return deferControllerStateChange(js, *this, [&]() mutable { return consumer->read(js); });
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return deferControllerStateChange(
          js, *this, [&]() mutable { return consumer->read(js, kj::mv(maybeByobOptions)); });
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseReader(*this, reader, maybeJs);
}

ReadableStreamController::Tee ReadableStreamJsController::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLockedToReader(), TypeError, "This ReadableStream is locked to a reader.");
  lock.state.init<Locked>();
  disturbed = true;

  // This will leave this stream locked, disturbed, and closed.

  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return Tee{
          .branch1 = js.alloc<ReadableStream>(
              kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
          .branch2 = js.alloc<ReadableStream>(
              kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
        };
      }
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        return Tee{
          .branch1 =
              js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(errored.addRef(js))),
          .branch2 =
              js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(errored.addRef(js))),
        };
      }
    }
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return Tee{
        .branch1 =
            js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
        .branch2 =
            js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
      };
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return Tee{
        .branch1 =
            js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(errored.addRef(js))),
        .branch2 =
            js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(errored.addRef(js))),
      };
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      KJ_DEFER(state.init<StreamStates::Closed>());
      // We create two additional streams that clone this stream's consumer state,
      // then close this stream's consumer.
      return Tee{
        .branch1 = js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
        .branch2 = js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
      };
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      KJ_DEFER(state.init<StreamStates::Closed>());
      // We create two additional streams that clone this stream's consumer state,
      // then close this stream's consumer.
      return Tee{
        .branch1 = js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
        .branch2 = js.alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
      };
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::setOwnerRef(ReadableStream& stream) {
  KJ_ASSERT(owner == kj::none);
  owner = &stream;
}

void ReadableStreamJsController::setup(jsg::Lock& js,
    jsg::Optional<UnderlyingSource> maybeUnderlyingSource,
    jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {
  auto underlyingSource = kj::mv(maybeUnderlyingSource).orDefault({});
  auto queuingStrategy = kj::mv(maybeQueuingStrategy).orDefault({});
  auto type = underlyingSource.type.map([](kj::StringPtr s) { return s; }).orDefault(""_kj);

  expectedLength = underlyingSource.expectedLength;

  if (type == "bytes") {
    auto autoAllocateChunkSize = underlyingSource.autoAllocateChunkSize.orDefault(
        UnderlyingSource::DEFAULT_AUTO_ALLOCATE_CHUNK_SIZE);

    auto controller =
        js.alloc<ReadableByteStreamController>(kj::mv(underlyingSource), kj::mv(queuingStrategy));

    JSG_REQUIRE(
        autoAllocateChunkSize > 0, TypeError, "The autoAllocateChunkSize option cannot be zero.");

    state = kj::heap<ByteReadable>(controller.addRef(), *this, autoAllocateChunkSize);
    controller->start(js);
  } else {
    JSG_REQUIRE(
        type == "", TypeError, kj::str("\"", type, "\" is not a valid type of ReadableStream."));
    auto controller = js.alloc<ReadableStreamDefaultController>(
        kj::mv(underlyingSource), kj::mv(queuingStrategy));
    state = kj::heap<ValueReadable>(controller.addRef(), *this);
    controller->start(js);
  }
}

kj::Maybe<ReadableStreamController::PipeController&> ReadableStreamJsController::tryPipeLock(
    jsg::Ref<WritableStream> destination) {
  return lock.tryPipeLock(*this, kj::mv(destination));
}

void ReadableStreamJsController::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(error, StreamStates::Errored) {
        visitor.visit(error);
      }
    }
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      visitor.visit(error);
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      visitor.visit(*consumer);
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      visitor.visit(*consumer);
    }
  }
  visitor.visit(lock);
};

kj::Maybe<int> ReadableStreamJsController::getDesiredSize() {
  if (maybePendingState != kj::none) {
    return kj::none;
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::none;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      return consumer->getDesiredSize();
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return consumer->getDesiredSize();
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<v8::Local<v8::Value>> ReadableStreamJsController::isErrored(jsg::Lock& js) {
  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return kj::none;
      }
      KJ_CASE_ONEOF(error, StreamStates::Errored) {
        return error.getHandle(js);
      }
    }
  }
  return state.tryGet<StreamStates::Errored>().map(
      [&](jsg::Value& reason) { return reason.getHandle(js); });
}

bool ReadableStreamJsController::canCloseOrEnqueue() {
  if (maybePendingState != kj::none) {
    return false;
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return false;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return false;
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      return consumer->canCloseOrEnqueue();
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return consumer->canCloseOrEnqueue();
    }
  }
  KJ_UNREACHABLE;
}

bool ReadableStreamJsController::hasBackpressure() {
  KJ_IF_SOME(size, getDesiredSize()) {
    return size <= 0;
  }
  return false;
}

kj::Maybe<kj::OneOf<DefaultController, ByobController>> ReadableStreamJsController::
    getController() {
  if (maybePendingState != kj::none) {
    return kj::none;
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::none;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      return consumer->getControllerRef();
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return consumer->getControllerRef();
    }
  }
  KJ_UNREACHABLE;
}

namespace {
// Consumes all bytes from a stream, buffering in memory, with the purpose
// of producing either a single concatenated kj::Array<byte> or kj::String.
class AllReader {
 public:
  using PartList = kj::Array<kj::ArrayPtr<byte>>;

  AllReader(jsg::Ref<ReadableStream> stream, uint64_t limit): state(kj::mv(stream)), limit(limit) {}
  KJ_DISALLOW_COPY_AND_MOVE(AllReader);

  jsg::Promise<jsg::BufferSource> allBytes(jsg::Lock& js) {
    return loop(js).then(js, [this](auto& js, PartList&& partPtrs) -> jsg::BufferSource {
      auto out = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, runningTotal);
      copyInto(out.asArrayPtr(), kj::mv(partPtrs));
      return jsg::BufferSource(js, kj::mv(out));
    });
  }

  jsg::Promise<kj::String> allText(jsg::Lock& js) {
    return loop(js).then(js, [this](auto& js, PartList&& partPtrs) {
      auto out = kj::heapArray<char>(runningTotal + 1);
      copyInto(out.first(out.size() - 1).asBytes(), kj::mv(partPtrs));
      out.back() = '\0';
      return kj::String(kj::mv(out));
    });
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        visitor.visit(errored);
      }
      KJ_CASE_ONEOF(readable, jsg::Ref<ReadableStream>) {
        visitor.visit(readable);
      }
    }
  }

 private:
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, jsg::Ref<ReadableStream>> state;
  uint64_t limit;
  kj::Vector<jsg::BufferSource> parts;
  uint64_t runningTotal = 0;

  jsg::Promise<PartList> loop(jsg::Lock& js) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return js.resolvedPromise(KJ_MAP(p, parts) { return p.asArrayPtr(); });
      }
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        return js.template rejectedPromise<PartList>(errored.getHandle(js));
      }
      KJ_CASE_ONEOF(readable, jsg::Ref<ReadableStream>) {
        // Note that these nested lambda retain references to `this` and `readable`
        // and are passed into to promise returned by this method. It is the responsibility
        // of the caller to ensure that the AllReader instance is kept alive until the
        // promise is settled.
        auto onSuccess = [this, &readable](
                             jsg::Lock& js, ReadResult result) -> jsg::Promise<PartList> {
          if (result.done) {
            state.template init<StreamStates::Closed>();
            return loop(js);
          }

          // If we're not done, the result value must be interpretable as
          // bytes for the read to make any sense.
          auto handle = KJ_ASSERT_NONNULL(result.value).getHandle(js);
          if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
            auto error = js.v8TypeError("This ReadableStream did not return bytes.");
            auto rs = kj::mv(readable);
            state.template init<StreamStates::Errored>(js.v8Ref(error));
            return rs->getController().cancel(js, error).then(
                js, [&](jsg::Lock& js) { return loop(js); });
          }

          jsg::BufferSource bufferSource(js, handle);
          jsg::BackingStore backing = bufferSource.detach(js);

          if (backing.size() == 0) {
            // Weird but allowed, we'll skip it.
            return loop(js);
          }

          if ((runningTotal + backing.size()) > limit) {
            auto error = js.v8TypeError("Memory limit exceeded before EOF.");
            auto rs = kj::mv(readable);
            state.template init<StreamStates::Errored>(js.v8Ref(error));
            return rs->getController().cancel(js, error).then(
                js, [&](jsg::Lock& js) { return loop(js); });
          }

          runningTotal += backing.size();
          parts.add(jsg::BufferSource(js, kj::mv(backing)));
          return loop(js);
        };

        auto onFailure = [this](auto& js, jsg::Value exception) -> jsg::Promise<PartList> {
          // In this case the stream should already be errored.
          state.template init<StreamStates::Errored>(js.v8Ref(exception.getHandle(js)));
          return loop(js);
        };

        return maybeAddFunctor(js, KJ_ASSERT_NONNULL(readable->getController().read(js, kj::none)),
            kj::mv(onSuccess), kj::mv(onFailure));
      }
    }
    KJ_UNREACHABLE;
  }

  void copyInto(kj::ArrayPtr<byte> out, PartList in) {
    size_t pos = 0;
    auto dest = out.begin();
    for (auto& part: in) {
      KJ_ASSERT(part.size() <= out.size() - pos);
      auto ptr = part.begin();
      std::copy(ptr, ptr + part.size(), dest);
      pos += part.size();
      dest += part.size();
    }
  }
};

class PumpToReader {
 public:
  PumpToReader(jsg::Ref<ReadableStream> stream, kj::Own<WritableStreamSink> sink, bool end)
      : ioContext(IoContext::current()),
        state(kj::mv(stream)),
        sink(kj::mv(sink)),
        self(kj::refcounted<WeakRef<PumpToReader>>(kj::Badge<PumpToReader>{}, *this)),
        end(end) {}
  KJ_DISALLOW_COPY_AND_MOVE(PumpToReader);

  ~PumpToReader() noexcept(false) {
    self->invalidate();
    // Ensure that if a write promise is pending it is proactively canceled.
    canceler.cancel("PumpToReader was destroyed");
  }

  kj::Promise<void> pumpTo(jsg::Lock& js) {
    ioContext.requireCurrentOrThrowJs();
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(stream, jsg::Ref<ReadableStream>) {
        auto readable = stream.addRef();
        state.template init<Pumping>();
        // Ownership of readable passes into the pump loop...
        // Ownership of the sink remains with the PumpToReader...
        // The JS Promise loop uses an IoOwn wrapping a weak ref to the PumpToReader...
        // The ownership of everything here is a bit complicated. We have a kj::Promise
        // wrapping a JS Promise that is essentially a loop of JS read promises followed
        // by kj write promise. If the outer kj Promise is dropped, the PumpToReader attached
        // to it is dropped. When that happens, there's a chance the JS continuation will still
        // be scheduled to run. The IoOwn ensures that the PumpToReader, and the sink it owns,
        // are always accessed from the right IoContext. The WeakRef ensures that if the
        // PumpToReader is freed while the JS continuation is pending, there won't be a dangling
        // reference.
        return ioContext.awaitJs(
            js, pumpLoop(js, ioContext, kj::mv(readable), ioContext.addObject(self->addRef())));
      }
      KJ_CASE_ONEOF(pumping, Pumping) {
        return KJ_EXCEPTION(FAILED, "pumping is already in progress");
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return KJ_EXCEPTION(FAILED, "stream has already been consumed");
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        return kj::cp(errored);
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  struct Pumping {};
  IoContext& ioContext;
  kj::OneOf<Pumping, StreamStates::Closed, kj::Exception, jsg::Ref<ReadableStream>> state;
  kj::Own<WritableStreamSink> sink;
  kj::Own<WeakRef<PumpToReader>> self;
  kj::Canceler canceler;
  bool end;

  bool isErroredOrClosed() {
    return state.template is<kj::Exception>() || state.template is<StreamStates::Closed>();
  }

  jsg::Promise<void> pumpLoop(jsg::Lock& js,
      IoContext& ioContext,
      jsg::Ref<ReadableStream> readable,
      IoOwn<WeakRef<PumpToReader>> pumpToReader) {
    ioContext.requireCurrentOrThrowJs();

    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ready, jsg::Ref<ReadableStream>) {
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return end ? ioContext.awaitIoLegacy(js, sink->end().attach(kj::mv(sink)))
                   : js.resolvedPromise();
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        if (end) {
          sink->abort(kj::cp(errored));
        }
        return js.rejectedPromise<void>(kj::cp(errored));
      }
      KJ_CASE_ONEOF(pumping, Pumping) {
        using Result = kj::OneOf<Pumping,  // Continue with next read.
            kj::Array<kj::byte>,           // Bytes to write were returned.
            StreamStates::Closed,          // Readable indicated done.
            jsg::Value>;                   // There was an error.

        // The flow here is relatively straightforward but the ownership of
        // readable/pumpToReader is fairly complicated.
        //
        // First, we read from the readable, attaching both a success and fail
        // continuation. In the success continuation, we check the read result
        // and determine if the readable is done, if the readable provided bytes,
        // or if the read failed. This result is passed to another continuation
        // that processes the result.
        //
        // Within that second continuation, we first check to see if the PumpToReader
        // is still alive. It won't be if the kj::Promise representing the pump has
        // been dropped. If it is not alive, we clean up by canceling the readable
        // if necessary and just stopping. If the PumpToReader is alive, our next
        // step is determined by the result of the read.
        //
        // If the read provided bytes, we write those into the sink, which returns
        // a kj::Promise wrapped with a JS promise. If that write fails, we error
        // the PumpToReader and cleanup. If the write succeeds, we loop again for
        // another read.
        //
        // If the read indicates that we're done, we close the PumpToReader and
        // cleanup.
        //
        // If the read indicates that we errored, we error the PumpToReader and
        // cleanup.
        //
        // Importantly, at each step, we check the PumpToReader to ensure that
        // we are accessing it from the correct IoContext (it is wrapped in an
        // IoOwn), and we check that it's still alive (using the WeakRef).
        //
        // This loop owns both the readable and pumpToReader, however, the
        // pumpToReader is an IoOwn<WeakRef> pointing at the actual PumpToReader
        // instance, which is a kj heap object attached to the kj::Promise returned
        // by the pumpTo method. If that promise gets dropped while any of the
        // JS promises in the loop are still pending, then the PumpToReader will
        // be freed. When the JS promise resolves, we make sure we detect that
        // case and handle appropriately (generally by canceling the readable
        // and exiting the loop).
        return KJ_ASSERT_NONNULL(readable->getController().read(js, kj::none))
            .then(js,
                ioContext.addFunctor([byteStream = readable->getController().isByteOriented()](
                                         auto& js, ReadResult result) mutable -> Result {
          if (result.done) {
            // Indicate to the outer promise that the readable is done.
            // There's nothing further to do.
            return StreamStates::Closed();
          }

          // If we're not done, the result value must be interpretable as
          // bytes for the read to make any sense.
          auto handle = KJ_ASSERT_NONNULL(result.value).getHandle(js);
          if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
            return js.v8Ref(js.v8TypeError("This ReadableStream did not return bytes."));
          }

          jsg::BufferSource bufferSource(js, handle);
          if (bufferSource.size() == 0) {
            // Weird, but allowed. We'll skip it.
            return Pumping{};
          }

          if (byteStream) {
            jsg::BackingStore backing = bufferSource.detach(js);
            return backing.asArrayPtr().attach(kj::mv(backing));
          } else {
            // We do not detach in this case because, as bad as an idea as it is,
            // the stream spec does allow a single typedarray/arraybuffer instance
            // to be queued multiple times when using value-oriented streams.
            return bufferSource.asArrayPtr().attach(kj::mv(bufferSource));
          }

          KJ_UNREACHABLE;
        }),
                [](auto& js, jsg::Value exception) mutable -> Result { return kj::mv(exception); })
            .then(js, ioContext.addFunctor( JSG_VISITABLE_LAMBDA((readable = kj::mv(readable), pumpToReader = kj::mv(pumpToReader)), (readable), (jsg::Lock & js, Result result) mutable {
              KJ_IF_SOME(reader, pumpToReader->tryGet()) {
              // Oh good, if we got here it means we're in the right IoContext and
              // the PumpToReader is still alive. Let's process the result.
              reader.ioContext.requireCurrentOrThrowJs();
              auto& ioContext = IoContext::current();
              KJ_SWITCH_ONEOF(result) {
              KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
              // We received bytes to write. Do so...
              auto promise = reader.sink->write(bytes).attach(kj::mv(bytes));
              // Wrap the write promise in a canceler that will be triggered when the
              // PumpToReader is dropped. While the write promise is pending, it is
              // possible for the promise that is holding the PumpToReader to be
              // dropped causing the hold on the sink to be released. If that is
              // released while the write is still pending we can end up with an
              // error further up the destruct chain.
              return ioContext.awaitIo(js, reader.canceler.wrap(kj::mv(promise)))
                  .then(js,
                      [](jsg::Lock& js) -> kj::Maybe<jsg::Value> {
                // The write completed successfully.
                return kj::Maybe<jsg::Value>(kj::none);
              },
                      [](jsg::Lock& js, jsg::Value exception) mutable -> kj::Maybe<jsg::Value> {
                // The write failed.
                return kj::mv(exception);
              })
                  .then(js,
                      ioContext.addFunctor(JSG_VISITABLE_LAMBDA(
                          (readable = readable.addRef(), pumpToReader = kj::mv(pumpToReader)),
                          (readable),
                          (jsg::Lock & js, kj::Maybe<jsg::Value> maybeException) mutable {
                            KJ_IF_SOME(reader, pumpToReader->tryGet()) {
                            auto& ioContext = reader.ioContext;
                            ioContext.requireCurrentOrThrowJs();
                            // Oh good, if we got here it means we're in the right IoContext and
                            // the PumpToReader is still alive.
                            KJ_IF_SOME(exception, maybeException) {
                            if (!reader.isErroredOrClosed()) {
                            reader.state.init<kj::Exception>(js.exceptionToKj(kj::mv(exception)));
                            }
                            } else {
                            // Else block to avert dangling else compiler warning.
                            }
                            return reader.pumpLoop(
                                js, ioContext, readable.addRef(), kj::mv(pumpToReader));
                            } else {
                            // If we got here, we're in the right IoContext but the PumpToReader
                            // has been destroyed. Let's cancel the readable as the last step.
                            return readable->getController().cancel(js,
                                maybeException.map(
                                    [&](jsg::Value& ex) { return ex.getHandle(js); }));
                            }
                          })));
              }
              KJ_CASE_ONEOF(pumping, Pumping) {
              // If we got here, a zero-length buffer was provided by the read and we're
              // just going to ignore it and keep going.
              }
              KJ_CASE_ONEOF(closed, StreamStates::Closed) {
              // If we got here, the read signaled that we're done. Close the reader and
              // pump one more time to shut things down.
              if (!reader.isErroredOrClosed()) {
              reader.state.init<StreamStates::Closed>();
              }
              }
              KJ_CASE_ONEOF(exception, jsg::Value) {
              // If we got here, the read signaled an exception. Either the read failed or
              // provided something other than bytes. Error the reader and pump one more
              // time to shut things down.
              if (!reader.isErroredOrClosed()) {
              reader.state.init<kj::Exception>(js.exceptionToKj(kj::mv(exception)));
              }
              }
              }
              return reader.pumpLoop(js, ioContext, readable.addRef(), kj::mv(pumpToReader));
              } else {
              // If we got here, we're in the right IoContext but the PumpToReader has been
              // freed. There's nothing we can do except cleanup.
              KJ_SWITCH_ONEOF(result) {
              KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
              return readable->getController().cancel(js, kj::none);
              }
              KJ_CASE_ONEOF(pumping, Pumping) {
              return readable->getController().cancel(js, kj::none);
              }
              KJ_CASE_ONEOF(closed, StreamStates::Closed) {
              // We do not have to cancel the readable in this case because it has already
              // signaled that it is done. There's nothing to cancel.
              return js.resolvedPromise();
              }
              KJ_CASE_ONEOF(exception, jsg::Value) {
              return readable->getController().cancel(js, exception.getHandle(js));
              }
              }
              }
              KJ_UNREACHABLE;
            })));
      }
    }
    KJ_UNREACHABLE;
  }
};
}  // namespace

template <typename T>
jsg::Promise<T> ReadableStreamJsController::readAll(jsg::Lock& js, uint64_t limit) {
  if (isLockedToReader()) {
    return js.rejectedPromise<T>(KJ_EXCEPTION(
        FAILED, "jsg.TypeError: This ReadableStream is currently locked to a reader."));
  }
  disturbed = true;

  // This operation leaves the stream locked and disturbed. The loop will read until
  // the stream is closed or errored. If the limit is reached, the loop will error.

  const auto readAll = [this, limit](auto& js) -> jsg::Promise<T> {
    KJ_ASSERT(lock.lock());
    // The AllReader will hold a traceable reference to the ReadableStream.
    auto reader = kj::heap<AllReader>(addRef(), limit);
    auto promise = ([&js, &reader]() -> jsg::Promise<T> {
      if constexpr (kj::isSameType<T, jsg::BufferSource>()) {
        return reader->allBytes(js);
      } else {
        return reader->allText(js);
      }
    })();

    return maybeAddFunctor(js, kj::mv(promise),
        // reader is a GC visitable type that holds a reference to either the stream
        // or an error. Accordingly, we wrap it in a visitable lambda attached as a
        // continuation on the promise to ensure that it is GC visited and kept alive until
        // the promise settles.
        JSG_VISITABLE_LAMBDA((reader = kj::mv(reader)), (reader),
            (jsg::Lock & js, T result)->jsg::Promise<T> {
              return js.resolvedPromise(kj::mv(result));
            }),
        [](jsg::Lock& js, jsg::Value exception) -> jsg::Promise<T> {
      return js.rejectedPromise<T>(kj::mv(exception));
    });
  };

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      if constexpr (kj::isSameType<T, jsg::BufferSource>()) {
        auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, 0);
        return js.resolvedPromise(jsg::BufferSource(js, kj::mv(backing)));
      } else {
        return js.resolvedPromise(T());
      }
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<T>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
      return readAll(js);
    }
    KJ_CASE_ONEOF(byteReadable, kj::Own<ByteReadable>) {
      return readAll(js);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<jsg::BufferSource> ReadableStreamJsController::readAllBytes(
    jsg::Lock& js, uint64_t limit) {
  return readAll<jsg::BufferSource>(js, limit);
}

jsg::Promise<kj::String> ReadableStreamJsController::readAllText(jsg::Lock& js, uint64_t limit) {
  return readAll<kj::String>(js, limit);
}

kj::Own<ReadableStreamController> ReadableStreamJsController::detach(
    jsg::Lock& js, bool ignored /* unused */) {
  KJ_ASSERT(!isLockedToReader());
  KJ_ASSERT(!isDisturbed());
  KJ_ASSERT(!isReadPending(), "Unable to detach with read pending");
  auto controller = kj::heap<ReadableStreamJsController>();
  disturbed = true;

  // Clones this streams state into a new ReadableStreamController, leaving this stream
  // locked, disturbed, and closed.

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      controller->state.init<StreamStates::Closed>();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      controller->state.init<StreamStates::Errored>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, kj::Own<ValueReadable>) {
      KJ_ASSERT(lock.lock());
      controller->state = readable->clone(js, *controller);
      state.init<StreamStates::Closed>();
      lock.onClose(js);
    }
    KJ_CASE_ONEOF(readable, kj::Own<ByteReadable>) {
      KJ_ASSERT(lock.lock());
      controller->state = readable->clone(js, *controller);
      state.init<StreamStates::Closed>();
      lock.onClose(js);
    }
  }

  return kj::mv(controller);
}

kj::Maybe<uint64_t> ReadableStreamJsController::tryGetLength(StreamEncoding encoding) {
  return expectedLength;
}

kj::Promise<DeferredProxy<void>> ReadableStreamJsController::pumpTo(
    jsg::Lock& js, kj::Own<WritableStreamSink> sink, bool end) {
  KJ_ASSERT(IoContext::hasCurrent(), "Unable to consume this ReadableStream outside of a request");
  KJ_REQUIRE(!isLockedToReader(), "This ReadableStream is currently locked to a reader.");
  disturbed = true;

  // This operation will leave the ReadableStream locked and disturbed. It will consume
  // the stream until it either closed or errors. If the deferred proxy promise or its
  // inner promise is dropped, the PumpToReader (and sink) will be dropped and the stream
  // will be canceled. If the PumpToReader is dropped while there is a pending write on
  // the sink, the pending write will be canceled.

  const auto handlePump = [&] {
    KJ_ASSERT(lock.lock());
    auto reader = kj::heap<PumpToReader>(addRef(), kj::mv(sink), end);
    return addNoopDeferredProxy(reader->pumpTo(js).attach(kj::mv(reader)));
  };

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return addNoopDeferredProxy(sink->end().attach(kj::mv(sink)));
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.exceptionToKj(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, kj::Own<ValueReadable>) {
      return handlePump();
    }
    KJ_CASE_ONEOF(readable, kj::Own<ByteReadable>) {
      return handlePump();
    }
  }

  KJ_UNREACHABLE;
}

// ======================================================================================

WritableStreamDefaultController::WritableStreamDefaultController(
    jsg::Lock& js, WritableStream& owner)
    : ioContext(tryGetIoContext()),
      impl(js, owner) {}

jsg::Promise<void> WritableStreamDefaultController::abort(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  return impl.abort(js, JSG_THIS, reason);
}

void WritableStreamDefaultController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

jsg::Promise<void> WritableStreamDefaultController::close(jsg::Lock& js) {
  return impl.close(js, JSG_THIS);
}

void WritableStreamDefaultController::error(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) {
  impl.error(js, JSG_THIS, reason.orDefault(js.v8Undefined()));
}

ssize_t WritableStreamDefaultController::getDesiredSize() {
  return impl.getDesiredSize();
}

jsg::Ref<AbortSignal> WritableStreamDefaultController::getSignal() {
  return impl.signal.addRef();
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamDefaultController::isErroring(jsg::Lock& js) {
  KJ_IF_SOME(erroring, impl.state.tryGet<StreamStates::Erroring>()) {
    return erroring.reason.getHandle(js);
  }
  return kj::none;
}

void WritableStreamDefaultController::setup(
    jsg::Lock& js, UnderlyingSink underlyingSink, StreamQueuingStrategy queuingStrategy) {
  impl.setup(js, JSG_THIS, kj::mv(underlyingSink), kj::mv(queuingStrategy));
}

jsg::Promise<void> WritableStreamDefaultController::write(
    jsg::Lock& js, v8::Local<v8::Value> value) {
  return impl.write(js, JSG_THIS, value);
}

void WritableStreamDefaultController::cancelPendingWrites(jsg::Lock& js, jsg::JsValue reason) {
  impl.cancelPendingWrites(js, reason);
}

// ======================================================================================
WritableStreamJsController::WritableStreamJsController(): ioContext(tryGetIoContext()) {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Closed closed)
    : ioContext(tryGetIoContext()),
      state(closed) {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Errored errored)
    : ioContext(tryGetIoContext()),
      state(kj::mv(errored)) {}

jsg::Promise<void> WritableStreamJsController::abort(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> reason) {
  // The spec requires that if abort is called multiple times, it is supposed to return the same
  // promise each time. That's a bit cumbersome here with jsg::Promise so we intentionally just
  // return a continuation branch off the same promise.
  KJ_IF_SOME(abortPromise, maybeAbortPromise) {
    return abortPromise.whenResolved(js);
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      maybeAbortPromise = js.resolvedPromise();
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved(js);
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      // Per the spec, if the stream is errored, we are to return a resolved promise.
      maybeAbortPromise = js.resolvedPromise();
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved(js);
    }
    KJ_CASE_ONEOF(controller, Controller) {
      maybeAbortPromise = controller->abort(js, reason.orDefault(js.v8Undefined()));
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved(js);
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<WritableStream> WritableStreamJsController::addRef() {
  return KJ_ASSERT_NONNULL(owner).addRef();
}

bool WritableStreamJsController::isClosedOrClosing() {
  return state.is<StreamStates::Closed>();
}

bool WritableStreamJsController::isErrored() {
  return state.is<StreamStates::Errored>();
}

jsg::Promise<void> WritableStreamJsController::close(jsg::Lock& js, bool markAsHandled) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return rejectedMaybeHandledPromise<void>(
          js, js.v8TypeError("This WritableStream has been closed."_kj), markAsHandled);
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return rejectedMaybeHandledPromise<void>(js, errored.getHandle(js), markAsHandled);
    }
    KJ_CASE_ONEOF(controller, Controller) {
      return controller->close(js);
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamJsController::doClose(jsg::Lock& js) {
  state.init<StreamStates::Closed>();
  KJ_IF_SOME(locked, lock.state.tryGet<WriterLocked>()) {
    maybeResolvePromise(js, locked.getClosedFulfiller());
    maybeResolvePromise(js, locked.getReadyFulfiller());
  } else if (lock.state.tryGet<WritableLockImpl::PipeLocked>() != kj::none) {
    lock.state.init<Unlocked>();
  }
}

void WritableStreamJsController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  state.init<StreamStates::Errored>(js.v8Ref(reason));
  KJ_IF_SOME(locked, lock.state.tryGet<WriterLocked>()) {
    maybeRejectPromise<void>(js, locked.getClosedFulfiller(), reason);
    maybeResolvePromise(js, locked.getReadyFulfiller());
  } else if (lock.state.tryGet<WritableLockImpl::PipeLocked>() != kj::none) {
    lock.state.init<Unlocked>();
  }
}

kj::Maybe<int> WritableStreamJsController::getDesiredSize() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return kj::none;
    }
    KJ_CASE_ONEOF(controller, Controller) {
      return controller->getDesiredSize();
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamJsController::isErroring(jsg::Lock& js) {
  KJ_IF_SOME(controller, state.tryGet<Controller>()) {
    return controller->isErroring(js);
  }
  return kj::none;
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamJsController::isErroredOrErroring(jsg::Lock& js) {
  KJ_IF_SOME(err, state.tryGet<StreamStates::Errored>()) {
    return err.getHandle(js);
  }
  return isErroring(js);
}

bool WritableStreamJsController::isStarted() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return true;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return true;
    }
    KJ_CASE_ONEOF(controller, Controller) {
      return controller->isStarted();
    }
  }
  KJ_UNREACHABLE;
}

bool WritableStreamJsController::isLocked() const {
  return isLockedToWriter();
}

bool WritableStreamJsController::isLockedToWriter() const {
  return !lock.state.is<Unlocked>();
}

bool WritableStreamJsController::lockWriter(jsg::Lock& js, Writer& writer) {
  return lock.lockWriter(js, *this, writer);
}

void WritableStreamJsController::maybeRejectReadyPromise(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_SOME(writerLock, lock.state.tryGet<WriterLocked>()) {
    if (writerLock.getReadyFulfiller() != kj::none) {
      maybeRejectPromise<void>(js, writerLock.getReadyFulfiller(), reason);
    } else {
      auto prp = js.newPromiseAndResolver<void>();
      prp.promise.markAsHandled(js);
      prp.resolver.reject(js, reason);
      writerLock.setReadyFulfiller(prp);
    }
  }
}

void WritableStreamJsController::maybeResolveReadyPromise(jsg::Lock& js) {
  KJ_IF_SOME(writerLock, lock.state.tryGet<WriterLocked>()) {
    maybeResolvePromise(js, writerLock.getReadyFulfiller());
  }
}

void WritableStreamJsController::releaseWriter(Writer& writer, kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseWriter(*this, writer, maybeJs);
}

kj::Maybe<kj::Own<WritableStreamSink>> WritableStreamJsController::removeSink(jsg::Lock& js) {
  return kj::none;
}
void WritableStreamJsController::detach(jsg::Lock& js) {
  KJ_UNIMPLEMENTED("WritableStreamJsController::detach is not implemented");
}

void WritableStreamJsController::setOwnerRef(WritableStream& stream) {
  owner = stream;
}

void WritableStreamJsController::setup(jsg::Lock& js,
    jsg::Optional<UnderlyingSink> maybeUnderlyingSink,
    jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {
  auto underlyingSink = kj::mv(maybeUnderlyingSink).orDefault({});
  auto queuingStrategy = kj::mv(maybeQueuingStrategy).orDefault({});
  state = js.alloc<WritableStreamDefaultController>(js, KJ_ASSERT_NONNULL(owner));
  state.get<Controller>()->setup(js, kj::mv(underlyingSink), kj::mv(queuingStrategy));
}

kj::Maybe<jsg::Promise<void>> WritableStreamJsController::tryPipeFrom(
    jsg::Lock& js, jsg::Ref<ReadableStream> source, PipeToOptions options) {
  JSG_REQUIRE_NONNULL(
      ioContext, Error, "Unable to pipe to a WritableStream created outside of a request");

  // The ReadableStream source here can be either a JavaScript-backed ReadableStream
  // or ReadableStreamSource-backed. In either case, however, this WritableStream is
  // JavaScript-based and must use a JavaScript promise-based data flow for piping data.
  // We'll treat all ReadableStreams as if they are JavaScript-backed.
  //
  // This method will return a JavaScript promise that is resolved when the pipe operation
  // completes, or is rejected if the pipe operation is aborted or errored.

  // Let's also acquire the destination pipe lock.
  lock.pipeLock(KJ_ASSERT_NONNULL(owner), kj::mv(source), options);

  return pipeLoop(js).then(js, JSG_VISITABLE_LAMBDA((ref = addRef()), (ref), (auto& js){}));
}

jsg::Promise<void> WritableStreamJsController::pipeLoop(jsg::Lock& js) {
  auto maybePipeLock = lock.tryGetPipe();
  if (maybePipeLock == kj::none) return js.resolvedPromise();
  auto& pipeLock = KJ_REQUIRE_NONNULL(maybePipeLock);

  auto preventAbort = pipeLock.preventAbort;
  auto preventCancel = pipeLock.preventCancel;
  auto preventClose = pipeLock.preventClose;
  auto pipeThrough = pipeLock.pipeThrough;
  auto& source = pipeLock.source;
  // At the start of each pipe step, we check to see if either the source or
  // the destination has closed or errored and propagate that on to the other.
  KJ_IF_SOME(promise, pipeLock.checkSignal(js, *this)) {
    lock.releasePipeLock();
    return kj::mv(promise);
  }

  KJ_IF_SOME(errored, pipeLock.source.tryGetErrored(js)) {
    source.release(js);
    lock.releasePipeLock();
    if (!preventAbort) {
      auto onSuccess = JSG_VISITABLE_LAMBDA(
          (pipeThrough, reason = js.v8Ref(errored)), (reason), (jsg::Lock& js) {
            return rejectedMaybeHandledPromise<void>(js, reason.getHandle(js), pipeThrough);
          });
      auto promise = abort(js, errored);
      if (IoContext::hasCurrent()) {
        return promise.then(js, IoContext::current().addFunctor(kj::mv(onSuccess)));
      } else {
        return promise.then(js, kj::mv(onSuccess));
      }
    }
    return rejectedMaybeHandledPromise<void>(js, errored, pipeThrough);
  }

  KJ_IF_SOME(errored, state.tryGet<StreamStates::Errored>()) {
    lock.releasePipeLock();
    auto reason = errored.getHandle(js);
    if (!preventCancel) {
      source.release(js, reason);
    } else {
      source.release(js);
    }
    return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
  }

  KJ_IF_SOME(erroring, isErroring(js)) {
    lock.releasePipeLock();
    if (!preventCancel) {
      source.release(js, erroring);
    } else {
      source.release(js);
    }
    return rejectedMaybeHandledPromise<void>(js, erroring, pipeThrough);
  }

  if (source.isClosed()) {
    source.release(js);
    lock.releasePipeLock();
    if (!preventClose) {
      auto promise = close(js);
      if (pipeThrough) {
        promise.markAsHandled(js);
      }
      return kj::mv(promise);
    }
    return js.resolvedPromise();
  }

  if (state.is<StreamStates::Closed>()) {
    lock.releasePipeLock();
    auto reason = js.v8TypeError("This destination writable stream is closed."_kj);
    if (!preventCancel) {
      source.release(js, reason);
    } else {
      source.release(js);
    }

    return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
  }

  // Assuming we get by that, we perform a read on the source. If the read errors,
  // we propagate the error to the destination, depending on options and reject
  // the pipe promise. If the read is successful then we'll get a ReadResult
  // back. If the ReadResult indicates done, then we close the destination
  // depending on options and resolve the pipe promise. If the ReadResult is
  // not done, we write the value on to the destination. If the write operation
  // fails, we reject the pipe promise and propagate the error back to the
  // source (again, depending on options). If the write operation is successful,
  // we call pipeLoop again to move on to the next iteration.

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, ref = addRef(), preventCancel, pipeThrough, &source),
      (ref), (jsg::Lock & js, ReadResult result)->jsg::Promise<void> {
        auto maybePipeLock = lock.tryGetPipe();
        if (maybePipeLock == kj::none) return js.resolvedPromise();
        auto& pipeLock = KJ_REQUIRE_NONNULL(maybePipeLock);

        KJ_IF_SOME(promise, pipeLock.checkSignal(js, *this)) {
          lock.releasePipeLock();
          return kj::mv(promise);
        } else {
        }  // Trailing else() is squash compiler warning

        if (result.done) {
          // We'll handle the close at the start of the next iteration.
          return pipeLoop(js);
        }

        auto onSuccess = JSG_VISITABLE_LAMBDA(
        (this, ref=addRef()), (ref) , (jsg::Lock& js) {
      return pipeLoop(js);
    } );

        auto onFailure = JSG_VISITABLE_LAMBDA(
        (ref=addRef(),&source, preventCancel, pipeThrough),
        (ref) , (jsg::Lock& js, jsg::Value value) {
      // The write failed. We handle it here because the pipe lock will have been released.
      auto reason = value.getHandle(js);
      if (!preventCancel) {
        source.release(js, reason);
      } else {
        source.release(js);
      }
      return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
    } );

        auto promise =
            write(js, result.value.map([&](jsg::Value& value) { return value.getHandle(js); }));

        return maybeAddFunctor(js, kj::mv(promise), kj::mv(onSuccess), kj::mv(onFailure));
      });

  auto onFailure =
      JSG_VISITABLE_LAMBDA((this, ref = addRef()), (ref), (jsg::Lock& js, jsg::Value value) {
        // The read failed. We will handle the error at the start of the next iteration.
        return pipeLoop(js);
      });

  return maybeAddFunctor(js, pipeLock.source.read(js), kj::mv(onSuccess), kj::mv(onFailure));
}

void WritableStreamJsController::updateBackpressure(jsg::Lock& js, bool backpressure) {
  KJ_IF_SOME(writerLock, lock.state.tryGet<WriterLocked>()) {
    if (backpressure) {
      // Per the spec, when backpressure is updated and is true, we replace the existing
      // ready promise on the writer with a new pending promise, regardless of whether
      // the existing one is resolved or not.
      auto prp = js.newPromiseAndResolver<void>();
      prp.promise.markAsHandled(js);
      return writerLock.setReadyFulfiller(prp);
    }

    // When backpressure is updated and is false, we resolve the ready promise on the writer
    maybeResolvePromise(js, writerLock.getReadyFulfiller());
  }
}

jsg::Promise<void> WritableStreamJsController::write(
    jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> value) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.rejectedPromise<void>(js.v8TypeError("This WritableStream has been closed."_kj));
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<void>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(controller, Controller) {
      return controller->write(js, value.orDefault([&] { return js.v8Undefined(); }));
    }
  }
  KJ_UNREACHABLE;
}

void WritableStreamJsController::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      visitor.visit(error);
    }
    KJ_CASE_ONEOF(controller, Controller) {
      visitor.visit(controller);
    }
  }
  visitor.visit(maybeAbortPromise, lock);
}

// =======================================================================================

TransformStreamDefaultController::TransformStreamDefaultController(jsg::Lock& js)
    : ioContext(tryGetIoContext()),
      startPromise(js.newPromiseAndResolver<void>()) {}

kj::Maybe<int> TransformStreamDefaultController::getDesiredSize() {
  KJ_IF_SOME(readableController, tryGetReadableController()) {
    return readableController.getDesiredSize();
  }
  return kj::none;
}

void TransformStreamDefaultController::enqueue(jsg::Lock& js, v8::Local<v8::Value> chunk) {
  auto& readableController = JSG_REQUIRE_NONNULL(tryGetReadableController(), TypeError,
      "The readable side of this TransformStream is no longer readable.");
  JSG_REQUIRE(readableController.canCloseOrEnqueue(), TypeError,
      "The readable side of this TransformStream is no longer readable.");
  js.tryCatch([&] { readableController.enqueue(js, chunk); }, [&](jsg::Value exception) {
    errorWritableAndUnblockWrite(js, exception.getHandle(js));
    js.throwException(kj::mv(exception));
  });

  bool newBackpressure = readableController.hasBackpressure();
  if (newBackpressure != backpressure) {
    KJ_ASSERT(newBackpressure);
    // Unfortunately the original implementation forgot to actually set the backpressure
    // here so the backpressure signaling failed to work correctly. This is unfortunate
    // because applying the backpressure here could break existing code, so we need to
    // put the fix behind a compat flag. Doh!
    if (FeatureFlags::get(js).getFixupTransformStreamBackpressure()) {
      setBackpressure(js, true);
    }
  }
}

void TransformStreamDefaultController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_SOME(readableController, tryGetReadableController()) {
    readableController.error(js, reason);
    readable = kj::none;
  }
  errorWritableAndUnblockWrite(js, reason);
}

void TransformStreamDefaultController::terminate(jsg::Lock& js) {
  KJ_IF_SOME(readableController, tryGetReadableController()) {
    readableController.close(js);
    readable = kj::none;
  }
  errorWritableAndUnblockWrite(js, js.v8TypeError("The transform stream has been terminated"_kj));
}

jsg::Promise<void> TransformStreamDefaultController::write(
    jsg::Lock& js, v8::Local<v8::Value> chunk) {
  KJ_IF_SOME(writableController, tryGetWritableController()) {
    KJ_IF_SOME(error, writableController.isErroredOrErroring(js)) {
      return js.rejectedPromise<void>(error);
    }

    KJ_ASSERT(writableController.isWritable());

    if (backpressure) {
      auto chunkRef = js.v8Ref(chunk);
      return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved(js).then(js,
          JSG_VISITABLE_LAMBDA((chunkRef = kj::mv(chunkRef), ref=JSG_THIS),
                              (chunkRef, ref), (jsg::Lock& js) mutable -> jsg::Promise<void> {
        KJ_IF_SOME(writableController, ref->tryGetWritableController()) {
          KJ_IF_SOME(error, writableController.isErroring(js)) {
            return js.rejectedPromise<void>(error);
          } else {
            // Else block to avert dangling else compiler warning.
          }
        } else {
          // Else block to avert dangling else compiler warning.
        }
        return ref->performTransform(js, chunkRef.getHandle(js));
      }));
    }
    return performTransform(js, chunk);
  } else {
    return js.rejectedPromise<void>(
        KJ_EXCEPTION(FAILED, "jsg.TypeError: Writing to the TransformStream failed."));
  }
}

jsg::Promise<void> TransformStreamDefaultController::abort(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_SOME(finish, algorithms.maybeFinish) {
    return finish.whenResolved(js);
  }
  return algorithms.maybeFinish
      .emplace(maybeRunAlgorithm(js, algorithms.cancel,
          JSG_VISITABLE_LAMBDA(
              (this, ref = JSG_THIS, reason = jsg::JsRef(js, jsg::JsValue(reason))), (ref, reason),
              (jsg::Lock & js)->jsg::Promise<void> {
                // If the readable side is errored, return a rejected promise with the stored error
                KJ_IF_SOME(controller, tryGetReadableController()) {
                KJ_IF_SOME(error, controller.getMaybeErrorState(js)) {
                return js.rejectedPromise<void>(kj::mv(error));
                } else {
                }  // Else block to avert dangling else compiler warning.
                } else {
                }  // Else block to avert dangling else compiler warning.

                // Otherwise... error with the given reason and resolve the abort promise
                error(js, reason.getHandle(js));
                return js.resolvedPromise();
              }),
          JSG_VISITABLE_LAMBDA((this, ref = JSG_THIS), (ref),
              (jsg::Lock & js, jsg::Value reason)->jsg::Promise<void> {
                error(js, reason.getHandle(js));
                return js.rejectedPromise<void>(kj::mv(reason));
              }),
          jsg::JsValue(reason)))
      .whenResolved(js);
}

jsg::Promise<void> TransformStreamDefaultController::close(jsg::Lock& js) {
  auto onSuccess =
      JSG_VISITABLE_LAMBDA((ref = JSG_THIS), (ref), (jsg::Lock & js)->jsg::Promise<void> {
        KJ_IF_SOME(readableController, ref->tryGetReadableController()) {
        // Allows for a graceful close of the readable side. Close will
        // complete once all of the queued data is read or the stream
        // errors.
        readableController.close(js);
        } else {
        // Else block to avert dangling else compiler warning.
        }
        return js.resolvedPromise();
      });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (ref = JSG_THIS), (ref), (jsg::Lock & js, jsg::Value reason)->jsg::Promise<void> {
        ref->error(js, reason.getHandle(js));
        return js.rejectedPromise<void>(kj::mv(reason));
      });

  return maybeRunAlgorithm(js, algorithms.flush, kj::mv(onSuccess), kj::mv(onFailure), JSG_THIS);
}

jsg::Promise<void> TransformStreamDefaultController::pull(jsg::Lock& js) {
  KJ_ASSERT(backpressure);
  setBackpressure(js, false);
  return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved(js);
}

jsg::Promise<void> TransformStreamDefaultController::cancel(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_SOME(finish, algorithms.maybeFinish) {
    return finish.whenResolved(js);
  }
  return algorithms.maybeFinish
      .emplace(maybeRunAlgorithm(js, algorithms.cancel,
          JSG_VISITABLE_LAMBDA(
              (this, ref = JSG_THIS, reason = jsg::JsRef(js, jsg::JsValue(reason))), (ref, reason),
              (jsg::Lock & js)->jsg::Promise<void> {
                readable = kj::none;
                errorWritableAndUnblockWrite(js, reason.getHandle(js));
                return js.resolvedPromise();
              }),
          JSG_VISITABLE_LAMBDA((this, ref = JSG_THIS), (ref),
              (jsg::Lock & js, jsg::Value reason)->jsg::Promise<void> {
                readable = kj::none;
                errorWritableAndUnblockWrite(js, reason.getHandle(js));
                return js.rejectedPromise<void>(kj::mv(reason));
              }),
          jsg::JsValue(reason)))
      .whenResolved(js);
}

jsg::Promise<void> TransformStreamDefaultController::performTransform(
    jsg::Lock& js, v8::Local<v8::Value> chunk) {
  if (algorithms.transform != kj::none) {
    return maybeRunAlgorithm(js, algorithms.transform,
        [](jsg::Lock& js) -> jsg::Promise<void> { return js.resolvedPromise(); },
        JSG_VISITABLE_LAMBDA((ref = JSG_THIS), (ref),
            (jsg::Lock & js, jsg::Value reason)->jsg::Promise<void> {
              ref->error(js, reason.getHandle(js));
              return js.rejectedPromise<void>(kj::mv(reason));
            }),
        chunk, JSG_THIS);
  }
  // If we got here, there is no transform algorithm. Per the spec, the default
  // behavior then is to just pass along the value untransformed.
  return js.tryCatch([&] {
    enqueue(js, chunk);
    return js.resolvedPromise();
  }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
}

void TransformStreamDefaultController::setBackpressure(jsg::Lock& js, bool newBackpressure) {
  KJ_ASSERT(newBackpressure != backpressure);
  KJ_IF_SOME(prp, maybeBackpressureChange) {
    prp.resolver.resolve(js);
  }
  maybeBackpressureChange = js.newPromiseAndResolver<void>();
  KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.markAsHandled(js);
  backpressure = newBackpressure;
}

void TransformStreamDefaultController::errorWritableAndUnblockWrite(
    jsg::Lock& js, v8::Local<v8::Value> reason) {
  algorithms.clear();
  KJ_IF_SOME(writableController, tryGetWritableController()) {
    if (writableController.isWritable()) {
      writableController.doError(js, reason);
    }
    writable = kj::none;
  }
  if (backpressure) {
    setBackpressure(js, false);
  }
}

void TransformStreamDefaultController::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(backpressureChange, maybeBackpressureChange) {
    visitor.visit(backpressureChange.promise, backpressureChange.resolver);
  }
  visitor.visit(writable, readable, startPromise.resolver, startPromise.promise, algorithms);
}

void TransformStreamDefaultController::init(jsg::Lock& js,
    jsg::Ref<ReadableStream>& readable,
    jsg::Ref<WritableStream>& writable,
    jsg::Optional<Transformer> maybeTransformer) {
  KJ_ASSERT(this->readable == kj::none);
  KJ_ASSERT(this->writable == kj::none);

  this->writable = writable.addRef();

  // The TransformStreamDefaultController needs to have a reference to the underlying controller
  // and not just the readable because if the readable is teed, or passed off to source, etc,
  // the TransformStream has to make sure that it can continue to interface with the controller
  // to push data into it.
  auto& readableController = static_cast<ReadableStreamJsController&>(readable->getController());
  auto readableRef = KJ_ASSERT_NONNULL(readableController.getController());
  this->readable = KJ_ASSERT_NONNULL(readableRef.tryGet<DefaultController>()).addRef();

  auto transformer = kj::mv(maybeTransformer).orDefault({});

  // TODO(someday): The stream standard includes placeholders for supporting byte-oriented
  // TransformStreams but does not yet define them. For now, we are limiting our implementation
  // here to only support value-based transforms.
  JSG_REQUIRE(transformer.readableType == kj::none, TypeError,
      "transformer.readableType must be undefined.");
  JSG_REQUIRE(transformer.writableType == kj::none, TypeError,
      "transformer.writableType must be undefined.");

  KJ_IF_SOME(transform, transformer.transform) {
    algorithms.transform = kj::mv(transform);
  }

  KJ_IF_SOME(flush, transformer.flush) {
    algorithms.flush = kj::mv(flush);
  }

  KJ_IF_SOME(cancel, transformer.cancel) {
    algorithms.cancel = kj::mv(cancel);
  }

  setBackpressure(js, true);

  maybeRunAlgorithm(js, transformer.start,
      JSG_VISITABLE_LAMBDA(
          (ref = JSG_THIS), (ref), (jsg::Lock& js) { ref->startPromise.resolver.resolve(js); }),
      JSG_VISITABLE_LAMBDA((ref = JSG_THIS), (ref),
          (jsg::Lock& js, jsg::Value reason) {
            ref->startPromise.resolver.reject(js, reason.getHandle(js));
          }),
      JSG_THIS);
}

kj::Maybe<ReadableStreamDefaultController&> TransformStreamDefaultController::
    tryGetReadableController() {
  KJ_IF_SOME(controller, readable) {
    return *controller;
  }
  return kj::none;
}

kj::Maybe<WritableStreamJsController&> TransformStreamDefaultController::
    tryGetWritableController() {
  KJ_IF_SOME(w, writable) {
    return static_cast<WritableStreamJsController&>(w->getController());
  }
  return kj::none;
}

template <class Self>
kj::StringPtr WritableImpl<Self>::jsgGetMemoryName() const {
  return "WritableImpl"_kjc;
}

template <class Self>
size_t WritableImpl<Self>::jsgGetMemorySelfSize() const {
  return sizeof(WritableImpl<Self>);
}

template <class Self>
void WritableImpl<Self>::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("signal", signal);

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      tracker.trackField("error", error);
    }
    KJ_CASE_ONEOF(erroring, StreamStates::Erroring) {
      tracker.trackField("erroring", erroring.reason);
    }
    KJ_CASE_ONEOF(writable, Writable) {}
  }

  tracker.trackField("abortAlgorithm", algorithms.abort);
  tracker.trackField("closeAlgorithm", algorithms.close);
  tracker.trackField("writeAlgorithm", algorithms.write);
  tracker.trackField("sizeAlgorithm", algorithms.size);

  for (auto& request: writeRequests) {
    tracker.trackField("pendingWrite", request);
  }

  tracker.trackField("inFlightWrite", inFlightWrite);
  tracker.trackField("inFlightClose", inFlightClose);
  tracker.trackField("closeRequest", closeRequest);
  tracker.trackField("maybePendingAbort", maybePendingAbort);
}

kj::StringPtr WritableStreamJsController::jsgGetMemoryName() const {
  return "WritableStreamJsController"_kjc;
}

size_t WritableStreamJsController::jsgGetMemorySelfSize() const {
  return sizeof(WritableStreamJsController);
}

void WritableStreamJsController::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      tracker.trackField("error", error);
    }
    KJ_CASE_ONEOF(controller, Controller) {
      tracker.trackField("controller", controller);
    }
  }
  tracker.trackField("lock", lock);
  tracker.trackField("maybeAbortPromise", maybeAbortPromise);
}

void WritableStreamDefaultController::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

kj::StringPtr ReadableStreamJsController::jsgGetMemoryName() const {
  return "ReadableStreamJsController"_kjc;
}

size_t ReadableStreamJsController::jsgGetMemorySelfSize() const {
  return sizeof(ReadableStreamJsController);
}

void ReadableStreamJsController::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      tracker.trackField("error", error);
    }
    KJ_CASE_ONEOF(readable, kj::Own<ValueReadable>) {
      tracker.trackField("readable", readable);
    }
    KJ_CASE_ONEOF(readable, kj::Own<ByteReadable>) {
      tracker.trackField("readable", readable);
    }
  }

  tracker.trackField("lock", lock);

  KJ_IF_SOME(pendingState, maybePendingState) {
    KJ_SWITCH_ONEOF(pendingState) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(error, StreamStates::Errored) {
        tracker.trackField("pendingError", error);
      }
    }
  }
}

template <class Self>
kj::StringPtr ReadableImpl<Self>::jsgGetMemoryName() const {
  return "ReadableImpl"_kjc;
}

template <class Self>
size_t ReadableImpl<Self>::jsgGetMemorySelfSize() const {
  return sizeof(ReadableImpl);
}

template <class Self>
void ReadableImpl<Self>::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(error, StreamStates::Errored) {
      tracker.trackField("error", error);
    }
    KJ_CASE_ONEOF(queue, Queue) {
      tracker.trackField("queue", queue);
    }
  }

  tracker.trackField("startAlgorithm", algorithms.start);
  tracker.trackField("pullAlgorithm", algorithms.pull);
  tracker.trackField("cancelAlgorithm", algorithms.cancel);
  tracker.trackField("sizeAlgorithm", algorithms.size);
  tracker.trackField("pendingCancel", maybePendingCancel);
}

void ReadableStreamBYOBRequest::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(impl, maybeImpl) {
    tracker.trackField("readRequest", impl.readRequest);
    tracker.trackField("controller", impl.controller);
    tracker.trackField("view", impl.view);
  }
}

void TransformStreamDefaultController::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("startPromise", startPromise);
  tracker.trackField("maybeBackpressureChange", maybeBackpressureChange);
  tracker.trackField("transformAlgorithm", algorithms.transform);
  tracker.trackField("flushAlgorithm", algorithms.flush);
  tracker.trackField("writable", writable);
  tracker.trackField("readable", readable);
}

// ======================================================================================

jsg::Ref<ReadableStream> ReadableStream::from(
    jsg::Lock& js, jsg::AsyncGenerator<jsg::Value> generator) {

  // AsyncGenerator is not a refcounted type, so we need to wrap it in a refcounted
  // struct so that we can keep it alive through the various promise branches below.
  struct RefcountedGenerator: public kj::Refcounted {
    jsg::AsyncGenerator<jsg::Value> generator;
    RefcountedGenerator(jsg::AsyncGenerator<jsg::Value> generator): generator(kj::mv(generator)) {}
  };
  auto rcGenerator = kj::refcounted<RefcountedGenerator>(kj::mv(generator));

  return constructor(js,
      UnderlyingSource{
        .pull =
            [generator = kj::addRef(*rcGenerator)](jsg::Lock& js, auto controller) mutable {
    auto& c = controller.template get<DefaultController>();
    return generator->generator.next(js).then(js,
        JSG_VISITABLE_LAMBDA((controller = c.addRef(), generator = kj::addRef(*generator)),
            (controller),
            (jsg::Lock& js, kj::Maybe<jsg::Value> value) {
              KJ_IF_SOME(v, value) {
              controller->enqueue(js, v.getHandle(js));
              } else {
              controller->close(js);
              }
              return js.resolvedPromise();
            }),
        JSG_VISITABLE_LAMBDA((controller = c.addRef(), generator = kj::addRef(*generator)),
            (controller), (jsg::Lock& js, jsg::Value reason) {
              controller->error(js, reason.getHandle(js));
              return js.rejectedPromise<void>(kj::mv(reason));
            }));
  },
        .cancel =
            [generator = kj::addRef(*rcGenerator)](jsg::Lock& js, auto reason) mutable {
    return generator->generator.return_(js, kj::none)
        .then(js, [genertor = kj::mv(generator)](auto& lock) {});
  },
      },
      StreamQueuingStrategy{
        .highWaterMark = 0,
      });
}

}  // namespace workerd::api
