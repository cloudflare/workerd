// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "standard.h"
#include "readable.h"
#include "writable.h"
#include <workerd/jsg/buffersource.h>
#include <iterator>

namespace workerd::api {

kj::Own<ReadableStreamJsController> newReadableStreamJsController() {
  return kj::heap<ReadableStreamJsController>();
}

namespace {
auto maybeAddFunctor(jsg::Lock& js, auto promise, auto onSuccess, auto onFailure) {
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    return promise.then(js, ioContext.addFunctor(kj::mv(onSuccess)),
                            ioContext.addFunctor(kj::mv(onFailure)));
  } else {
    return promise.then(js, kj::mv(onSuccess), kj::mv(onFailure));
  }
}
}  // namespace

namespace jscontroller {
jsg::Promise<void> maybeRunAlgorithm(
    jsg::Lock& js,
    auto& maybeAlgorithm,
    auto&& onSuccess,
    auto&& onFailure,
    auto&&...args) {
  // The algorithm is a JavaScript function mapped through jsg::Function.
  // It is expected to return a Promise mapped via jsg::Promise. If the
  // function returns synchronously, the jsg::Promise wrapper ensures
  // that it is properly mapped to a jsg::Promise, but if the Promise
  // throws synchronously, we have to convert that synchronous throw
  // into a proper rejected jsg::Promise.

  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    KJ_IF_MAYBE(algorithm, maybeAlgorithm) {
      return js.tryCatch([&] {
        return (*algorithm)(js, kj::fwd<decltype(args)>(args)...);
      }, [&](jsg::Value&& exception) {
        return js.rejectedPromise<void>(kj::mv(exception));
      }).then(js, ioContext.addFunctor(kj::mv(onSuccess)),
                  ioContext.addFunctor(kj::mv(onFailure)));
    }
  } else {
    KJ_IF_MAYBE(algorithm, maybeAlgorithm) {
      return js.tryCatch([&] {
        return (*algorithm)(js, kj::fwd<decltype(args)>(args)...);
      }, [&](jsg::Value&& exception) {
        return js.rejectedPromise<void>(kj::mv(exception));
      }).then(js, kj::mv(onSuccess), kj::mv(onFailure));
    }
  }

  // If the algorithm does not exist, we just handle it as a success and move on.
  onSuccess(js);
  return js.resolvedPromise();
}

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
bool ReadableLockImpl<Controller>::lockReader(
    jsg::Lock& js,
    Controller& self,
    Reader& reader) {
  if (isLockedToReader()) {
    return false;
  }

  auto prp = js.newPromiseAndResolver<void>();
  prp.promise.markAsHandled();

  auto lock = ReaderLocked(reader, kj::mv(prp.resolver));

  if (self.state.template is<StreamStates::Closed>()) {
    maybeResolvePromise(lock.getClosedFulfiller());
  } else KJ_IF_MAYBE(errored, self.state.template tryGet<StreamStates::Errored>()) {
    maybeRejectPromise<void>(lock.getClosedFulfiller(), errored->getHandle(js));
  }

  state = kj::mv(lock);
  reader.attach(self, kj::mv(prp.promise));
  return true;
}

template <typename Controller>
void ReadableLockImpl<Controller>::releaseReader(
    Controller& self,
    Reader& reader,
    kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_MAYBE(locked, state.template tryGet<ReaderLocked>()) {
    KJ_ASSERT(&locked->getReader() == &reader);

    KJ_IF_MAYBE(js, maybeJs) {
      JSG_REQUIRE(!self.hasPendingReadRequests(),
                  TypeError,
                  "Cannot call releaseLock() on a reader with outstanding read promises.");

      maybeRejectPromise<void>(
          locked->getClosedFulfiller(),
          js->v8TypeError("This ReadableStream reader has been released."_kj));
    }

    // Keep the kj::mv(locked) after the isolate and hasPendingReadRequests check above.
    // Moving will release the references and we don't want to do that if the hasPendingReadRequests
    // check fails.
    auto lock = kj::mv(*locked);

    // When maybeJs is nullptr, that means releaseReader was called when the reader is
    // being deconstructed and not as the result of explicitly calling releaseLock and
    // we do not have an isolate lock. In that case, we don't want to change the lock
    // state itself. Moving the lock above will free the lock state while keeping the
    // ReadableStream marked as locked.
    if (maybeJs != nullptr) {
      state.template init<Unlocked>();
    }
  }
}

template <typename Controller>
kj::Maybe<ReadableStreamController::PipeController&> ReadableLockImpl<Controller>::tryPipeLock(
    Controller& self,
    jsg::Ref<WritableStream> destination) {
  if (isLockedToReader()) {
    return nullptr;
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
void ReadableLockImpl<Controller>::onClose() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      maybeResolvePromise(locked.getClosedFulfiller());
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
      maybeRejectPromise<void>(locked.getClosedFulfiller(), reason);
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
  return nullptr;
}

template <typename Controller>
jsg::Promise<ReadResult> ReadableLockImpl<Controller>::PipeLocked::read(jsg::Lock& js) {
  return KJ_ASSERT_NONNULL(inner.read(js, nullptr));
}

template <typename Controller>
void ReadableLockImpl<Controller>::PipeLocked::visitForGc(jsg::GcVisitor &visitor) {
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
  closedPrp.promise.markAsHandled();
  auto readyPrp = js.newPromiseAndResolver<void>();
  readyPrp.promise.markAsHandled();

  auto lock = WriterLocked(writer, kj::mv(closedPrp.resolver), kj::mv(readyPrp.resolver));

  if (self.state.template is<StreamStates::Closed>()) {
    maybeResolvePromise(lock.getClosedFulfiller());
    maybeResolvePromise(lock.getReadyFulfiller());
  } else KJ_IF_MAYBE(errored, self.state.template tryGet<StreamStates::Errored>()) {
    maybeRejectPromise<void>(lock.getClosedFulfiller(), errored->getHandle(js));
    maybeRejectPromise<void>(lock.getReadyFulfiller(), errored->getHandle(js));
  } else {
    if (self.isStarted()) {
      maybeResolvePromise(lock.getReadyFulfiller());
    }
  }

  state = kj::mv(lock);
  writer.attach(self, kj::mv(closedPrp.promise), kj::mv(readyPrp.promise));
  return true;
}

template <typename Controller>
void WritableLockImpl<Controller>::releaseWriter(
    Controller& self,
    Writer& writer,
    kj::Maybe<jsg::Lock&> maybeJs) {
  auto& locked = state.template get<WriterLocked>();
  KJ_ASSERT(&locked.getWriter() == &writer);
  KJ_IF_MAYBE(js, maybeJs) {
    maybeRejectPromise<void>(
        locked.getClosedFulfiller(),
        js->v8TypeError("This WritableStream writer has been released."_kj));
  }
  auto lock = kj::mv(locked);

  // When maybeJs is nullptr, that means releaseWriter was called when the writer is
  // being deconstructed and not as the result of explicitly calling releaseLock and
  // we do not have an isolate lock. In that case, we don't want to change the lock
  // state itself. Moving the lock above will free the lock state while keeping the
  // WritableStream marked as locked.
  if (maybeJs != nullptr) {
    state.template init<Unlocked>();
  }
}

template <typename Controller>
bool WritableLockImpl<Controller>::pipeLock(
    WritableStream& owner,
    jsg::Ref<ReadableStream> source,
    PipeToOptions& options) {
  if (isLockedToWriter()) {
    return false;
  }

  auto& sourceLock = KJ_ASSERT_NONNULL(source->getController().tryPipeLock(owner.addRef()));

  state.template init<PipeLocked>(PipeLocked {
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
      KJ_IF_MAYBE(signal, locked.maybeSignal) {
        visitor.visit(*signal);
      }
    }
  }
}

template <typename Controller>
kj::Maybe<jsg::Promise<void>> WritableLockImpl<Controller>::PipeLocked::checkSignal(
    jsg::Lock& js,
    Controller& self) {
  KJ_IF_MAYBE(signal, maybeSignal) {
    if ((*signal)->getAborted()) {
      auto reason = (*signal)->getReason(js);
      if (!preventCancel) {
        source.release(js, reason);
      } else {
        source.release(js);
      }
      if (!preventAbort) {
        return self.abort(js, reason).then(js,
            JSG_VISITABLE_LAMBDA((this, reason = js.v8Ref(reason), ref = self.addRef()),
                                 (reason, ref), (jsg::Lock& js) {
          return rejectedMaybeHandledPromise<void>(
              js,
              reason.getHandle(js),
              pipeThrough);
        }));
      }
      return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
    }
  }
  return nullptr;
}

// ======================================================================================

namespace {
int getHighWaterMark(const UnderlyingSource& underlyingSource,
                     const StreamQueuingStrategy& queuingStrategy) {
  bool isBytes = underlyingSource.type.map([](auto& s) { return s == "bytes"; }).orDefault(false);
  return queuingStrategy.highWaterMark.orDefault(isBytes ? 0 : 1);
}
}  // namespace

template <typename Self>
ReadableImpl<Self>::ReadableImpl(
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy)
    : state(Queue(getHighWaterMark(underlyingSource, queuingStrategy))),
      algorithms(kj::mv(underlyingSource), kj::mv(queuingStrategy)) {}

template <typename Self>
void ReadableImpl<Self>::start(jsg::Lock& js, jsg::Ref<Self> self) {
  KJ_ASSERT(!started && algorithms.starting == nullptr);

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    algorithms.starting = nullptr;
    started = true;
    pullIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                        (jsg::Lock& js, jsg::Value reason) {
    algorithms.starting = nullptr;
    started = true;
    doError(js, kj::mv(reason));
  });

  algorithms.starting = maybeRunAlgorithm(js,
                                          algorithms.start,
                                          kj::mv(onSuccess),
                                          kj::mv(onFailure),
                                          kj::mv(self));
  algorithms.start = nullptr;
}

template <typename Self>
size_t ReadableImpl<Self>::consumerCount() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return 0; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return 0; }
    KJ_CASE_ONEOF(queue, Queue) {
      return queue.getConsumerCount();
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
jsg::Promise<void> ReadableImpl<Self>::cancel(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
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
        // canceling it's interest in the underlying source but we do not yet
        // want to cancel the underlying source since there are still other
        // consumers that want data.
        return js.resolvedPromise();
      }

      // Otherwise, there should be exactly one consumer at this point.
      KJ_ASSERT(consumerCount == 1);
      KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
        // If we're already waiting for cancel to complete, just return the
        // already existing pending promise.
        // This shouldn't happen but we handle the case anyway, just to be safe.
        return pendingCancel->promise.whenResolved();
      }

      auto prp = js.newPromiseAndResolver<void>();
      maybePendingCancel = PendingCancel {
        .fulfiller = kj::mv(prp.resolver),
        .promise = kj::mv(prp.promise),
      };
      auto promise = KJ_ASSERT_NONNULL(maybePendingCancel).promise.whenResolved();
      doCancel(js, kj::mv(self), reason);
      return kj::mv(promise);
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
bool ReadableImpl<Self>::canCloseOrEnqueue() {
  return state.template is<Queue>() && !closeRequested;
}

template <typename Self>
void ReadableImpl<Self>::doCancel(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  // doCancel() is triggered by cancel() being called, which is an explicit signal from
  // the ReadableStream that we don't care about the data this controller provides any
  // more. We don't need to notify the consumers because we presume they already know
  // that they called cancel. What we do want to do here, tho, is close the implementation
  // and trigger the cancel algorithm.

  state.template init<StreamStates::Closed>();

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    algorithms.canceling = nullptr;
    doClose(js);
    KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
      maybeResolvePromise(pendingCancel->fulfiller);
    }
  });
  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                        (jsg::Lock& js, jsg::Value reason) {
    algorithms.canceling = nullptr;
    // We do not call doError() here because there's really no point. Everything
    // that cares about the state of this controller impl has signaled that it
    // no longer cares and has gone away.
    doClose(js);
    KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
      maybeRejectPromise<void>(pendingCancel->fulfiller, reason.getHandle(js));
    }
  });

  algorithms.canceling = maybeRunAlgorithm(js,
                                           algorithms.cancel,
                                           kj::mv(onSuccess),
                                           kj::mv(onFailure),
                                           reason);
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
    auto error = js.v8Ref(js.v8TypeError(
        "This ReadableStream was closed with a partial read pending."));
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
      return nullptr;
    }
    KJ_CASE_ONEOF(queue, Queue) {
      return queue.desiredSize();
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
bool ReadableImpl<Self>::shouldCallPull() {
  // We should call pull if any of the consumers known to the queue have read requests or
  // we haven't yet signalled backpressure.
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
    algorithms.pulling = nullptr;
    pulling = false;
    if (pullAgain) {
      pullAgain = false;
      pullIfNeeded(js, kj::mv(self));
    }
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                           (jsg::Lock& js, jsg::Value reason) {
    algorithms.pulling = nullptr;
    doError(js, kj::mv(reason));
  });

  algorithms.pulling = maybeRunAlgorithm(js,
                                         algorithms.pull,
                                         kj::mv(onSuccess),
                                         kj::mv(onFailure),
                                         self.addRef());
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
  KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
    visitor.visit(pendingCancel->fulfiller, pendingCancel->promise);
  }
  visitor.visit(algorithms);
}

template <typename Self>
kj::Own<typename ReadableImpl<Self>::Consumer>
ReadableImpl<Self>::getConsumer(kj::Maybe<ReadableImpl<Self>::StateListener&> listener) {
  auto& queue = state.template get<Queue>();
  return kj::heap<typename ReadableImpl<Self>::Consumer>(queue, listener);
}

template <typename Self>
bool ReadableImpl<Self>::hasPendingReadRequests() {
  KJ_IF_MAYBE(queue, state.template tryGet<Queue>()) {
    return queue->wantsRead();
  }
  return false;
}

// ======================================================================================

template <typename Self>
WritableImpl<Self>::WritableImpl(WriterOwner& owner)
    : owner(owner),
      signal(jsg::alloc<AbortSignal>()) {}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::abort(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  signal->triggerAbort(js, reason);

  // We have to check this again after the AbortSignal is triggered.
  if (state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>()) {
    return js.resolvedPromise();
  }

  KJ_IF_MAYBE(pendingAbort, maybePendingAbort) {
    // Notice here that, per the spec, the reason given in this call of abort is
    // intentionally ignored if there is already an abort pending.
    return pendingAbort->whenResolved();
  }

  bool wasAlreadyErroring = false;
  if (state.is<StreamStates::Erroring>()) {
    wasAlreadyErroring = true;
    reason = js.v8Undefined();
  }

  KJ_DEFER(
    if (!wasAlreadyErroring) {
      startErroring(js, kj::mv(self), reason);
    }
  );

  maybePendingAbort = PendingAbort(js, reason, wasAlreadyErroring);
  return KJ_ASSERT_NONNULL(maybePendingAbort).whenResolved();
}

template <typename Self>
ssize_t WritableImpl<Self>::getDesiredSize() {
  return highWaterMark - amountBuffered;
}

template <typename Self>
void WritableImpl<Self>::advanceQueueIfNeeded(jsg::Lock& js, jsg::Ref<Self> self) {
  if (!started || inFlightWrite != nullptr) {
    return;
  }
  KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());

  if (state.template is<StreamStates::Erroring>()) {
    return finishErroring(js, kj::mv(self));
  }

  if (writeRequests.empty()) {
    KJ_IF_MAYBE(req, closeRequest) {
      KJ_ASSERT(inFlightClose == nullptr);
      KJ_ASSERT_NONNULL(closeRequest);
      inFlightClose = kj::mv(closeRequest);

      auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
        algorithms.closing = nullptr;
        finishInFlightClose(js, kj::mv(self));
      });

      auto onFailure = JSG_VISITABLE_LAMBDA(
          (this, self = self.addRef()), (self), (jsg::Lock& js, jsg::Value reason) {
        algorithms.closing = nullptr;
        finishInFlightClose(js, kj::mv(self), reason.getHandle(js));
      });

      algorithms.closing = maybeRunAlgorithm(js,
                                            algorithms.close,
                                            kj::mv(onSuccess),
                                            kj::mv(onFailure));
    }
    return;
  }

  KJ_ASSERT(inFlightWrite == nullptr);
  auto req = dequeueWriteRequest();
  auto value = req.value.addRef(js);
  auto size = req.size;
  inFlightWrite = kj::mv(req);

  auto onSuccess = JSG_VISITABLE_LAMBDA(
      (this, self = self.addRef(), size), (self), (jsg::Lock& js) {
    amountBuffered -= size;
    algorithms.writing = nullptr;
    finishInFlightWrite(js, self.addRef());
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
    if (!isCloseQueuedOrInFlight() && state.template is<Writable>()) {
      updateBackpressure(js);
    }
    advanceQueueIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef(), size), (self),
                                        (jsg::Lock& js, jsg::Value reason) {
    amountBuffered -= size;
    algorithms.writing = nullptr;
    finishInFlightWrite(js, kj::mv(self), reason.getHandle(js));
  });

  algorithms.writing = maybeRunAlgorithm(js,
                                         algorithms.write,
                                         kj::mv(onSuccess),
                                         kj::mv(onFailure),
                                         value.getHandle(js),
                                         self.addRef());
}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::close(jsg::Lock& js, jsg::Ref<Self> self) {
  KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
  JSG_REQUIRE(!isCloseQueuedOrInFlight(), TypeError,
      "Cannot close a writer that is already being closed");
  auto prp = js.newPromiseAndResolver<void>();
  closeRequest = kj::mv(prp.resolver);

  if (backpressure && state.template is<Writable>() && owner != nullptr) {
    getOwner().maybeResolveReadyPromise();
  }

  advanceQueueIfNeeded(js, kj::mv(self));

  return kj::mv(prp.promise);
}

template <typename Self>
void WritableImpl<Self>::dealWithRejection(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  if (state.template is<Writable>()) {
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
void WritableImpl<Self>::doClose() {
  KJ_ASSERT(closeRequest == nullptr);
  KJ_ASSERT(inFlightClose == nullptr);
  KJ_ASSERT(inFlightWrite == nullptr);
  KJ_ASSERT(maybePendingAbort == nullptr);
  KJ_ASSERT(writeRequests.empty());
  state.template init<StreamStates::Closed>();
  algorithms.clear();

  KJ_IF_MAYBE(theOwner, owner) {
    theOwner->doClose();
    owner = nullptr;
    // Calling doClose here most likely caused the WritableImpl<Self> to be destroyed,
    // so it is important not to do anything else after calling doClose here.
  }
}

template <typename Self>
void WritableImpl<Self>::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_ASSERT(closeRequest == nullptr);
  KJ_ASSERT(inFlightClose == nullptr);
  KJ_ASSERT(inFlightWrite == nullptr);
  KJ_ASSERT(maybePendingAbort == nullptr);
  KJ_ASSERT(writeRequests.empty());
  state = js.v8Ref(reason);
  algorithms.clear();

  KJ_IF_MAYBE(theOwner, owner) {
    theOwner->doError(js, reason);
    owner = nullptr;
    // Calling doError here most likely caused the WritableImpl<Self> to be destroyed,
    // so it is important not to do anything else after calling doError here.
  }
}

template <typename Self>
void WritableImpl<Self>::error(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  if (state.template is<Writable>()) {
    algorithms.clear();
    startErroring(js, kj::mv(self), reason);
  }
}

template <typename Self>
void WritableImpl<Self>::finishErroring(jsg::Lock& js, jsg::Ref<Self> self) {
  auto erroring = kj::mv(KJ_ASSERT_NONNULL(state.template tryGet<StreamStates::Erroring>()));
  auto reason = erroring.reason.getHandle(js);
  KJ_ASSERT(inFlightWrite == nullptr);
  KJ_ASSERT(inFlightClose == nullptr);
  state.template init<StreamStates::Errored>(kj::mv(erroring.reason));

  while (!writeRequests.empty()) {
    dequeueWriteRequest().resolver.reject(reason);
  }
  KJ_ASSERT(writeRequests.empty());

  KJ_IF_MAYBE(pendingAbort, maybePendingAbort) {
    if (pendingAbort->reject) {
      pendingAbort->fail(reason);
      return rejectCloseAndClosedPromiseIfNeeded(js);
    }

    auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
      algorithms.aborting = nullptr;
      auto& pendingAbort = KJ_ASSERT_NONNULL(maybePendingAbort);
      pendingAbort.reject = false;
      pendingAbort.complete(js);
      rejectCloseAndClosedPromiseIfNeeded(js);
    });

    auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                          (jsg::Lock& js, jsg::Value reason) {
      algorithms.aborting = nullptr;
      auto& pendingAbort = KJ_ASSERT_NONNULL(maybePendingAbort);
      pendingAbort.fail(reason.getHandle(js));
      rejectCloseAndClosedPromiseIfNeeded(js);
    });

    algorithms.aborting = maybeRunAlgorithm(js,
                                            algorithms.abort,
                                            kj::mv(onSuccess),
                                            kj::mv(onFailure),
                                            reason);
    return;
  }
  rejectCloseAndClosedPromiseIfNeeded(js);
}

template <typename Self>
void WritableImpl<Self>::finishInFlightClose(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    kj::Maybe<v8::Local<v8::Value>> maybeReason) {
  algorithms.clear();
  KJ_ASSERT_NONNULL(inFlightClose);
  KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());

  KJ_IF_MAYBE(reason, maybeReason) {
    maybeRejectPromise<void>(inFlightClose, *reason);

    KJ_IF_MAYBE(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
      pendingAbort->fail(*reason);
    }

    return dealWithRejection(js, kj::mv(self), *reason);
  }

  maybeResolvePromise(inFlightClose);

  if (state.template is<StreamStates::Erroring>()) {
    KJ_IF_MAYBE(pendingAbort, PendingAbort::dequeue(maybePendingAbort)) {
      pendingAbort->reject = false;
      pendingAbort->complete(js);
    }
  }
  KJ_ASSERT(maybePendingAbort == nullptr);

  state.template init<StreamStates::Closed>();
  doClose();
}

template <typename Self>
void WritableImpl<Self>::finishInFlightWrite(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    kj::Maybe<v8::Local<v8::Value>> maybeReason) {
  auto& write = KJ_ASSERT_NONNULL(inFlightWrite);

  KJ_IF_MAYBE(reason, maybeReason) {
    write.resolver.reject(js, *reason);
    inFlightWrite = nullptr;
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
    return dealWithRejection(js, kj::mv(self), *reason);
  }

  write.resolver.resolve();
  inFlightWrite = nullptr;
}

template <typename Self>
bool WritableImpl<Self>::isCloseQueuedOrInFlight() {
  return closeRequest != nullptr || inFlightClose != nullptr;
}

template <typename Self>
void WritableImpl<Self>::rejectCloseAndClosedPromiseIfNeeded(jsg::Lock& js) {
  algorithms.clear();
  auto reason =
      KJ_ASSERT_NONNULL(state.template tryGet<StreamStates::Errored>()).getHandle(js);
  maybeRejectPromise<void>(closeRequest, reason);
  PendingAbort::dequeue(maybePendingAbort);
  doError(js, reason);
}

template <typename Self>
void WritableImpl<Self>::setup(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    UnderlyingSink underlyingSink,
    StreamQueuingStrategy queuingStrategy) {

  highWaterMark = queuingStrategy.highWaterMark.orDefault(1);
  auto startAlgorithm = kj::mv(underlyingSink.start);
  algorithms.write = kj::mv(underlyingSink.write);
  algorithms.close = kj::mv(underlyingSink.close);
  algorithms.abort = kj::mv(underlyingSink.abort);
  algorithms.size = kj::mv(queuingStrategy.size);

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    algorithms.starting = nullptr;
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());

    if (state.template is<Writable>() && owner != nullptr) {
      // Only resolve the ready promise if an abort is not pending.
      // It will have been rejected already.
      getOwner().maybeResolveReadyPromise();
    }

    started = true;
    advanceQueueIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                        (jsg::Lock& js, jsg::Value reason) {
    algorithms.starting = nullptr;
    auto handle = reason.getHandle(js);
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
    KJ_IF_MAYBE(theOwner, owner) {
      theOwner->maybeRejectReadyPromise(js, handle);
    }
    started = true;
    dealWithRejection(js, kj::mv(self), handle);
  });

  backpressure = getDesiredSize() < 0;

  algorithms.starting = maybeRunAlgorithm(js,
                                          startAlgorithm,
                                          kj::mv(onSuccess),
                                          kj::mv(onFailure),
                                          self.addRef());
}

template <typename Self>
void WritableImpl<Self>::startErroring(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  KJ_ASSERT(state.template is<Writable>());
  KJ_IF_MAYBE(theOwner, owner) {
    theOwner->maybeRejectReadyPromise(js, reason);
  }
  state.template init<StreamStates::Erroring>(js.v8Ref(reason));
  if (inFlightWrite == nullptr && inFlightClose == nullptr && started) {
    finishErroring(js, kj::mv(self));
  }
}

template <typename Self>
void WritableImpl<Self>::updateBackpressure(jsg::Lock& js) {
  KJ_ASSERT(state.template is<Writable>());
  KJ_ASSERT(!isCloseQueuedOrInFlight());
  bool bp = getDesiredSize() < 0;
  if (bp != backpressure) {
    backpressure = bp;
    KJ_IF_MAYBE(theOwner, owner) {
      theOwner->updateBackpressure(js, backpressure);
    }
  }
}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::write(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> value) {

  size_t size = 1;
  KJ_IF_MAYBE(sizeFunc, algorithms.size) {
    kj::Maybe<jsg::Value> failure;
    js.tryCatch([&] {
      size = (*sizeFunc)(js, value);
    }, [&](jsg::Value exception) {
      startErroring(js, self.addRef(), exception.getHandle(js));
      failure = kj::mv(exception);
    });
    KJ_IF_MAYBE(exception, failure) {
      return js.rejectedPromise<void>(kj::mv(*exception));
    }
  }

  KJ_IF_MAYBE(error, state.tryGet<StreamStates::Errored>()) {
    return js.rejectedPromise<void>(error->addRef(js));
  }

  if (isCloseQueuedOrInFlight() || state.is<StreamStates::Closed>()) {
    return js.rejectedPromise<void>(js.v8TypeError("This ReadableStream is closed."_kj));
  }

  KJ_IF_MAYBE(erroring, state.tryGet<StreamStates::Erroring>()) {
    return js.rejectedPromise<void>(erroring->reason.addRef(js));
  }

  KJ_ASSERT(state.template is<Writable>());

  auto prp = js.newPromiseAndResolver<void>();
  writeRequests.push_back(WriteRequest {
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
void WritableImpl<Self>::visitForGc(jsg::GcVisitor &visitor) {
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
  visitor.visit(inFlightWrite,
                inFlightClose,
                closeRequest,
                algorithms,
                signal,
                maybePendingAbort);
  visitor.visitAll(writeRequests);
}
}  // namespace jscontroller

// ======================================================================================

namespace {
template <typename T>
kj::Maybe<T&> tryGetAs(auto& ref) {
  KJ_IF_MAYBE(ptr, ref->tryGet()) {
    return kj::dynamicDowncastIfAvailable<T>(*ptr);
  }
  return nullptr;
}

class AllReaderBase {
public:
  AllReaderBase() : ref(kj::refcounted<WeakRef<AllReaderBase>>(*this)) {}
  virtual ~AllReaderBase() noexcept(false) {
    ref->reset();
  }
  KJ_DISALLOW_COPY_AND_MOVE(AllReaderBase);

  virtual void doClose() = 0;
  virtual void doError(jsg::Lock& js, v8::Local<v8::Value> reason) = 0;

  kj::Own<WeakRef<AllReaderBase>> addWeakRef() { return ref->addRef(); }

private:
  kj::Own<WeakRef<AllReaderBase>> ref;
};

template <typename Controller, typename Consumer>
struct ReadableState {
  using AllReaderWeakRef = kj::Own<WeakRef<AllReaderBase>>;
  jsg::Ref<Controller> controller;
  kj::Maybe<kj::OneOf<ReadableStreamJsController*, AllReaderWeakRef>> owner;
  kj::Own<Consumer> consumer;

  ReadableState(
      jsg::Ref<Controller> controller, auto owner, auto stateListener)
      : controller(kj::mv(controller)),
        owner(kj::mv(owner)),
        consumer(this->controller->getConsumer(stateListener)) {}

  ReadableState(jsg::Ref<Controller> controller, auto owner, kj::Own<Consumer> consumer)
      : controller(kj::mv(controller)),
        owner(owner),
        consumer(kj::mv(consumer)) {}

  void setOwner(auto newOwner) {
    owner = kj::mv(newOwner);
  }

  bool hasPendingReadRequests() {
    return consumer->hasReadRequests();
  }

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    consumer->cancel(js, maybeReason);
    return controller->cancel(js, kj::mv(maybeReason));
  }

  void consumerClose() {
    KJ_IF_MAYBE(o, owner) {
      KJ_SWITCH_ONEOF(*o) {
        KJ_CASE_ONEOF(controller, ReadableStreamJsController*) {
          return controller->doClose();
        }
        KJ_CASE_ONEOF(reader, AllReaderWeakRef) {
          KJ_IF_MAYBE(base, reader->tryGet()) {
            base->doClose();
          }
          return;
        }
      }
      KJ_UNREACHABLE;
    }
  }

  void consumerError(jsg::Lock& js, jsg::Value reason) {
    KJ_IF_MAYBE(o, owner) {
      KJ_SWITCH_ONEOF(*o) {
        KJ_CASE_ONEOF(controller, ReadableStreamJsController*) {
          return controller->doError(js, reason.getHandle(js));
        }
        KJ_CASE_ONEOF(reader, AllReaderWeakRef) {
          KJ_IF_MAYBE(base, reader->tryGet()) {
            base->doError(js, reason.getHandle(js));
          }
          return;
        }
      }
      KJ_UNREACHABLE;
    }
  }

  void consumerWantsData(jsg::Lock& js) {
    controller->pull(js);
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(*consumer);
  }

  ReadableState cloneWithNewOwner(jsg::Lock& js, auto owner, auto stateListener) {
    return ReadableState(controller.addRef(), owner, consumer->clone(js, stateListener));
  }

  kj::Maybe<int> getDesiredSize() {
    return controller->getDesiredSize();
  }

  bool canCloseOrEnqueue() {
    return controller->canCloseOrEnqueue();
  }

  jsg::Ref<Controller> getControllerRef() {
    return controller.addRef();
  }
};
}  // namespace

struct ValueReadable final: public api::ValueQueue::ConsumerImpl::StateListener,
                            public kj::Refcounted {
  using State = ReadableState<ReadableStreamDefaultController, api::ValueQueue::Consumer>;
  kj::Maybe<State> state;

  ValueReadable(jsg::Ref<ReadableStreamDefaultController> controller, auto owner)
      : state(State(kj::mv(controller), owner, this)) {}

  ValueReadable(jsg::Lock& js, auto owner, ValueReadable& other)
      : state(KJ_ASSERT_NONNULL(other.state).cloneWithNewOwner(js, owner, this)) {}

  KJ_DISALLOW_COPY_AND_MOVE(ValueReadable);

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(state);
  }

  bool hasPendingReadRequests() {
    return state.map([](State& state) { return state.hasPendingReadRequests(); }).orDefault(false);
  }

  void setOwner(auto newOwner) {
    KJ_IF_MAYBE(s, state) { s->setOwner(kj::mv(newOwner)); }
  }

  kj::Own<ValueReadable> clone(jsg::Lock& js, ReadableStreamJsController* owner) {
    // A single ReadableStreamDefaultController can have multiple consumers.
    // When the ValueReadable constructor is used, the new consumer is added
    // and starts to receive new data that becomes enqueued. When clone
    // is used, any state currently held by this consumer is copied to the
    // new consumer.
    return kj::refcounted<ValueReadable>(js, owner, *this);
  }

  jsg::Promise<ReadResult> read(jsg::Lock& js) {
    KJ_IF_MAYBE(s, state) {
      // It's possible for the controller to be closed synchronously while the
      // read operation is executing. In that case, we want to make sure we keep
      // a reference so it'll survice at least long enough for the read method
      // to complete.
      auto self KJ_UNUSED = kj::addRef(*this);

      auto prp = js.newPromiseAndResolver<ReadResult>();
      s->consumer->read(js, ValueQueue::ReadRequest {
        .resolver = kj::mv(prp.resolver),
      });
      return kj::mv(prp.promise);
    }

    // We are canceled! There's nothing to do.
    return js.resolvedPromise(ReadResult { .done = true });
  }

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    // When a ReadableStream is canceled, the expected behavior is that the underlying
    // controller is notified and the cancel algorithm on the underlying source is
    // called. When there are multiple ReadableStreams sharing consumption of a
    // controller, however, it should act as a shared pointer of sorts, canceling
    // the underlying controller only when the last reader is canceled.
    // Here, we rely on the controller implementing the correct behavior since it owns
    // the queue that knows about all of the attached consumers.
    KJ_IF_MAYBE(s, state) {
      KJ_DEFER({
        state = nullptr;
      });
      return s->cancel(js, kj::mv(maybeReason));
    }

    return js.resolvedPromise();
  }

  void onConsumerClose(jsg::Lock& js) override {
    // Called by the consumer when a state change to closed happens.
    // We need to notify the owner
    KJ_IF_MAYBE(s, state) { s->consumerClose(); }
  }

  void onConsumerError(jsg::Lock& js, jsg::Value reason) override {
    // Called by the consumer when a state change to errored happens.
    // We need to noify the owner
    KJ_IF_MAYBE(s, state) { s->consumerError(js, kj::mv(reason)); }
  }

  void onConsumerWantsData(jsg::Lock& js) override {
    // Called by the consumer when it has a queued pending read and needs
    // data to be provided to fulfill it. We need to notify the controller
    // to initiate pulling to provide the data.
    KJ_IF_MAYBE(s, state) { s->consumerWantsData(js); }
  }

  kj::Maybe<int> getDesiredSize() {
    KJ_IF_MAYBE(s, state) { return s->getDesiredSize(); }
    return nullptr;
  }

  bool canCloseOrEnqueue() {
    return state.map([](State& state) { return state.canCloseOrEnqueue(); }).orDefault(false);
  }

  kj::Maybe<jsg::Ref<ReadableStreamDefaultController>> getControllerRef() {
    return state.map([](State& state) { return state.getControllerRef(); });
  }
};

struct ByteReadable final: public api::ByteQueue::ConsumerImpl::StateListener,
                           public kj::Refcounted {
  using State = ReadableState<ReadableByteStreamController, api::ByteQueue::Consumer>;
  kj::Maybe<State> state;
  int autoAllocateChunkSize;

  ByteReadable(
      jsg::Ref<ReadableByteStreamController> controller,
      auto owner,
      int autoAllocateChunkSize)
      : state(State(kj::mv(controller), owner, this)),
        autoAllocateChunkSize(autoAllocateChunkSize) {}

  ByteReadable(jsg::Lock& js, auto owner, ByteReadable& other)
      : state(KJ_ASSERT_NONNULL(other.state).cloneWithNewOwner(js, owner, this)),
        autoAllocateChunkSize(other.autoAllocateChunkSize) {}

  KJ_DISALLOW_COPY_AND_MOVE(ByteReadable);

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(state);
  }

  bool hasPendingReadRequests() {
    return state.map([](State& state) { return state.hasPendingReadRequests(); }).orDefault(false);
  }

  void setOwner(auto newOwner) {
    KJ_IF_MAYBE(s, state) { s->setOwner(kj::mv(newOwner)); }
  }

  kj::Own<ByteReadable> clone(jsg::Lock& js, ReadableStreamJsController* owner) {
    // A single ReadableByteStreamController can have multiple consumers.
    // When the ByteReadable constructor is used, the new consumer is added
    // and starts to receive new data that becomes enqueued. When clone
    // is used, any state currently held by this consumer is copied to the
    // new consumer.
    return kj::refcounted<ByteReadable>(js, owner, *this);
  }

  jsg::Promise<ReadResult> read(
      jsg::Lock& js,
      kj::Maybe<ReadableStreamController::ByobOptions> byobOptions) {
    KJ_IF_MAYBE(s, state) {
      // It's possible for the controller to be closed synchronously while the
      // read operation is executing. In that case, we want to make sure we keep
      // a reference so it'll survice at least long enough for the read method
      // to complete.
      auto self KJ_UNUSED = kj::addRef(*this);

      auto prp = js.newPromiseAndResolver<ReadResult>();

      KJ_IF_MAYBE(byob, byobOptions) {
        jsg::BufferSource source(js, byob->bufferView.getHandle(js));
        // If atLeast is not given, then by default it is the element size of the view
        // that we were given. If atLeast is given, we make sure that it is aligned
        // with the element size. No matter what, atLeast cannot be less than 1.
        auto atLeast = kj::max(source.getElementSize(), byob->atLeast.orDefault(1));
        atLeast = kj::max(1, atLeast - (atLeast % source.getElementSize()));
        s->consumer->read(js, ByteQueue::ReadRequest {
          .resolver = kj::mv(prp.resolver),
          .pullInto {
            .store = source.detach(js),
            .atLeast = atLeast,
            .type = ByteQueue::ReadRequest::Type::BYOB,
          },
        });
      } else {
        s->consumer->read(js, ByteQueue::ReadRequest {
          .resolver = kj::mv(prp.resolver),
          .pullInto {
            .store = jsg::BackingStore::alloc(js, autoAllocateChunkSize),
            .type = ByteQueue::ReadRequest::Type::BYOB,
          },
        });
      }

      return kj::mv(prp.promise);
    }

    // We are canceled! There's nothing else to do.
    KJ_IF_MAYBE(byob, byobOptions) {
      // If a BYOB buffer was given, we need to give it back wrapped in a TypedArray
      // whose size is set to zero.
      jsg::BufferSource source(js, byob->bufferView.getHandle(js));
      auto store = source.detach(js);
      store.consume(store.size());
      return js.resolvedPromise(ReadResult {
        .value = js.v8Ref(store.createHandle(js)),
        .done = true,
      });
    } else {
      return js.resolvedPromise(ReadResult { .done = true });
    }
  }

  jsg::Promise<void> cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    // When a ReadableStream is canceled, the expected behavior is that the underlying
    // controller is notified and the cancel algorithm on the underlying source is
    // called. When there are multiple ReadableStreams sharing consumption of a
    // controller, however, it should act as a shared pointer of sorts, canceling
    // the underlying controller only when the last reader is canceled.
    // Here, we rely on the controller implementing the correct behavior since it owns
    // the queue that knows about all of the attached consumers.
    KJ_IF_MAYBE(s, state) {
      KJ_DEFER({
        // Clear the references to the controller, free the consumer, and the
        // owner state once this scope exits. This ByteReadable will no longer
        // be usable once this is done.
        state = nullptr;
      });

      return s->cancel(js, kj::mv(maybeReason));
    }

    return js.resolvedPromise();
  }

  void onConsumerClose(jsg::Lock& js) override {
    KJ_IF_MAYBE(s, state) { s->consumerClose(); }
  }

  void onConsumerError(jsg::Lock& js, jsg::Value reason) override {
    KJ_IF_MAYBE(s, state) { s->consumerError(js, kj::mv(reason)); };
  }

  void onConsumerWantsData(jsg::Lock& js) override {
    // Called by the consumer when it has a queued pending read and needs
    // data to be provided to fulfill it. We need to notify the controller
    // to initiate pulling to provide the data.
    KJ_IF_MAYBE(s, state) { s->consumerWantsData(js); }
  }

  kj::Maybe<int> getDesiredSize() {
    KJ_IF_MAYBE(s, state) { return s->getDesiredSize(); }
    return nullptr;
  }

  bool canCloseOrEnqueue() {
    return state.map([](State& state) { return state.canCloseOrEnqueue(); }).orDefault(false);
  }

  kj::Maybe<jsg::Ref<ReadableByteStreamController>> getControllerRef() {
    return state.map([](State& state) { return state.getControllerRef(); });
  }
};

// =======================================================================================

ReadableStreamDefaultController::ReadableStreamDefaultController(
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy)
    : ioContext(tryGetIoContext()),
      impl(kj::mv(underlyingSource), kj::mv(queuingStrategy)),
      weakRef(kj::refcounted<WeakRef<ReadableStreamDefaultController>>(*this)) {}

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

bool ReadableStreamDefaultController::hasPendingReadRequests() {
  return impl.hasPendingReadRequests();
}

void ReadableStreamDefaultController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

jsg::Promise<void> ReadableStreamDefaultController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault([&] { return js.v8Undefined(); }));
}

void ReadableStreamDefaultController::close(jsg::Lock& js) {
  impl.close(js);
}

void ReadableStreamDefaultController::enqueue(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> chunk) {
  auto value = chunk.orDefault(js.v8Undefined());

  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError, "Unable to enqueue");

  size_t size = 1;
  bool errored = false;
  KJ_IF_MAYBE(sizeFunc, impl.algorithms.size) {
    js.tryCatch([&] {
      size = (*sizeFunc)(js, value);
    }, [&](jsg::Value exception) {
      impl.doError(js, kj::mv(exception));
      errored = true;
    });
  }

  if (!errored) {
    impl.enqueue(js, kj::refcounted<ValueQueue::Entry>(js.v8Ref(value), size), JSG_THIS);
  }
}

void ReadableStreamDefaultController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  impl.doError(js, js.v8Ref(reason));
}

void ReadableStreamDefaultController::pull(jsg::Lock& js) {
  // When a consumer receives a read request, but does not have the data available to
  // fulfill the request, the consumer will call pull on the controller to pull that
  // data if needed.
  impl.pullIfNeeded(js, JSG_THIS);
}

kj::Own<ValueQueue::Consumer> ReadableStreamDefaultController::getConsumer(
    kj::Maybe<ValueQueue::ConsumerImpl::StateListener&> stateListener) {
  return impl.getConsumer(stateListener);
}

// ======================================================================================

void ReadableStreamBYOBRequest::Impl::updateView(jsg::Lock& js) {
  jsg::check(view.getHandle(js)->Buffer()->Detach(v8::Local<v8::Value>()));
  view = js.v8Ref(readRequest->getView(js));
}

void ReadableStreamBYOBRequest::visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(impl, maybeImpl) {
      visitor.visit(impl->view, impl->controller);
    }
  }

ReadableStreamBYOBRequest::ReadableStreamBYOBRequest(
    jsg::Lock& js,
    kj::Own<ByteQueue::ByobRequest> readRequest,
    jsg::Ref<ReadableByteStreamController> controller)
    : ioContext(tryGetIoContext()),
      maybeImpl(Impl(js, kj::mv(readRequest), kj::mv(controller))) {}

kj::Maybe<int> ReadableStreamBYOBRequest::getAtLeast() {
  KJ_IF_MAYBE(impl, maybeImpl) {
    return impl->readRequest->getAtLeast();
  }
  return nullptr;
}

kj::Maybe<jsg::V8Ref<v8::Uint8Array>> ReadableStreamBYOBRequest::getView(jsg::Lock& js) {
  KJ_IF_MAYBE(impl, maybeImpl) {
    return impl->view.addRef(js);
  }
  return nullptr;
}

void ReadableStreamBYOBRequest::invalidate(jsg::Lock& js) {
  KJ_IF_MAYBE(impl, maybeImpl) {
    // If the user code happened to have retained a reference to the view or
    // the buffer, we need to detach it so that those references cannot be used
    // to modify or observe modifications.
    jsg::check(impl->view.getHandle(js)->Buffer()->Detach(v8::Local<v8::Value>()));
    impl->controller->maybeByobRequest = nullptr;
  }
  maybeImpl = nullptr;
}

void ReadableStreamBYOBRequest::respond(jsg::Lock& js, int bytesWritten) {
  auto& impl = JSG_REQUIRE_NONNULL(maybeImpl,
                                   TypeError,
                                   "This ReadableStreamBYOBRequest has been invalidated.");
  JSG_REQUIRE(impl.view.getHandle(js)->ByteLength() > 0, TypeError,
      "Cannot respond with a zero-length or detached view");
  if (!impl.controller->canCloseOrEnqueue()) {
    JSG_REQUIRE(bytesWritten == 0,
                 TypeError,
                 "The bytesWritten must be zero after the stream is closed.");
    KJ_ASSERT(impl.readRequest->isInvalidated());
    invalidate(js);
  } else {
    bool shouldInvalidate = false;
    if (impl.readRequest->isInvalidated() && impl.controller->impl.consumerCount() >= 1) {
      // While this particular request may be invalidated, there are still
      // other branches we can push the data to. Let's do so.
      jsg::BufferSource source(js, impl.view.getHandle(js));
      auto entry = kj::refcounted<ByteQueue::Entry>(source.detach(js));
      impl.controller->impl.enqueue(js, kj::mv(entry), impl.controller.addRef());
    } else {
      JSG_REQUIRE(bytesWritten > 0,
                  TypeError,
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
  auto& impl = JSG_REQUIRE_NONNULL(maybeImpl,
                                    TypeError,
                                    "This ReadableStreamBYOBRequest has been invalidated.");
  if (!impl.controller->canCloseOrEnqueue()) {
    JSG_REQUIRE(view.size() == 0,
                 TypeError,
                 "The view byte length must be zero after the stream is closed.");
    KJ_ASSERT(impl.readRequest->isInvalidated());
    invalidate(js);
  } else {
    bool shouldInvalidate = false;
    if (impl.readRequest->isInvalidated() && impl.controller->impl.consumerCount() >= 1) {
      // While this particular request may be invalidated, there are still
      // other branches we can push the data to. Let's do so.
      auto entry = kj::refcounted<ByteQueue::Entry>(view.detach(js));
      impl.controller->impl.enqueue(js, kj::mv(entry), impl.controller.addRef());
    } else {
      JSG_REQUIRE(view.size() > 0,
                  TypeError,
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
  KJ_IF_MAYBE(impl, maybeImpl) {
    return impl->readRequest->isPartiallyFulfilled();
  }
  return false;
}

// ======================================================================================

ReadableByteStreamController::ReadableByteStreamController(
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy)
    : ioContext(tryGetIoContext()),
      impl(kj::mv(underlyingSource),
      kj::mv(queuingStrategy)) {}

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

bool ReadableByteStreamController::hasPendingReadRequests() {
  return impl.hasPendingReadRequests();
}

void ReadableByteStreamController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(maybeByobRequest, impl);
}

jsg::Promise<void> ReadableByteStreamController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  KJ_IF_MAYBE(byobRequest, maybeByobRequest) {
    if (impl.consumerCount() == 1) {
      (*byobRequest)->invalidate(js);
    }
  }
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault(js.v8Undefined()));
}

void ReadableByteStreamController::close(jsg::Lock& js) {
  KJ_IF_MAYBE(byobRequest, maybeByobRequest) {
    JSG_REQUIRE(!(*byobRequest)->isPartiallyFulfilled(), TypeError,
        "This ReadableStream was closed with a partial read pending.");
  }
  impl.close(js);
}

void ReadableByteStreamController::enqueue(jsg::Lock& js, jsg::BufferSource chunk) {
  JSG_REQUIRE(chunk.size() > 0, TypeError, "Cannot enqueue a zero-length ArrayBuffer.");
  JSG_REQUIRE(chunk.canDetach(js), TypeError,
               "The provided ArrayBuffer must be detachable.");
  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError, "This ReadableByteStreamController is closed.");

  KJ_IF_MAYBE(byobRequest, maybeByobRequest) {
    KJ_IF_MAYBE(view, (*byobRequest)->getView(js)) {
      JSG_REQUIRE(view->getHandle(js)->ByteLength() > 0, TypeError,
          "The byobRequest.view is zero-length or was detached");
    }
    (*byobRequest)->invalidate(js);
  }

  impl.enqueue(js, kj::refcounted<ByteQueue::Entry>(chunk.detach(js)), JSG_THIS);
}

void ReadableByteStreamController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  impl.doError(js, js.v8Ref(reason));
}

kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>>
ReadableByteStreamController::getByobRequest(jsg::Lock& js) {
  if (maybeByobRequest == nullptr) {
    KJ_IF_MAYBE(queue, impl.state.tryGet<ByteQueue>()) {
      KJ_IF_MAYBE(pendingByob, queue->nextPendingByobReadRequest()) {
        maybeByobRequest = jsg::alloc<ReadableStreamBYOBRequest>(js,
            kj::mv(*pendingByob), JSG_THIS);
      }
    } else {
      return nullptr;
    }
  }

  return maybeByobRequest.map([&](jsg::Ref<ReadableStreamBYOBRequest>& req) {
    return req.addRef();
  });
}

void ReadableByteStreamController::pull(jsg::Lock& js) {
  // When a consumer receives a read request, but does not have the data available to
  // fulfill the request, the consumer will call pull on the controller to pull that
  // data if needed.
  impl.pullIfNeeded(js, JSG_THIS);
}

kj::Own<ByteQueue::Consumer> ReadableByteStreamController::getConsumer(
    kj::Maybe<ByteQueue::ConsumerImpl::StateListener&> stateListener) {
  return impl.getConsumer(stateListener);
}

// ======================================================================================

ReadableStreamJsController::ReadableStreamJsController() : ioContext(tryGetIoContext()) {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Closed closed)
    : ioContext(tryGetIoContext()), state(closed) {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Errored errored)
    : ioContext(tryGetIoContext()), state(kj::mv(errored)) {}

ReadableStreamJsController::ReadableStreamJsController(
    jsg::Lock& js,
    ValueReadable& consumer)
    : ioContext(tryGetIoContext()), state(consumer.clone(js, this)) {}

ReadableStreamJsController::ReadableStreamJsController(
    jsg::Lock& js,
    ByteReadable& consumer)
    : ioContext(tryGetIoContext()), state(consumer.clone(js, this)) {}

ReadableStreamJsController::ReadableStreamJsController(kj::Own<ValueReadable> consumer)
    : ioContext(tryGetIoContext()), state(kj::mv(consumer)) {
  state.get<kj::Own<ValueReadable>>()->setOwner(this);
}

ReadableStreamJsController::ReadableStreamJsController(kj::Own<ByteReadable> consumer)
    : ioContext(tryGetIoContext()), state(kj::mv(consumer)) {
  state.get<kj::Own<ByteReadable>>()->setOwner(this);
}

jsg::Ref<ReadableStream> ReadableStreamJsController::addRef() {
  return KJ_REQUIRE_NONNULL(owner).addRef();
}

jsg::Promise<void> ReadableStreamJsController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  disturbed = true;

  const auto doCancel = [&](auto& consumer) {
    auto reason = js.v8Ref(maybeReason.orDefault([&] { return js.v8Undefined(); }));
    KJ_DEFER(state.init<StreamStates::Closed>());
    return consumer->cancel(js, reason.getHandle(js));
  };

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

void ReadableStreamJsController::doClose() {
  // Finalizes the closed state of this ReadableStream. The connection to the underlying
  // controller is released with no further action. Importantly, this method is triggered
  // by the underlying controller as a result of that controller closing or being canceled.
  // We detach ourselves from the underlying controller by releasing the ValueReadable or
  // ByteReadable in the state and changing that to closed.
  // We also clean up other state here.
  state.init<StreamStates::Closed>();
  lock.onClose();
}

void ReadableStreamJsController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  // As with doClose(), doError() finalizes the error state of this ReadableStream.
  // The connection to the underlying controller is released with no further action.
  // This method is triggered by the underlying controller as a result of that controller
  // erroring. We detach ourselves from the underlying controller by releasing the ValueReadable
  // or ByteReadable in the state and changing that to errored.
  // We also clean up other state here.
  state.init<StreamStates::Errored>(js.v8Ref(reason));
  lock.onError(js, reason);
}

bool ReadableStreamJsController::hasPendingReadRequests() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return false; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return false; }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      return consumer->hasPendingReadRequests();
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return consumer->hasPendingReadRequests();
    }
  }
  KJ_UNREACHABLE;
}

bool ReadableStreamJsController::isByteOriented() const {
  return state.is<kj::Own<ByteReadable>>();
}

bool ReadableStreamJsController::isClosedOrErrored() const {
  return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
}

bool ReadableStreamJsController::isDisturbed() { return disturbed; }

bool ReadableStreamJsController::isLockedToReader() const {
  return lock.isLockedToReader();
}

bool ReadableStreamJsController::lockReader(jsg::Lock& js, Reader& reader) {
  return lock.lockReader(js, *this, reader);
}

jsg::Promise<void> ReadableStreamJsController::pipeTo(
    jsg::Lock& js,
    WritableStreamController& destination,
    PipeToOptions options) {
  KJ_DASSERT(!isLockedToReader());
  KJ_DASSERT(!destination.isLockedToWriter());

  disturbed = true;
  KJ_IF_MAYBE(promise, destination.tryPipeFrom(js, addRef(), kj::mv(options))) {
    return kj::mv(*promise);
  }

  return js.rejectedPromise<void>(
      js.v8TypeError("This ReadableStream cannot be piped to this WritableStream"_kj));
}

kj::Maybe<jsg::Promise<ReadResult>> ReadableStreamJsController::read(
    jsg::Lock& js,
    kj::Maybe<ByobOptions> maybeByobOptions) {
  disturbed = true;

  KJ_IF_MAYBE(byobOptions, maybeByobOptions) {
    byobOptions->detachBuffer = true;
    auto view = byobOptions->bufferView.getHandle(js);
    if (!view->Buffer()->IsDetachable()) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("Unabled to use non-detachable ArrayBuffer."_kj));
    }

    if (view->ByteLength() == 0 || view->Buffer()->ByteLength() == 0) {
      return js.rejectedPromise<ReadResult>(
          js.v8TypeError("Unable to use a zero-length ArrayBuffer."_kj));
    }

    if (state.is<StreamStates::Closed>()) {
      // If it is a BYOB read, then the spec requires that we return an empty
      // view of the same type provided, that uses the same backing memory
      // as that provided, but with zero-length.
      auto source = jsg::BufferSource(js, byobOptions->bufferView.getHandle(js));
      auto store = source.detach(js);
      store.consume(store.size());
      return js.resolvedPromise(ReadResult {
        .value = js.v8Ref(store.createHandle(js)),
        .done = true,
      });
    }
  }

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      // The closed state for BYOB reads is handled in the maybeByobOptions check above.
      KJ_ASSERT(maybeByobOptions == nullptr);
      return js.resolvedPromise(ReadResult { .done = true });
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<ReadResult>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      // The ReadableStreamDefaultController does not support ByobOptions.
      // It should never happen, but let's make sure.
      KJ_ASSERT(maybeByobOptions == nullptr);
      return consumer->read(js);
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      return consumer->read(js, kj::mv(maybeByobOptions));
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::releaseReader(
    Reader& reader,
    kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseReader(*this, reader, maybeJs);
}

ReadableStreamController::Tee ReadableStreamJsController::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLockedToReader(), TypeError, "This ReadableStream is locked to a reader.");
  lock.state.init<Locked>();
  disturbed = true;

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(
            kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
        .branch2 = jsg::alloc<ReadableStream>(
            kj::heap<ReadableStreamJsController>(StreamStates::Closed())),
      };
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(
            errored.addRef(js))),
        .branch2 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(
            errored.addRef(js))),
      };
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ValueReadable>) {
      KJ_DEFER(state.init<StreamStates::Closed>());
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
        .branch2 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(
            kj::mv(consumer))),
      };
    }
    KJ_CASE_ONEOF(consumer, kj::Own<ByteReadable>) {
      KJ_DEFER(state.init<StreamStates::Closed>());
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(js, *consumer)),
        .branch2 = jsg::alloc<ReadableStream>(kj::heap<ReadableStreamJsController>(
            kj::mv(consumer))),
      };
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::setOwnerRef(ReadableStream& stream) {
  KJ_ASSERT(owner == nullptr);
  owner = &stream;
}

void ReadableStreamJsController::setup(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSource> maybeUnderlyingSource,
    jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {
  auto underlyingSource = kj::mv(maybeUnderlyingSource).orDefault({});
  auto queuingStrategy = kj::mv(maybeQueuingStrategy).orDefault({});
  auto type = underlyingSource.type.map([](kj::StringPtr s) { return s; }).orDefault(""_kj);

  if (type == "bytes") {
    auto autoAllocateChunkSize = underlyingSource.autoAllocateChunkSize.orDefault(
        UnderlyingSource::DEFAULT_AUTO_ALLOCATE_CHUNK_SIZE);

    auto controller = jsg::alloc<ReadableByteStreamController>(
        kj::mv(underlyingSource),
        kj::mv(queuingStrategy));

    JSG_REQUIRE(autoAllocateChunkSize > 0,
                TypeError,
                "The autoAllocateChunkSize option cannot be zero.");

    state = kj::refcounted<ByteReadable>(controller.addRef(), this, autoAllocateChunkSize);
    controller->start(js);
  } else {
    JSG_REQUIRE(type == "", TypeError,
        kj::str("\"", type, "\" is not a valid type of ReadableStream."));
    auto controller = jsg::alloc<ReadableStreamDefaultController>(
        kj::mv(underlyingSource),
        kj::mv(queuingStrategy));
    state = kj::refcounted<ValueReadable>(controller.addRef(), this);
    controller->start(js);
  }
}

kj::Maybe<ReadableStreamController::PipeController&> ReadableStreamJsController::tryPipeLock(
    jsg::Ref<WritableStream> destination) {
  return lock.tryPipeLock(*this, kj::mv(destination));
}

void ReadableStreamJsController::visitForGc(jsg::GcVisitor& visitor) {
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
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return nullptr; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return nullptr; }
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
  return state.tryGet<StreamStates::Errored>().map([&](jsg::Value& reason) {
    return reason.getHandle(js);
  });
}

bool ReadableStreamJsController::canCloseOrEnqueue() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return false; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return false; }
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
  KJ_IF_MAYBE(size, getDesiredSize()) { return *size <= 0; }
  return false;
}

kj::Maybe<kj::OneOf<jsg::Ref<ReadableStreamDefaultController>,
                    jsg::Ref<ReadableByteStreamController>>>
ReadableStreamJsController::getController() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return nullptr; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return nullptr; }
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
template <typename T>
class AllReader final: public AllReaderBase {
  // Consumes all bytes from a stream, buffering in memory, with the purpose
  // of producing either a single concatenated kj::Array<byte> or kj::String.
public:
  using Readable = kj::Own<T>;
  using PartList = kj::Array<kj::ArrayPtr<byte>>;

  AllReader(Readable readable, uint64_t limit)
      : ioContext(tryGetIoContext()),
        state(kj::mv(readable)),
        limit(limit) {
    state.template get<Readable>()->setOwner(addWeakRef());
  }

  void doClose() override {
    state.template init<StreamStates::Closed>();
  }

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason) override {
    state.template init<StreamStates::Errored>(js.v8Ref(reason));
  }

  jsg::Promise<kj::Array<byte>> allBytes(jsg::Lock& js) {
    auto after = [this](auto& js, PartList&& partPtrs) {
      auto out = kj::heapArray<byte>(runningTotal);
      copyInto(out, kj::mv(partPtrs));
      return kj::mv(out);
    };
    auto promise = loop(js);
    if (IoContext::hasCurrent()) {
      return promise.then(js, IoContext::current().addFunctor(kj::mv(after)));
    } else {
      return promise.then(js, kj::mv(after));
    }
  }

  jsg::Promise<kj::String> allText(jsg::Lock& js) {
    auto after = [this](auto& js, PartList&& partPtrs) {
      auto out = kj::heapArray<char>(runningTotal + 1);
      copyInto(out.slice(0, out.size() - 1).asBytes(), kj::mv(partPtrs));
      out.back() = '\0';
      return kj::String(kj::mv(out));
    };
    auto promise = loop(js);
    if (IoContext::hasCurrent()) {
      return promise.then(js, IoContext::current().addFunctor(kj::mv(after)));
    } else {
      return promise.then(js, kj::mv(after));
    }
  }

private:
  kj::Maybe<IoContext&> ioContext;
  kj::OneOf<StreamStates::Closed, StreamStates::Errored, Readable> state;
  uint64_t limit;
  kj::Vector<jsg::BackingStore> parts;
  uint64_t runningTotal = 0;

  jsg::Promise<PartList> loop(jsg::Lock& js) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return js.resolvedPromise(KJ_MAP(p, parts) { return p.asArrayPtr(); });
      }
      KJ_CASE_ONEOF(errored, StreamStates::Errored) {
        return js.template rejectedPromise<PartList>(errored.getHandle(js));
      }
      KJ_CASE_ONEOF(readable, Readable) {
        const auto read = [&](auto& js) {
          if constexpr (kj::isSameType<T, ByteReadable>()) {
            return readable->read(js, nullptr);
          } else {
            return readable->read(js);
          }
        };

        auto onSuccess = [this](auto& js, ReadResult result) {
          if (result.done) {
            doClose();
            return loop(js);
          }

          // If we're not done, the result value must be interpretable as
          // bytes for the read to make any sense.
          auto handle = KJ_ASSERT_NONNULL(result.value).getHandle(js);
          if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
            auto error = js.v8TypeError("This ReadableStream did not return bytes.");
            doError(js, error);
            return loop(js);
          }

          jsg::BufferSource bufferSource(js, handle);
          jsg::BackingStore backing = bufferSource.detach(js);

          if (backing.size() == 0) {
            // Weird but allowed, we'll skip it.
            return loop(js);
          }

          if ((runningTotal + backing.size()) > limit) {
            auto error = js.v8TypeError("Memory limit exceeded before EOF.");
            doError(js, error);
            return loop(js);
          }

          runningTotal += backing.size();

          parts.add(kj::mv(backing));
          return loop(js);
        };

        auto onFailure = [this](auto& js, jsg::Value exception) {
          doError(js, exception.getHandle(js));
          return loop(js);
        };

        return maybeAddFunctor(js, read(js), kj::mv(onSuccess), kj::mv(onFailure));
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

template <typename T>
class PumpToReader final: public AllReaderBase {
public:
  using Readable = kj::Own<T>;

  KJ_DISALLOW_COPY_AND_MOVE(PumpToReader);

  PumpToReader(Readable readable, kj::Own<WritableStreamSink> sink, bool end)
    : ioContext(IoContext::current()),
      state(kj::mv(readable)),
      sink(kj::mv(sink)),
      end(end) {
    state.template get<Readable>()->setOwner(addWeakRef());
  }

  void doClose() override {
    if (!isErroredOrClosed()) {
      state.template init<StreamStates::Closed>();
    }
  }

  void doError(jsg::Lock& js, v8::Local<v8::Value> reason) override {
    if (!isErroredOrClosed()) {
      state.template init<kj::Exception>(js.exceptionToKj(js.v8Ref(reason)));
    }
  }

  kj::Promise<void> pumpTo(jsg::Lock& js) {
    ioContext.requireCurrentOrThrowJs();
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ready, Readable) {
        auto readable = kj::mv(ready);
        state.template init<Pumping>();
        // Ownership of readable passes into the pump loop...
        // Ownership of the sink remains with the PumpToReader...
        // The JS Promise loop uses an IoOwn wrapping a weak ref to the PumpToReader...
        // The ownership of everything here is a bit complicated. Effectiely, we have
        // a kj::Promise wrapping a JS Promise that is essentially a loop of JS read
        // promises followed by kj write promises. If the outer kj Promise is dropped,
        // the PumpToReader attached to it is dropped. When that happens, there's a
        // chance the JS continuation will still be scheduled to run. The IoOwn ensures
        // that the PumpToReader, and the sink it owns, are always accessed from the right
        // IoContext. Thw WeakRef ensures that if the PumpToReader is freed while
        // the JS continuation is pending, there won't be a dangling reference.
        return ioContext.awaitJs(
            pumpLoop(js, ioContext, kj::mv(readable), ioContext.addObject(addWeakRef())));
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
  kj::OneOf<Readable, Pumping, StreamStates::Closed, kj::Exception> state;
  kj::Own<WritableStreamSink> sink;
  bool end;

  bool isErroredOrClosed() {
    return state.template is<kj::Exception>() ||
           state.template is<StreamStates::Closed>();
  }

  jsg::Promise<void> pumpLoop(
      jsg::Lock& js,
      IoContext& ioContext,
      Readable readable,
      IoOwn<WeakRef<AllReaderBase>> pumpToReader) {
    ioContext.requireCurrentOrThrowJs();
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(ready, Readable) {
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {
        return end ?
            ioContext.awaitIoLegacy(sink->end().attach(kj::mv(sink))) :
            js.resolvedPromise();
      }
      KJ_CASE_ONEOF(errored, kj::Exception) {
        if (end) {
          sink->abort(kj::cp(errored));
        }
        return js.rejectedPromise<void>(js.exceptionToJs(kj::cp(errored)));
      }
      KJ_CASE_ONEOF(pumping, Pumping) {
        const auto read = [&](auto& js) {
          if constexpr (kj::isSameType<T, ByteReadable>()) {
            return readable->read(js, nullptr);
          } else {
            return readable->read(js);
          }
        };

        using Result = kj::OneOf<Pumping,              // Continue with next read.
                                 kj::Array<kj::byte>,  // Bytes to write were returned.
                                 StreamStates::Closed, // Readable indicated done.
                                 jsg::Value>;          // There was an error.

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

        return read(js).then(js, ioContext.addFunctor(
            [](auto& js, ReadResult result) mutable -> Result {
          if (result.done) {
            // Indicate to the outer promise that the readable is done.
            // There's nothing further to do.
            return StreamStates::Closed ();
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
            return Pumping {};
          }

          if constexpr (kj::isSameType<T, ByteReadable>()) {
            jsg::BackingStore backing = bufferSource.detach(js);
            return backing.asArrayPtr().attach(kj::mv(backing));
          } else if constexpr (kj::isSameType<T, ValueReadable>()) {
            // We do not detach in this case because, as bad as an idea as it is,
            // the stream spec does allow a single typedarray/arraybuffer instance
            // to be queued multiple times when using value-oriented streams.
            return bufferSource.asArrayPtr().attach(kj::mv(bufferSource));
          }

          KJ_UNREACHABLE;
        }), [](auto& js, jsg::Value exception) mutable -> Result {
          return kj::mv(exception);
        }).then(js, ioContext.addFunctor(
            [readable=kj::mv(readable),pumpToReader=kj::mv(pumpToReader)]
            (jsg::Lock& js, Result result) mutable {
          KJ_IF_MAYBE(reader, tryGetAs<PumpToReader>(pumpToReader)) {
            // Oh good, if we got here it means we're in the right IoContext and
            // the PumpToReader is still alive. Let's process the result.
            reader->ioContext.requireCurrentOrThrowJs();
            auto& ioContext = IoContext::current();
            KJ_SWITCH_ONEOF(result) {
              KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
                // We received bytes to write. Do so...
                // (It's safe to directly access reader->sink here --it's an kj::Own--
                // because we accessed the reader through an IoOwn, proving that
                // we're in the correct IoContext...)
                auto promise = reader->sink->write(bytes.begin(), bytes.size())
                    .attach(kj::mv(bytes));
                return ioContext.awaitIo(js, kj::mv(promise),
                    [](jsg::Lock& js) -> kj::Maybe<jsg::Value> {
                  // The write completed successfully.
                  return kj::Maybe<jsg::Value>(nullptr);
                }, [](jsg::Lock& js, jsg::Value exception) mutable -> kj::Maybe<jsg::Value> {
                  // The write failed.
                  return kj::mv(exception);
                }).then(js, ioContext.addFunctor(
                    [readable=kj::mv(readable),pumpToReader=kj::mv(pumpToReader)]
                    (jsg::Lock& js, kj::Maybe<jsg::Value> maybeException) mutable {
                  KJ_IF_MAYBE(reader, tryGetAs<PumpToReader>(pumpToReader)) {
                    auto& ioContext = reader->ioContext;
                    ioContext.requireCurrentOrThrowJs();
                    // Oh good, if we got here it means we're in the right IoContext and
                    // the PumpToReader is still alive.
                    KJ_IF_MAYBE(exception, maybeException) {
                      reader->doError(js, exception->getHandle(js));
                    }
                    return reader->pumpLoop(js, ioContext, kj::mv(readable), kj::mv(pumpToReader));
                  } else {
                    // If we got here, we're in the right IoContext but the PumpToReader
                    // has been destroyed. Let's cancel the readable as the last step.
                    return readable->cancel(js, maybeException.map([&](jsg::Value& ex) {
                      return ex.getHandle(js);
                    }));
                  }
                }));
              }
              KJ_CASE_ONEOF(pumping, Pumping) {
                // If we got here, a zero-length buffer was provided by the read and we're
                // just going to ignore it and keep going.
              }
              KJ_CASE_ONEOF(closed, StreamStates::Closed) {
                // If we got here, the read signaled that we're done. Close the reader and
                // pump one more time to shut things down.
                reader->doClose();
              }
              KJ_CASE_ONEOF(exception, jsg::Value) {
                // If we got here, the read signaled an exception. Either the read failed or
                // provided something other than bytes. Error the reader and pump one more
                // time to shut things down.
                reader->doError(js, exception.getHandle(js));
              }
            }
            return reader->pumpLoop(js, ioContext, kj::mv(readable), kj::mv(pumpToReader));
          } else {
            // If we got here, we're in the right IoContext but the PumpToReader has been
            // freed. There's nothing we can do except cleanup.
            KJ_SWITCH_ONEOF(result) {
              KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
                return readable->cancel(js, nullptr);
              }
              KJ_CASE_ONEOF(pumping, Pumping) {
                return readable->cancel(js, nullptr);
              }
              KJ_CASE_ONEOF(closed, StreamStates::Closed) {
                // We do not have to cancel the readable in this case because it has already
                // signaled that it is done. There's nothing to cancel.
                return js.resolvedPromise();
              }
              KJ_CASE_ONEOF(exception, jsg::Value) {
                return readable->cancel(js, exception.getHandle(js));
              }
            }
          }
          KJ_UNREACHABLE;
        }));
      }
    }
    KJ_UNREACHABLE;
  }

};
}  // namespace

jsg::Promise<kj::Array<byte>> ReadableStreamJsController::readAllBytes(
    jsg::Lock& js,
    uint64_t limit) {
  if (isLockedToReader()) {
    return js.rejectedPromise<kj::Array<byte>>(KJ_EXCEPTION(FAILED,
        "jsg.TypeError: This ReadableStream is currently locked to a reader."));
  }
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise(kj::Array<byte>());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<kj::Array<byte>>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<AllReader<ValueReadable>>(kj::mv(valueReadable), limit);
      doClose();
      if (IoContext::hasCurrent()) {
        return reader->allBytes(js).then(js, IoContext::current().addFunctor(
            [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        }));
      } else {
        return reader->allBytes(js).then(js, [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        });
      }
    }
    KJ_CASE_ONEOF(byteReadable, kj::Own<ByteReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<AllReader<ByteReadable>>(kj::mv(byteReadable), limit);
      doClose();
      if (IoContext::hasCurrent()) {
        return reader->allBytes(js).then(js, IoContext::current().addFunctor(
            [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        }));
      } else {
        return reader->allBytes(js).then(js, [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<kj::String> ReadableStreamJsController::readAllText(
    jsg::Lock& js,
    uint64_t limit) {
  if (isLockedToReader()) {
    return js.rejectedPromise<kj::String>(KJ_EXCEPTION(FAILED,
        "jsg.TypeError: This ReadableStream is currently locked to a reader."));
  }
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise(kj::String());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<kj::String>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<AllReader<ValueReadable>>(kj::mv(valueReadable), limit);
      doClose();
      if (IoContext::hasCurrent()) {
        return reader->allText(js).then(js, IoContext::current().addFunctor(
            [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        }));
      } else {
        return reader->allText(js).then(js, [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        });
      }
    }
    KJ_CASE_ONEOF(byteReadable, kj::Own<ByteReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<AllReader<ByteReadable>>(kj::mv(byteReadable), limit);
      doClose();
      if (IoContext::hasCurrent()) {
        return reader->allText(js).then(js, IoContext::current().addFunctor(
            [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        }));
      } else {
        return reader->allText(js).then(js, [reader=kj::mv(reader)](auto& js, auto result) {
          return kj::mv(result);
        });
      }
    }
  }
  KJ_UNREACHABLE;
}

kj::Own<ReadableStreamJsController> ReadableStreamJsController::detach(jsg::Lock& js) {
  KJ_ASSERT(!isLockedToReader());
  KJ_ASSERT(!isDisturbed());
  auto controller = kj::heap<ReadableStreamJsController>();

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      controller->state.init<StreamStates::Closed>();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      controller->state.init<StreamStates::Errored>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, kj::Own<ValueReadable>) {
      lock.lock();
      disturbed = true;
      controller->state = kj::mv(readable);
      doClose();
    }
    KJ_CASE_ONEOF(readable, kj::Own<ByteReadable>) {
      lock.lock();
      disturbed = true;
      controller->state = kj::mv(readable);
      doClose();
    }
  }

  return kj::mv(controller);
}

kj::Maybe<uint64_t> ReadableStreamJsController::tryGetLength(StreamEncoding encoding) {
  return nullptr;
}

kj::Promise<DeferredProxy<void>> ReadableStreamJsController::pumpTo(
    jsg::Lock& js,
    kj::Own<WritableStreamSink> sink,
    bool end) {
  KJ_ASSERT(IoContext::hasCurrent(), "Unable to consume this ReadableStream outside of a request");
  KJ_REQUIRE(!isLockedToReader(), "This ReadableStream is currently locked to a reader.");
  disturbed = true;

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return addNoopDeferredProxy(sink->end().attach(kj::mv(sink)));
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.exceptionToKj(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, kj::Own<ValueReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<PumpToReader<ValueReadable>>(kj::mv(readable), kj::mv(sink), end);
      doClose();
      return addNoopDeferredProxy(reader->pumpTo(js).attach(kj::mv(reader)));
    }
    KJ_CASE_ONEOF(readable, kj::Own<ByteReadable>) {
      KJ_ASSERT(lock.lock());
      auto reader = kj::heap<PumpToReader<ByteReadable>>(kj::mv(readable), kj::mv(sink), end);
      doClose();
      return addNoopDeferredProxy(reader->pumpTo(js).attach(kj::mv(reader)));
    }
  }

  KJ_UNREACHABLE;
}

// ======================================================================================

WritableStreamDefaultController::WritableStreamDefaultController(WriterOwner& owner)
    : ioContext(tryGetIoContext()), impl(owner) {}

jsg::Promise<void> WritableStreamDefaultController::abort(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  return impl.abort(js, JSG_THIS, reason);
}

jsg::Promise<void> WritableStreamDefaultController::close(jsg::Lock& js) {
  return impl.close(js, JSG_THIS);
}

void WritableStreamDefaultController::error(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  impl.error(js, JSG_THIS, reason.orDefault(js.v8Undefined()));
}

ssize_t WritableStreamDefaultController::getDesiredSize() {
  return impl.getDesiredSize();
}

jsg::Ref<AbortSignal> WritableStreamDefaultController::getSignal() {
  return impl.signal.addRef();
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamDefaultController::isErroring(jsg::Lock& js) {
  KJ_IF_MAYBE(erroring, impl.state.tryGet<StreamStates::Erroring>()) {
    return erroring->reason.getHandle(js);
  }
  return nullptr;
}

void WritableStreamDefaultController::setOwner(kj::Maybe<WriterOwner&> owner) {
  impl.setOwner(owner);
}

void WritableStreamDefaultController::setup(
    jsg::Lock& js,
    UnderlyingSink underlyingSink,
    StreamQueuingStrategy queuingStrategy) {
  impl.setup(js, JSG_THIS, kj::mv(underlyingSink), kj::mv(queuingStrategy));
}

jsg::Promise<void> WritableStreamDefaultController::write(
    jsg::Lock& js,
    v8::Local<v8::Value> value) {
  return impl.write(js, JSG_THIS, value);
}

// ======================================================================================
WritableStreamJsController::WritableStreamJsController()
    : ioContext(tryGetIoContext()),
      weakRef(kj::refcounted<WeakRef<WritableStreamJsController>>(*this)) {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Closed closed)
    : ioContext(tryGetIoContext()), state(closed),
      weakRef(kj::refcounted<WeakRef<WritableStreamJsController>>(*this)) {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Errored errored)
    : ioContext(tryGetIoContext()), state(kj::mv(errored)),
      weakRef(kj::refcounted<WeakRef<WritableStreamJsController>>(*this)) {}

jsg::Promise<void> WritableStreamJsController::abort(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  // The spec requires that if abort is called multiple times, it is supposed to return the same
  // promise each time. That's a bit cumbersome here with jsg::Promise so we intentionally just
  // return a continuation branch off the same promise.
  KJ_IF_MAYBE(abortPromise, maybeAbortPromise) {
    return abortPromise->whenResolved();
  }
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      maybeAbortPromise = js.resolvedPromise();
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      // Per the spec, if the stream is errored, we are to return a resolved promise.
      maybeAbortPromise = js.resolvedPromise();
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved();
    }
    KJ_CASE_ONEOF(controller, Controller) {
      maybeAbortPromise = controller->abort(js, reason.orDefault(js.v8Undefined()));
      return KJ_ASSERT_NONNULL(maybeAbortPromise).whenResolved();
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<WritableStream> WritableStreamJsController::addRef() {
  return KJ_ASSERT_NONNULL(owner).addRef();
}

jsg::Promise<void> WritableStreamJsController::close(jsg::Lock& js, bool markAsHandled) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return rejectedMaybeHandledPromise<void>(
          js,
          js.v8TypeError("This WritableStream has been closed."_kj),
          markAsHandled);
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

void WritableStreamJsController::doClose() {
  KJ_IF_MAYBE(controller, state.tryGet<Controller>()) {
    (*controller)->setOwner(nullptr);
  }
  state.init<StreamStates::Closed>();
  KJ_IF_MAYBE(locked, lock.state.tryGet<WriterLocked>()) {
    maybeResolvePromise(locked->getClosedFulfiller());
    maybeResolvePromise(locked->getReadyFulfiller());
  } else KJ_IF_MAYBE(locked, lock.state.tryGet<WritableLockImpl::PipeLocked>()) {
    lock.state.init<Unlocked>();
  }
}

void WritableStreamJsController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_MAYBE(controller, state.tryGet<Controller>()) {
    (*controller)->setOwner(nullptr);
  }
  state.init<StreamStates::Errored>(js.v8Ref(reason));
  KJ_IF_MAYBE(locked, lock.state.tryGet<WriterLocked>()) {
    maybeRejectPromise<void>(locked->getClosedFulfiller(), reason);
    maybeResolvePromise(locked->getReadyFulfiller());
  } else KJ_IF_MAYBE(locked, lock.state.tryGet<WritableLockImpl::PipeLocked>()) {
    lock.state.init<Unlocked>();
  }
}

kj::Maybe<int> WritableStreamJsController::getDesiredSize() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return nullptr;
    }
    KJ_CASE_ONEOF(controller, Controller) {
      return controller->getDesiredSize();
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamJsController::isErroring(jsg::Lock& js) {
  KJ_IF_MAYBE(controller, state.tryGet<Controller>()) {
    return (*controller)->isErroring(js);
  }
  return nullptr;
}

kj::Maybe<v8::Local<v8::Value>> WritableStreamJsController::isErroredOrErroring(
    jsg::Lock& js) {
  KJ_IF_MAYBE(err, state.tryGet<StreamStates::Errored>()) {
    return err->getHandle(js);
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

bool WritableStreamJsController::isLocked() const { return isLockedToWriter(); }

bool WritableStreamJsController::isLockedToWriter() const {
  return !lock.state.is<Unlocked>();
}

bool WritableStreamJsController::lockWriter(jsg::Lock& js, Writer& writer) {
  return lock.lockWriter(js, *this, writer);
}

void WritableStreamJsController::maybeRejectReadyPromise(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  KJ_IF_MAYBE(writerLock, lock.state.tryGet<WriterLocked>()) {
    if (writerLock->getReadyFulfiller() != nullptr) {
      maybeRejectPromise<void>(writerLock->getReadyFulfiller(), reason);
    } else {
      auto prp = js.newPromiseAndResolver<void>();
      prp.promise.markAsHandled();
      prp.resolver.reject(reason);
      writerLock->setReadyFulfiller(prp);
    }
  }
}

void WritableStreamJsController::maybeResolveReadyPromise() {
  KJ_IF_MAYBE(writerLock, lock.state.tryGet<WriterLocked>()) {
    maybeResolvePromise(writerLock->getReadyFulfiller());
  }
}

void WritableStreamJsController::releaseWriter(
    Writer& writer,
    kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseWriter(*this, writer, maybeJs);
}

kj::Maybe<kj::Own<WritableStreamSink>> WritableStreamJsController::removeSink(jsg::Lock& js) {
  KJ_UNIMPLEMENTED("WritableStreamJsController::removeSink is not implemented");
}

void WritableStreamJsController::setOwnerRef(WritableStream& stream) {
  owner = stream;
}

void WritableStreamJsController::setup(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSink> maybeUnderlyingSink,
    jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {
  auto underlyingSink = kj::mv(maybeUnderlyingSink).orDefault({});
  auto queuingStrategy = kj::mv(maybeQueuingStrategy).orDefault({});
  state = jsg::alloc<WritableStreamDefaultController>(*this);
  state.get<Controller>()->setup(js, kj::mv(underlyingSink), kj::mv(queuingStrategy));
}

kj::Maybe<jsg::Promise<void>> WritableStreamJsController::tryPipeFrom(
    jsg::Lock& js,
    jsg::Ref<ReadableStream> source,
    PipeToOptions options) {
  JSG_REQUIRE_NONNULL(ioContext, Error,
      "Unable to pipe to a WritableStream created outside of a request");

  // The ReadableStream source here can be either a JavaScript-backed ReadableStream
  // or ReadableStreamSource-backed. In either case, however, this WritableStream is
  // JavaScript-based and must use a JavaScript promise-based data flow for piping data.
  // We'll treat all ReadableStreams as if they are JavaScript-backed.
  //
  // This method will return a JavaScript promise that is resolved when the pipe operation
  // completes, or is rejected if the pipe operation is aborted or errored.

  // Let's also acquire the destination pipe lock.
  lock.pipeLock(KJ_ASSERT_NONNULL(owner), kj::mv(source), options);

  return pipeLoop(js).then(js, JSG_VISITABLE_LAMBDA((ref = addRef()), (ref), (auto& js) {}));
}

jsg::Promise<void> WritableStreamJsController::pipeLoop(jsg::Lock& js) {
  auto maybePipeLock = lock.tryGetPipe();
  if (maybePipeLock == nullptr) return js.resolvedPromise();
  auto& pipeLock = KJ_REQUIRE_NONNULL(maybePipeLock);

  auto preventAbort = pipeLock.preventAbort;
  auto preventCancel = pipeLock.preventCancel;
  auto preventClose = pipeLock.preventClose;
  auto pipeThrough = pipeLock.pipeThrough;
  auto& source = pipeLock.source;
  // At the start of each pipe step, we check to see if either the source or
  // the destination has closed or errored and propagate that on to the other.
  KJ_IF_MAYBE(promise, pipeLock.checkSignal(js, *this)) {
    lock.releasePipeLock();
    return kj::mv(*promise);
  }

  KJ_IF_MAYBE(errored, pipeLock.source.tryGetErrored(js)) {
    source.release(js);
    lock.releasePipeLock();
    if (!preventAbort) {
      auto onSuccess = JSG_VISITABLE_LAMBDA((pipeThrough, reason = js.v8Ref(*errored)),
                                            (reason), (jsg::Lock& js) {
        return rejectedMaybeHandledPromise<void>(
          js,
          reason.getHandle(js),
          pipeThrough);
      });
      auto promise = abort(js, *errored);
      if (IoContext::hasCurrent()) {
        return promise.then(js, IoContext::current().addFunctor(kj::mv(onSuccess)));
      } else {
        return promise.then(js, kj::mv(onSuccess));
      }
    }
    return rejectedMaybeHandledPromise<void>(js, *errored, pipeThrough);
  }

  KJ_IF_MAYBE(errored, state.tryGet<StreamStates::Errored>()) {
    lock.releasePipeLock();
    auto reason = errored->getHandle(js);
    if (!preventCancel) {
      source.release(js, reason);
    } else {
      source.release(js);
    }
    return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
  }

  KJ_IF_MAYBE(erroring, isErroring(js)) {
    lock.releasePipeLock();
    if (!preventCancel) {
      source.release(js, *erroring);
    } else {
      source.release(js);
    }
    return rejectedMaybeHandledPromise<void>(js, *erroring, pipeThrough);
  }

  if (source.isClosed()) {
    source.release(js);
    lock.releasePipeLock();
    if (!preventClose) {
      auto promise = close(js);
      if (pipeThrough) {
        promise.markAsHandled();
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

  auto onSuccess = [this,ref=addRef(),preventCancel,pipeThrough,&source]
          (jsg::Lock& js, ReadResult result) -> jsg::Promise<void> {
    auto maybePipeLock = lock.tryGetPipe();
    if (maybePipeLock == nullptr) return js.resolvedPromise();
    auto& pipeLock = KJ_REQUIRE_NONNULL(maybePipeLock);

    KJ_IF_MAYBE(promise, pipeLock.checkSignal(js, *this)) {
      lock.releasePipeLock();
      return kj::mv(*promise);
    }

    if (result.done) {
      // We'll handle the close at the start of the next iteration.
      return pipeLoop(js);
    }

    auto onSuccess = [this,ref=addRef()](jsg::Lock& js) { return pipeLoop(js); };

    auto onFailure = [ref=addRef(),&source, preventCancel, pipeThrough]
                     (jsg::Lock& js, jsg::Value value) {
      // The write failed. We handle it here because the pipe lock will have been released.
      auto reason = value.getHandle(js);
      if (!preventCancel) {
        source.release(js, reason);
      } else {
        source.release(js);
      }
      return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
    };

    auto promise = write(js, result.value.map([&](jsg::Value& value) {
      return value.getHandle(js.v8Isolate);
    }));

    return maybeAddFunctor(js, kj::mv(promise), kj::mv(onSuccess), kj::mv(onFailure));
  };

  auto onFailure = [this,ref=addRef()] (jsg::Lock& js, jsg::Value value) {
    // The read failed. We will handle the error at the start of the next iteration.
    return pipeLoop(js);
  };

  return maybeAddFunctor(js, pipeLock.source.read(js), kj::mv(onSuccess), kj::mv(onFailure));
}

void WritableStreamJsController::updateBackpressure(jsg::Lock& js, bool backpressure) {
  KJ_IF_MAYBE(writerLock, lock.state.tryGet<WriterLocked>()) {
    if (backpressure) {
      // Per the spec, when backpressure is updated and is true, we replace the existing
      // ready promise on the writer with a new pending promise, regardless of whether
      // the existing one is resolved or not.
      auto prp = js.newPromiseAndResolver<void>();
      prp.promise.markAsHandled();
      return writerLock->setReadyFulfiller(prp);
    }

    // When backpressure is updated and is false, we resolve the ready promise on the writer
    maybeResolvePromise(writerLock->getReadyFulfiller());
  }
}

jsg::Promise<void> WritableStreamJsController::write(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> value) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.rejectedPromise<void>(
          js.v8TypeError("This WritableStream has been closed."_kj));
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
  KJ_IF_MAYBE(readableController, tryGetReadableController()) {
    return readableController->getDesiredSize();
  }
  return nullptr;
}

void TransformStreamDefaultController::enqueue(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
  auto& readableController = JSG_REQUIRE_NONNULL(
      tryGetReadableController(), TypeError,
      "The readable side of this TransformStream is no longer readable.");
  JSG_REQUIRE(readableController.canCloseOrEnqueue(), TypeError,
      "The readable side of this TransformStream is no longer readable.");
  js.tryCatch([&] {
    readableController.enqueue(js, chunk);
  }, [&](jsg::Value exception) {
    errorWritableAndUnblockWrite(js, exception.getHandle(js));
    js.throwException(kj::mv(exception));
  });

  bool newBackpressure = readableController.hasBackpressure();
  if (newBackpressure != backpressure) {
    KJ_ASSERT(newBackpressure);
  }
}

void TransformStreamDefaultController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
  KJ_IF_MAYBE(readableController, tryGetReadableController()) {
    readableController->error(js, reason);
    maybeReadableController = nullptr;
  }
  errorWritableAndUnblockWrite(js, reason);
}

void TransformStreamDefaultController::terminate(jsg::Lock& js) {
  KJ_IF_MAYBE(readableController, tryGetReadableController()) {
    readableController->close(js);
    maybeReadableController = nullptr;
  }
  errorWritableAndUnblockWrite(js, js.v8TypeError("The transform stream has been terminated"_kj));
}

jsg::Promise<void> TransformStreamDefaultController::write(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
  KJ_IF_MAYBE(writableController, tryGetWritableController()) {
    KJ_IF_MAYBE(error, writableController->isErroredOrErroring(js)) {
      return js.rejectedPromise<void>(*error);
    }

    KJ_ASSERT(writableController->isWritable());

    if (backpressure) {
      auto chunkRef = js.v8Ref(chunk);
      return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved().then(js,
          JSG_VISITABLE_LAMBDA((chunkRef = kj::mv(chunkRef), ref=JSG_THIS),
                              (chunkRef, ref), (jsg::Lock& js) mutable -> jsg::Promise<void> {
        KJ_IF_MAYBE(writableController, ref->tryGetWritableController()) {
          KJ_IF_MAYBE(error, writableController->isErroring(js)) {
            return js.rejectedPromise<void>(*error);
          }
        }
        return ref->performTransform(js, chunkRef.getHandle(js));
      }));
    }
    return performTransform(js, chunk);
  } else {
    return js.rejectedPromise<void>(KJ_EXCEPTION(FAILED,
        "jsg.TypeError: Writing to the TransformStream failed."));
  }
}

jsg::Promise<void> TransformStreamDefaultController::abort(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  error(js, reason);
  return js.resolvedPromise();
}

jsg::Promise<void> TransformStreamDefaultController::close(jsg::Lock& js) {
  auto onSuccess = JSG_VISITABLE_LAMBDA(
      (ref=JSG_THIS), (ref), (jsg::Lock& js) -> jsg::Promise<void> {
    KJ_IF_MAYBE(readableController, ref->tryGetReadableController()) {
      // Allows for a graceful close of the readable side. Close will
      // complete once all of the queued data is read or the stream
      // errors.
      readableController->close(js);
    }
    return js.resolvedPromise();
  });

  auto onFailure = JSG_VISITABLE_LAMBDA(
      (ref=JSG_THIS),(ref),(jsg::Lock& js, jsg::Value reason) -> jsg::Promise<void> {
    ref->error(js, reason.getHandle(js));
    return js.rejectedPromise<void>(kj::mv(reason));
  });

  return jscontroller::maybeRunAlgorithm(
      js, algorithms.flush, kj::mv(onSuccess), kj::mv(onFailure), JSG_THIS);
}

jsg::Promise<void> TransformStreamDefaultController::pull(jsg::Lock& js) {
  KJ_ASSERT(backpressure);
  setBackpressure(js, false);
  return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved();
}

jsg::Promise<void> TransformStreamDefaultController::cancel(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  maybeReadableController = nullptr;
  errorWritableAndUnblockWrite(js, reason);
  return js.resolvedPromise();
}

jsg::Promise<void> TransformStreamDefaultController::performTransform(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
  KJ_IF_MAYBE(transform, algorithms.transform) {
    return jscontroller::maybeRunAlgorithm(
        js,
        transform,
        [](jsg::Lock& js) -> jsg::Promise<void> {
          return js.resolvedPromise();
        },
        JSG_VISITABLE_LAMBDA(
            (ref=JSG_THIS),(ref),(jsg::Lock& js, jsg::Value reason) -> jsg::Promise<void> {
          ref->error(js, reason.getHandle(js));
          return js.rejectedPromise<void>(kj::mv(reason));
        }),
        chunk,
        JSG_THIS);
  }
  // If we got here, there is no transform algorithm. Per the spec, the default
  // behavior then is to just pass along the value untransformed.
  return js.tryCatch([&] {
    enqueue(js, chunk);
    return js.resolvedPromise();
  }, [&](jsg::Value exception) {
    return js.rejectedPromise<void>(kj::mv(exception));
  });
}

void TransformStreamDefaultController::setBackpressure(jsg::Lock& js, bool newBackpressure) {
  KJ_ASSERT(newBackpressure != backpressure);
  KJ_IF_MAYBE(prp, maybeBackpressureChange) {
    prp->resolver.resolve();
  }
  maybeBackpressureChange = js.newPromiseAndResolver<void>();
  KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.markAsHandled();
  backpressure = newBackpressure;
}

void TransformStreamDefaultController::errorWritableAndUnblockWrite(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  algorithms.clear();
  KJ_IF_MAYBE(writableController, tryGetWritableController()) {
    if (writableController->isWritable()) {
      writableController->doError(js, reason);
    }
    maybeWritableController = nullptr;
  }
  if (backpressure) {
    setBackpressure(js, false);
  }
}

void TransformStreamDefaultController::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_MAYBE(backpressureChange, maybeBackpressureChange) {
    visitor.visit(backpressureChange->promise, backpressureChange->resolver);
  }
  visitor.visit(startPromise.resolver, startPromise.promise, algorithms);
}

void TransformStreamDefaultController::init(
    jsg::Lock& js,
    jsg::Ref<ReadableStream>& readable,
    jsg::Ref<WritableStream>& writable,
    jsg::Optional<Transformer> maybeTransformer) {
  KJ_ASSERT(maybeReadableController == nullptr);
  KJ_ASSERT(maybeWritableController == nullptr);
  maybeWritableController =
      static_cast<WritableStreamJsController&>(writable->getController()).getWeakRef();

  // The TransformStreamDefaultController needs to have a reference to the underlying controller
  // and not just the readable because if the readable is teed, or passed off to source, etc,
  // the TransformStream has to make sure that it can continue to interface with the controller
  // to push data into it.
  auto& readableController = static_cast<ReadableStreamJsController&>(readable->getController());
  auto readableRef = KJ_ASSERT_NONNULL(readableController.getController());
  maybeReadableController = KJ_ASSERT_NONNULL(
      readableRef.tryGet<jsg::Ref<ReadableStreamDefaultController>>())->getWeakRef();

  auto transformer = kj::mv(maybeTransformer).orDefault({});

  // TODO(someday): The stream standard includes placeholders for supporting byte-oriented
  // TransformStreams but does not yet define them. For now, we are limiting our implementation
  // here to only support value-based transforms.
  JSG_REQUIRE(transformer.readableType == nullptr, TypeError,
               "transformer.readableType must be undefined.");
  JSG_REQUIRE(transformer.writableType == nullptr, TypeError,
               "transformer.writableType must be undefined.");

  KJ_IF_MAYBE(transform, transformer.transform) {
    algorithms.transform = kj::mv(*transform);
  }

  KJ_IF_MAYBE(flush, transformer.flush) {
    algorithms.flush = kj::mv(*flush);
  }

  setBackpressure(js, true);

  algorithms.starting = jscontroller::maybeRunAlgorithm(
      js,
      transformer.start,
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js) {
        ref->algorithms.starting = nullptr;
        ref->startPromise.resolver.resolve();
      }),
      JSG_VISITABLE_LAMBDA((ref=JSG_THIS), (ref), (jsg::Lock& js, jsg::Value reason) {
        ref->algorithms.starting = nullptr;
        ref->startPromise.resolver.reject(reason.getHandle(js));
      }),
      JSG_THIS);
}

kj::Maybe<ReadableStreamDefaultController&>
TransformStreamDefaultController::tryGetReadableController() {
  KJ_IF_MAYBE(controller, maybeReadableController) {
    return (*controller)->tryGet();
  }
  return nullptr;
}

kj::Maybe<WritableStreamJsController&>
TransformStreamDefaultController::tryGetWritableController() {
  KJ_IF_MAYBE(controller, maybeWritableController) {
    return (*controller)->tryGet();
  }
  return nullptr;
}

}  // namespace workerd::api
