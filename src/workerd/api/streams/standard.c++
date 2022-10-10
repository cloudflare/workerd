// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "standard.h"
#include "readable.h"
#include "writable.h"
#include <workerd/jsg/buffersource.h>
#include <iterator>

namespace workerd::api {
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

  KJ_IF_MAYBE(algorithm, maybeAlgorithm) {
    return js.tryCatch([&] {
      return (*algorithm)(js, kj::fwd<decltype(args)>(args)...);
    }, [&](jsg::Value&& exception) {
      return js.rejectedPromise<void>(kj::mv(exception));
    }).then(js, kj::mv(onSuccess), kj::mv(onFailure));
  }

  // If the algorithm does not exist, we just handle it as a success and move on.
  onSuccess(js);
  return js.resolvedPromise();
}

kj::Maybe<size_t> getChunkSize(
    jsg::Lock& js,
    auto& sizeAlgorithm,
    v8::Local<v8::Value> value,
    auto onError) {
  KJ_IF_MAYBE(alg, sizeAlgorithm) {
    return js.tryCatch([&]() -> kj::Maybe<size_t> {
      return (*alg)(js, value);
    }, [&](jsg::Value&& exception) -> kj::Maybe<size_t> {
      onError(js, exception.getHandle(js));
      return nullptr;
    });
  }
  return 1;
}

// ======================================================================================

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
    KJ_CASE_ONEOF(locked, TeeLocked) {
      visitor.visit(locked);
    }
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      visitor.visit(locked);
    }
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

template <typename Controller>
kj::Maybe<ReadableStreamController::TeeController&> ReadableLockImpl<Controller>::tryTeeLock(
    Controller& self) {
// TODO(stream-tee): There will be no more tee controller and we have to rethink tee lock.
// Essentially, tee lock will mean the stream is effectively closed and no longer used because
// the underlying controller references have been passed on to the branches.
  if (isLockedToReader()) {
    return nullptr;
  }
  state.template init<TeeLocked>(self);
  return state.template get<TeeLocked>();
}

// TODO(stream-tee): Everything relating to TeeLocked here goes away.

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::addBranch(Branch* branch) {
  KJ_ASSERT(branches.find(BranchPtr(branch)) == nullptr,
            "branch should not already be in the list!");
  branches.insert(BranchPtr(branch));
}

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::close() {
  inner.state.template init<StreamStates::Closed>();
  forEachBranch([](auto& branch) { branch.doClose(); });
}

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::error(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  inner.state.template init<StreamStates::Errored>(js.v8Ref(reason));
  forEachBranch([&](auto& branch) { branch.doError(js, reason); });
  // Each of the branches should have removed themselves from the tee adapter
  // Let's make sure.
  KJ_ASSERT(branches.size() == 0);
}

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::ensurePulling(jsg::Lock& js) {
  KJ_IF_MAYBE(pulling, maybePulling) {
    pullAgain = true;
    return;
  }

  maybePulling = pull(js).then(js,
      JSG_VISITABLE_LAMBDA((this, ref = inner.addRef()), (ref),
                            (jsg::Lock& js, ReadResult result) {
    maybePulling = nullptr;

    forEachBranch([&](auto& branch) {
      branch.handleData(js, ReadResult {
        .value = result.value.map([&](jsg::Value& ref) -> jsg::Value {
          return ref.addRef(js);
        }),
        .done = result.done,
      });
    });

    if (pullAgain) {
      pullAgain = false;
      ensurePulling(js);
    }
    return js.resolvedPromise();
  }), JSG_VISITABLE_LAMBDA((this, ref = inner.addRef()),
                            (ref),
                            (jsg::Lock& js, jsg::Value value) {
    maybePulling = nullptr;
    return js.rejectedPromise<void>(kj::mv(value));
  }));
}

template <typename Controller>
jsg::Promise<ReadResult> ReadableLockImpl<Controller>::TeeLocked::pull(jsg::Lock& js) {
  if (inner.state.template is<StreamStates::Closed>()) {
    return js.resolvedPromise(ReadResult { .done = true });
  }

  KJ_IF_MAYBE(errored, inner.state.template tryGet<StreamStates::Errored>()) {
    return js.rejectedPromise<ReadResult>(errored->addRef(js));
  }

  return KJ_ASSERT_NONNULL(inner.read(js, nullptr));
}

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::removeBranch(
    Branch* branch,
    kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_ASSERT(branches.eraseMatch(BranchPtr(branch)),
      "Tee branch wasn't found? Possible invalid branch pointer.");

  KJ_IF_MAYBE(js, maybeJs) {
    if (branches.size() == 0) {
      inner.doCancel(*js, js->v8Undefined());
    }
  }
}

template <typename Controller>
void ReadableLockImpl<Controller>::TeeLocked::visitForGc(jsg::GcVisitor &visitor) {
  visitor.visit(maybePulling);
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
            JSG_VISITABLE_LAMBDA((this, reason = js.v8Ref(reason),
                                   ref = self.addRef()), (reason, ref), (jsg::Lock& js) {
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

template <typename Self>
jsg::Promise<void> ReadableImpl<Self>::cancel(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  KJ_ASSERT(state.template is<Readable>());
  KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {

// TODO(stream-tee): This should only be called if the last consumer known to the queue
// is canceling.

    // If we're already waiting for cancel to complete, just return the
    // already existing pending promise.
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

template <typename Self>
bool ReadableImpl<Self>::canCloseOrEnqueue() {
  return owner != nullptr && state.template is<Readable>() && !closeRequested;
}

template <typename Self>
ReadRequest ReadableImpl<Self>::dequeueReadRequest() {
// TODO(stream-tee): This goes away. The Queue::Consumer is responsible for tracking
// read requests.
  KJ_ASSERT(!readRequests.empty());
  auto request = kj::mv(readRequests.front());
  readRequests.pop_front();
  return kj::mv(request);
}

template <typename Self>
void ReadableImpl<Self>::doCancel(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    v8::Local<v8::Value> reason) {
  if (!state.template is<Readable>()) {
    return;
  }
  queue.reset();

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()),
                                         (self),
                                         (jsg::Lock& js) {
    algorithms.canceling = nullptr;
    doClose(js);
    KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
      maybeResolvePromise(pendingCancel->fulfiller);
    }
  });
  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()),
                                         (self),
                                         (jsg::Lock& js, jsg::Value reason) {
    algorithms.canceling = nullptr;
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
void ReadableImpl<Self>::doClose(jsg::Lock& js) {
// TODO(stream-tee): Closing and resetting the queue also needs to notify all of the
// still-attached consumers that they should close and detach immediately. All still
// pending reads should be closed out (if they are partially fulfilled) or canceled,
// and the references holding this controller should be cleared.
  if (!state.template is<Readable>()) {
    return;
  }
  state.template init<StreamStates::Closed>();
  queue.reset();
  algorithms.clear();

  for (auto& request : readRequests) {
    request.resolve(ReadResult { .done = true });
  }

  KJ_IF_MAYBE(theOwner, owner) {
    theOwner->doClose();
    owner = nullptr;
    // Calling doClose here most likely caused the ReadableImpl<Self> to be destroyed,
    // so it is important not to do anything else after calling doClose here.
  }
}

template <typename Self>
void ReadableImpl<Self>::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Error the queue, notify the consumers of the error. All still
// pending reads should be rejected and all still connected consumers should detach,
// keeping record of the error. All references holding this controller should be cleared.
  if (!state.template is<Readable>()) {
    return;
  }
  state = js.v8Ref(reason);
  queue.reset();
  algorithms.clear();

  while (!readRequests.empty()) {
    auto request = kj::mv(readRequests.front());
    readRequests.pop_front();
    request.reject(reason);
  }

  KJ_IF_MAYBE(theOwner, owner) {
    theOwner->doError(js, reason);
    owner = nullptr;
    // Calling doError here most likely caused the ReadableImpl<Self> to be destroyed,
    // so it is important not to do anything else after calling doError here.
  }
}

template <typename Self>
kj::Maybe<int> ReadableImpl<Self>::getDesiredSize() {
// TODO(stream-tee): Reimplemented by the queue
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return 0;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return nullptr;
    }
    KJ_CASE_ONEOF(readable, Readable) {
      return highWaterMark - queue.size();
    }
  }
  KJ_UNREACHABLE;
}

template <typename Self>
bool ReadableImpl<Self>::shouldCallPull() {
  if (!canCloseOrEnqueue()) {
    return false;
  }
  if (!started) {
    return false;
  }
// TODO(stream-tee): This will need to be reimplemented to see if any of the known
// consumers want data.
  if (getOwner().isLocked() && readRequests.size() > 0) {
    return true;
  }
  return getDesiredSize().orDefault(1) > 0;
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
    doError(js, reason.getHandle(js));
  });

  algorithms.pulling = maybeRunAlgorithm(js,
                                         algorithms.pull,
                                         kj::mv(onSuccess),
                                         kj::mv(onFailure),
                                         self.addRef());
}

template <typename Self>
void ReadableImpl<Self>::resolveReadRequest(
    ReadResult result,
    kj::Maybe<ReadRequest> maybeRequest) {
// TODO(stream-tee): This goes away because consumers become responsible for resolving reads.
  if (maybeRequest != nullptr) {
    maybeResolvePromise(maybeRequest, kj::mv(result));
    return;
  }
  dequeueReadRequest().resolve(kj::mv(result));
}

template <typename Self>
void ReadableImpl<Self>::setup(
    jsg::Lock& js,
    jsg::Ref<Self> self,
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy) {
  bool isBytes = underlyingSource.type.map([](auto& s) { return s == "bytes"; }).orDefault(false);

// TODO(stream-tee): the highWaterMark is handled by the queue impl
  highWaterMark = queuingStrategy.highWaterMark.orDefault(isBytes ? 0 : 1);

  auto startAlgorithm = kj::mv(underlyingSource.start);
  algorithms.pull = kj::mv(underlyingSource.pull);
  algorithms.cancel = kj::mv(underlyingSource.cancel);
  algorithms.size = kj::mv(queuingStrategy.size);

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    algorithms.starting = nullptr;
    started = true;
    pullIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this,self = self.addRef()), (self),
                                         (jsg::Lock& js, jsg::Value reason) {
    algorithms.starting = nullptr;
    started = true;
    doError(js, reason.getHandle(js));
  });

  algorithms.starting = maybeRunAlgorithm(js,
                                          startAlgorithm,
                                          kj::mv(onSuccess),
                                          kj::mv(onFailure),
                                          self.addRef());
}

template <typename Self>
void ReadableImpl<Self>::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_MAYBE(error, state.tryGet<StreamStates::Errored>()) {
    visitor.visit(*error);
  }
  KJ_IF_MAYBE(pendingCancel, maybePendingCancel) {
    visitor.visit(pendingCancel->fulfiller, pendingCancel->promise);
  }
  visitor.visit(algorithms, queue);
  visitor.visitAll(readRequests);
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
  return highWaterMark - queue.size();
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

  if (queue.empty()) {
    return;
  }

  if (queue.frontIsClose()) {
    KJ_ASSERT(inFlightClose == nullptr);
    KJ_ASSERT_NONNULL(closeRequest);
    inFlightClose = kj::mv(closeRequest);
    queue.template pop<ValueQueue::Close>();
    KJ_ASSERT(queue.empty());

    auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
      algorithms.closing = nullptr;
      finishInFlightClose(js, kj::mv(self));
    });

    auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                           (jsg::Lock& js, jsg::Value reason) {
      algorithms.closing = nullptr;
      finishInFlightClose(js, kj::mv(self), reason.getHandle(js));
    });

    algorithms.closing = maybeRunAlgorithm(js,
                                           algorithms.close,
                                           kj::mv(onSuccess),
                                           kj::mv(onFailure));
    return;
  }

  auto& chunk = queue.peek();

  KJ_ASSERT(inFlightWrite == nullptr);
  inFlightWrite = dequeueWriteRequest();

  auto onSuccess = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self), (jsg::Lock& js) {
    algorithms.writing = nullptr;
    finishInFlightWrite(js, self.addRef());
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
    queue.pop();
    if (!isCloseQueuedOrInFlight() && state.template is<Writable>()) {
      updateBackpressure(js);
    }
    advanceQueueIfNeeded(js, kj::mv(self));
  });

  auto onFailure = JSG_VISITABLE_LAMBDA((this, self = self.addRef()), (self),
                                         (jsg::Lock& js, jsg::Value reason) {
    algorithms.writing = nullptr;
    finishInFlightWrite(js, kj::mv(self), reason.getHandle(js));
  });

  algorithms.writing = maybeRunAlgorithm(js,
                                         algorithms.write,
                                         kj::mv(onSuccess),
                                         kj::mv(onFailure),
                                         chunk.value.getHandle(js),
                                         self.addRef());
}

template <typename Self>
jsg::Promise<void> WritableImpl<Self>::close(jsg::Lock& js, jsg::Ref<Self> self) {
  KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
  KJ_ASSERT(!isCloseQueuedOrInFlight());

  auto prp = js.newPromiseAndResolver<void>();
  closeRequest = kj::mv(prp.resolver);

  if (backpressure && state.template is<Writable>() && owner != nullptr) {
    getOwner().maybeResolveReadyPromise();
  }

  queue.close();
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
WriteRequest WritableImpl<Self>::dequeueWriteRequest() {
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
  queue.reset();
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
  queue.reset();
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
  KJ_ASSERT(inFlightWrite == nullptr && inFlightClose == nullptr);
  state.template init<StreamStates::Errored>(kj::mv(erroring.reason));

  queue.reset();

  while (!writeRequests.empty()) {
    dequeueWriteRequest().reject(reason);
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
  KJ_ASSERT_NONNULL(inFlightWrite);

  KJ_IF_MAYBE(reason, maybeReason) {
    maybeRejectPromise<void>(inFlightWrite, *reason);
    KJ_ASSERT(state.template is<Writable>() || state.template is<StreamStates::Erroring>());
    return dealWithRejection(js, kj::mv(self), *reason);
  }

  maybeResolvePromise(inFlightWrite);
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
  size_t size = jscontroller::getChunkSize(
      js,
      algorithms.size,
      value,
      [&](jsg::Lock& js, v8::Local<v8::Value> error) {
    if (state.template is<Writable>()) {
      algorithms.clear();
      startErroring(js, self.addRef(), error);
    }
  }).orDefault(1);

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
  writeRequests.push_back(kj::mv(prp.resolver));

  queue.push(ValueQueueEntry {
    .value = js.v8Ref(value),
    .size = size
  });

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
                queue,
                signal,
                maybePendingAbort);
  visitor.visitAll(writeRequests);
}
}  // namespace jscontroller

// =======================================================================================

ReadableStreamDefaultController::ReadableStreamDefaultController(ReaderOwner& owner)
    : impl(owner) {}
// TODO(stream-tee): The controller will no longer have any notion of a single owner. As long
// as there are consumers known to the queue, the controller will be kept alive. State changes
// are communicated via the consumer.

void ReadableStreamDefaultController::setOwner(kj::Maybe<ReaderOwner&> owner) {
  impl.setOwner(owner);
}

jsg::Promise<void> ReadableStreamDefaultController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
// TODO(stream-tee): This cancel is triggered by the ReadableStreamJsController via the
// ValueReadable or ByteReadable. Some of the functionality needs to be moved into those.
// For instance, the individual consumer will be responsible for resolving its own
// collection of read requests. The cancel algorithm will only be invoked when the
// last consumer known to the queue is canceling.
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault(js.v8Undefined()));
}

void ReadableStreamDefaultController::close(jsg::Lock& js) {
// TODO(stream-tee): This close is triggered by user-call. It must result in the queue
// being closed, which in turn will communicate the close to each of the consumers.
// It will be the responsibility of the consumers to cancel their pending pull intos.
  JSG_REQUIRE(impl.canCloseOrEnqueue(),
               TypeError,
               "This ReadableStreamDefaultController is closed.");
  impl.closeRequested = true;
  if (impl.queue.empty()) {
    impl.doClose(js);
  }
}

void ReadableStreamDefaultController::doCancel(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Revisit this
  impl.doCancel(js, JSG_THIS, reason);
}

void ReadableStreamDefaultController::enqueue(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> chunk) {
  JSG_REQUIRE(impl.canCloseOrEnqueue(),
               TypeError,
               "This ReadableStreamDefaultController is closed.");
  doEnqueue(js, chunk);
}

void ReadableStreamDefaultController::doEnqueue(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> chunk) {
// TODO(stream-tee): Do we need the separate doEnqueue? The implementation here changes a bit.
// the enqueue will push into the ValueQueue and the ValueQueue::Consumers will handle resolving
// the reads.
  KJ_ASSERT(impl.canCloseOrEnqueue());

  auto value = chunk.orDefault(js.v8Undefined());

  KJ_DEFER(impl.pullIfNeeded(js, JSG_THIS));
  if (!impl.getOwner().isLocked() || impl.readRequests.empty()) {
    KJ_IF_MAYBE(size, jscontroller::getChunkSize(
        js,
        impl.algorithms.size,
        value,
        [&](jsg::Lock& js, v8::Local<v8::Value> error) { impl.doError(js, error); })) {
      impl.queue.push(jscontroller::ValueQueueEntry { js.v8Ref(value), *size });
    }
    return;
  }

  KJ_ASSERT(impl.queue.empty());
  impl.resolveReadRequest(
      ReadResult {
        .value = js.v8Ref(value),
        .done = false,
      });
}

void ReadableStreamDefaultController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Called by user-call. We need to notify the queue of the error and notify
// all of the consumers about the error.
  if (impl.state.is<jscontroller::Readable>()) {
    impl.doError(js, reason);
  }
}

kj::Maybe<int> ReadableStreamDefaultController::getDesiredSize() {
  return impl.getDesiredSize();
}

bool ReadableStreamDefaultController::hasPendingReadRequests() {
// TODO(stream-tee): Needs to be modified such that if any consumer has pending reads, this
// returns true.
  return !impl.readRequests.empty();
}

void ReadableStreamDefaultController::pull(jsg::Lock& js, ReadRequest readRequest) {
// TODO(stream-tee): Needs to be reimplemented in terms of the consumer. Essentially, the consumer
// receives the read request. If the consumer doesn't have the data in it's own queue to serve
// the request, it will ask the queue to get it. If there's no data in the queue, then we need
// to pull from the underlying source.

  // This should only be called if the stream is readable
  KJ_ASSERT(impl.state.is<jscontroller::Readable>());
  if (!impl.queue.empty()) {
    // Here the entry should always be a ValueQueueEntry.
    auto entry = impl.queue.pop();
    if (impl.closeRequested && impl.queue.empty()) {
      impl.doClose(js);
    } else {
      impl.pullIfNeeded(js, JSG_THIS);
    }
    impl.resolveReadRequest(
        ReadResult {
          .value = kj::mv(entry.value),
          .done = false,
        },
        kj::mv(readRequest));
    return;
  }
  impl.readRequests.push_back(kj::mv(readRequest));
  impl.pullIfNeeded(js, JSG_THIS);
}

jsg::Promise<ReadResult> ReadableStreamDefaultController::read(jsg::Lock& js) {

  if (impl.state.is<StreamStates::Closed>()) {
    return js.resolvedPromise(ReadResult { .done = true });
  }

  KJ_IF_MAYBE(errored, impl.state.tryGet<StreamStates::Errored>()) {
    return js.rejectedPromise<ReadResult>(errored->addRef(js));
  }

  auto prp = js.newPromiseAndResolver<ReadResult>();
  pull(js, kj::mv(prp.resolver));
  return kj::mv(prp.promise);
}

void ReadableStreamDefaultController::setup(
    jsg::Lock& js,
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy) {
  impl.setup(js, JSG_THIS, kj::mv(underlyingSource), kj::mv(queuingStrategy));
}

// ======================================================================================

// TODO(stream-tee): The ReadableStreamBYOBRequest needs to be modified to operate in
// terms of the ByteQueue::Consumer model.

ReadableStreamBYOBRequest::Impl::Impl(
      jsg::V8Ref<v8::Uint8Array> view,
      jsg::Ref<ReadableByteStreamController> controller,
      size_t atLeast)
      : view(kj::mv(view)),
        controller(kj::mv(controller)),
        atLeast(atLeast) {}

void ReadableStreamBYOBRequest::visitForGc(jsg::GcVisitor& visitor) {
    KJ_IF_MAYBE(impl, maybeImpl) {
      visitor.visit(impl->view, impl->controller);
    }
  }

ReadableStreamBYOBRequest::ReadableStreamBYOBRequest(
    jsg::V8Ref<v8::Uint8Array> view,
    jsg::Ref<ReadableByteStreamController> controller,
    size_t atLeast)
    : maybeImpl(Impl(kj::mv(view), kj::mv(controller), atLeast)) {}

kj::Maybe<int> ReadableStreamBYOBRequest::getAtLeast() {
  KJ_IF_MAYBE(impl, maybeImpl) {
    return impl->atLeast;
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
    impl->view.getHandle(js)->Buffer()->Detach();
    impl->controller->maybeByobRequest = nullptr;
  }
  maybeImpl = nullptr;
}

void ReadableStreamBYOBRequest::respond(jsg::Lock& js, int bytesWritten) {
  auto& impl = JSG_REQUIRE_NONNULL(maybeImpl,
                                    TypeError,
                                    "This ReadableStreamBYOBRequest has been invalidated.");
  JSG_REQUIRE(!impl.controller->pendingPullIntos.empty(),
               TypeError,
               "There are no pending BYOB read requests.");

  if (!impl.controller->isReadable()) {
    JSG_REQUIRE(bytesWritten == 0,
                 TypeError,
                 "The bytesWritten must be zero after the stream is closed.");
  } else {
    JSG_REQUIRE(bytesWritten > 0,
                 TypeError,
                 "The bytesWritten must be more than zero while the stream is open.");
  }

  auto& pullInto = impl.controller->pendingPullIntos.front();
  JSG_REQUIRE(pullInto.filled + bytesWritten <= pullInto.store.size(),
               RangeError, "Too many bytes written.");

  // Spec says to detach pullInto's buffer, but it's just a backing store
  // and we'll be invalidating the BYOBRequest in the next step so skip that...
  impl.controller->respondInternal(js, bytesWritten);
}

void ReadableStreamBYOBRequest::respondWithNewView(jsg::Lock& js, jsg::BufferSource view) {
  auto& impl = JSG_REQUIRE_NONNULL(maybeImpl,
                                    TypeError,
                                    "This ReadableStreamBYOBRequest has been invalidated.");
  JSG_REQUIRE(!impl.controller->pendingPullIntos.empty(),
               TypeError,
               "There are no pending BYOB read requests.");

  if (!impl.controller->isReadable()) {
    JSG_REQUIRE(view.size() == 0,
                 TypeError,
                 "The view byte length must be zero after the stream is closed.");
  } else {
    JSG_REQUIRE(view.size() > 0,
                 TypeError,
                 "The view byte length must be more than zero while the stream is open.");
  }

  impl.controller->respondInternal(js, impl.controller->updatePullInto(js, kj::mv(view)));
}

// ======================================================================================

ReadableByteStreamController::ReadableByteStreamController(ReaderOwner& owner)
    : impl(owner) {
// TODO(stream-tee): The notion of a single owner for the controller goes away. The controller
// will have one or more consumers. As long as there are consumers, the controller will be
// available.
}

kj::Maybe<int> ReadableByteStreamController::getDesiredSize() {
  return impl.getDesiredSize();
}

jsg::Promise<void> ReadableByteStreamController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
// TODO(stream-tee): This cancel is triggered by the ReadableStreamJsController via the
// ValueReadable or ByteReadable. Some of the functionality needs to be moved into those.
// For instance, the individual consumer will be responsible for resolving its own
// collection of read requests. The cancel algorithm will only be invoked when the
// last consumer known to the queue is canceling.
  pendingPullIntos.clear();
  while (!impl.readRequests.empty()) {
    impl.dequeueReadRequest().resolve(ReadResult { .done = true });
  }
  return impl.cancel(js, JSG_THIS, maybeReason.orDefault(js.v8Undefined()));
}

void ReadableByteStreamController::close(jsg::Lock& js) {
// TODO(stream-tee): This close is triggered by user-call. It must result in the queue
// being closed, which in turn will communicate the close to each of the consumers.
// It will be the responsibility of the consumers to cancel their pending pull intos.
  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError,
               "This ReadableByteStreamController is closed.");
  if (!impl.queue.empty()) {
    impl.closeRequested = true;
    return;
  }
  if (!pendingPullIntos.empty()) {
    auto& pullInto = pendingPullIntos.front();
    if (pullInto.filled > 0) {
      auto error = js.v8TypeError(
          "This ReadablebyteStreamController was closed with a partial BYOB read"_kj);
      impl.doError(js, error);
      jsg::throwTunneledException(js.v8Isolate, error);
    }
  }
  impl.doClose(js);
}

void ReadableByteStreamController::commitPullInto(jsg::Lock& js, PendingPullInto pullInto) {
// TODO(stream-tee): Responsibility here moves into the ByteQueue::Consumer implementation.
  bool done = false;
  if (impl.state.is<StreamStates::Closed>()) {
    KJ_ASSERT(pullInto.filled == 0);
    done = true;
  }
  pullInto.store.trim(pullInto.store.size() - pullInto.filled);
  impl.resolveReadRequest(
      ReadResult {
        .value = js.v8Ref(pullInto.store.createHandle(js)),
        .done = done,
      });
}

ReadableByteStreamController::PendingPullInto
ReadableByteStreamController::dequeuePendingPullInto() {
// TODO(stream-tee): Responsibility here moves into the ByteQueue::Consumer implementation.
  KJ_ASSERT(!pendingPullIntos.empty());
  auto pullInto = kj::mv(pendingPullIntos.front());
  pendingPullIntos.pop_front();
  return kj::mv(pullInto);
}

void ReadableByteStreamController::doCancel(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Re-evaluate...
  impl.doCancel(js, JSG_THIS, reason);
}

void ReadableByteStreamController::enqueue(jsg::Lock& js, jsg::BufferSource chunk) {
// TODO(stream-tee): This is called by user-call. It must be modified to push data into the queue,
// which will trigger the cascade of that data into the consumers, which will handle the majority
// of the handling here.
  JSG_REQUIRE(chunk.size() > 0, TypeError, "Cannot enqueue a zero-length ArrayBuffer.");
  JSG_REQUIRE(chunk.canDetach(js), TypeError,
               "The provided ArrayBuffer must be detachable.");
  JSG_REQUIRE(impl.canCloseOrEnqueue(), TypeError, "This ReadableByteStreamController is closed.");

  auto backing = chunk.detach(js);

  KJ_IF_MAYBE(byobRequest, maybeByobRequest) {
    (*byobRequest)->invalidate(js);
  }

  const auto enqueueChunk = [&] {
    impl.queue.push(jscontroller::ByteQueueEntry { .store = kj::mv(backing) });
  };

  KJ_DEFER(impl.pullIfNeeded(js, JSG_THIS));
  if (!impl.getOwner().isLocked() || impl.readRequests.empty()) {
    return enqueueChunk();
  }

  if (impl.getOwner().isLockedReaderByteOriented()) {
    enqueueChunk();
    pullIntoUsingQueue(js);
  } else {
    KJ_ASSERT(impl.queue.empty());
    if (!pendingPullIntos.empty()) {
      auto pending = dequeuePendingPullInto();
      KJ_ASSERT(pending.type == PendingPullInto::Type::DEFAULT);
    }
    impl.resolveReadRequest(
      ReadResult {
        .value = js.v8Ref(backing.getTypedView<v8::Uint8Array>().createHandle(js)),
        .done = false,
      });
  }
}

void ReadableByteStreamController::error(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): This is called by the user-call. It must error the queue and communicate to
// each of the consumers that the readable stream is immediately shutting down because of error.
  if (impl.state.is<jscontroller::Readable>()) {
    impl.doError(js, reason);
  }
}

bool ReadableByteStreamController::fillPullInto(PendingPullInto& pullInto) {
// TODO(stream-tee): Responsibility here moves into the ByteQueue::Consumer implementation.
  auto elementSize = pullInto.store.getElementSize();
  auto currentAlignedBytes = pullInto.filled - (pullInto.filled % elementSize);
  auto maxBytesToCopy = kj::min(impl.queue.size(), pullInto.store.size() - pullInto.filled);
  auto maxBytesFilled = pullInto.filled + maxBytesToCopy;
  auto maxAlignedBytes = maxBytesFilled - (maxBytesFilled % elementSize);
  auto totalBytesToCopyRemaining = maxBytesToCopy;
  bool ready = false;

  if (maxAlignedBytes > currentAlignedBytes) {
    totalBytesToCopyRemaining = maxAlignedBytes - pullInto.filled;
    ready = true;
  }

  auto destination = pullInto.store.asArrayPtr().begin();

  while (totalBytesToCopyRemaining > 0) {
    // The head will always be a ByteQueueEntry here
    auto& head = impl.queue.peek();
    auto bytesToCopy = kj::min(totalBytesToCopyRemaining, head.store.size());
    memcpy(destination, head.store.asArrayPtr().begin(), bytesToCopy);
    if (head.store.size() == bytesToCopy) {
      auto removeHead = kj::mv(head);
      impl.queue.pop();
    } else {
      head.store.consume(bytesToCopy);
      impl.queue.dec(bytesToCopy);
    }
    KJ_ASSERT(maybeByobRequest == nullptr);
    pullInto.filled += bytesToCopy;
    totalBytesToCopyRemaining -= bytesToCopy;
    destination += bytesToCopy;
  }

  if (!ready) {
    KJ_ASSERT(impl.queue.empty());
    KJ_ASSERT(pullInto.filled > 0);
    KJ_ASSERT(pullInto.filled < elementSize);
  }

  return ready;
}

kj::Maybe<jsg::Ref<ReadableStreamBYOBRequest>> ReadableByteStreamController::getByobRequest(
    jsg::Lock& js) {
// TODO(stream-tee): The ReadableStreamBYOBRequest mechanism needs to be reworked around
// ByteQueue::Consumer's similar concept.
  JSG_REQUIRE(impl.state.is<jscontroller::Readable>(),
               TypeError,
               "This ReadableByteStreamController has been closed.");
  if (maybeByobRequest == nullptr && !pendingPullIntos.empty()) {
    auto& pullInto = pendingPullIntos.front();
    auto view = pullInto.store.getTypedView<v8::Uint8Array>();
    view.consume(pullInto.filled);
    maybeByobRequest =
        jsg::alloc<ReadableStreamBYOBRequest>(
            js.v8Ref(view.createHandle(js).As<v8::Uint8Array>()),
            JSG_THIS,
            pullInto.atLeast);
  }
  return kj::mv(maybeByobRequest);
}

bool ReadableByteStreamController::hasPendingReadRequests() {
// TODO(stream-tee): Needs to be modified such that if any known consumer has a pending
// read request this returns true. The controller itself will no longer track read requests
// itself.
  return !impl.readRequests.empty();
}

bool ReadableByteStreamController::isReadable() const {
  return impl.state.is<jscontroller::Readable>();
}

void ReadableByteStreamController::pullIntoUsingQueue(jsg::Lock& js) {
// TODO(stream-tee): Responsibility here moves into the ByteQueue::Consumer implementation.
  KJ_ASSERT(!impl.closeRequested);
  while (!pendingPullIntos.empty() && !impl.queue.empty()) {
    auto& pullInto = pendingPullIntos.front();
    if (fillPullInto(pullInto)) {
      commitPullInto(js, dequeuePendingPullInto());
    }
  }
}

void ReadableByteStreamController::pull(jsg::Lock& js, ReadRequest readRequest) {
// TODO(stream-tee): Triggers the pull algorithm to be called on this controller.
// We will need to rework the impl.queue.empty() piece below. Is it even still
// relevant with the new queue model?

  // This should only ever be called if the stream is readable
  KJ_ASSERT(impl.state.is<jscontroller::Readable>());
  if (!impl.queue.empty()) {
    KJ_ASSERT(impl.readRequests.empty());
    auto entry = impl.queue.pop();
    queueDrain(js);
    impl.resolveReadRequest(
      ReadResult {
        .value = js.v8Ref(entry.store.getTypedView<v8::Uint8Array>().createHandle(js)),
        .done = false,
      },
      kj::mv(readRequest));
    return;
  }
  // Per the spec, we're only supposed to follow the next step if autoAllocateChunkSize
  // is enabled. We *always* support autoAllocateChunkSize. If the user has not specified
  // the size explicitly, we'll use a default, so autoAllocateChunkSize is never undefined.
  pendingPullIntos.push_back(PendingPullInto {
    .store = jsg::BackingStore::alloc<v8::Uint8Array>(js, autoAllocateChunkSize),
    .filled = 0,
    .atLeast = 1,
    .type = PendingPullInto::Type::DEFAULT
  });

  impl.readRequests.push_back(kj::mv(readRequest));
  impl.pullIfNeeded(js, JSG_THIS);
}

void ReadableByteStreamController::queueDrain(jsg::Lock& js) {
// TODO(stream-tee): Need to re-evaluate this. Essentially, this is a graceful
// close. If close has been requested and the queue is empty, we signal to all
// consumers that we're done and they should disconnect, otherwise we try
// pulling more data.
  if (impl.queue.size() == 0 && impl.closeRequested) {
    return impl.doClose(js);
  }
  impl.pullIfNeeded(js, JSG_THIS);
}

jsg::Promise<ReadResult> ReadableByteStreamController::read(
    jsg::Lock& js,
    kj::Maybe<ReadableStreamController::ByobOptions> maybeByobOptions) {
// TODO(stream-tee): This likely goes away. It will be up to the ByteQueue::Consumer
// to handle this functionality.
  if (impl.state.is<StreamStates::Closed>()) {
    KJ_IF_MAYBE(byobOptions, maybeByobOptions) {
      // We're going to return an empty ArrayBufferView using the same underlying buffer but with
      // the length set to 0, and with the same type as the one we were given.
      auto source = jsg::BufferSource(js, byobOptions->bufferView.getHandle(js));
      auto store = source.detach(js);
      store.consume(store.size());  // Ensures that our return is zero-length.

      return js.resolvedPromise(ReadResult {
        .value = js.v8Ref(store.createHandle(js)),
        .done = true,
      });
    }

    // We weren't given an ArrayBufferView to fill but we still want to return an empty one here.
    return js.resolvedPromise(ReadResult {
      .value = js.v8Ref(v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, 0), 0, 0)
          .As<v8::Value>()),
      .done = true,
    });
  }

  KJ_IF_MAYBE(errored, impl.state.tryGet<StreamStates::Errored>()) {
    return js.rejectedPromise<ReadResult>(errored->addRef(js));
  }

  auto readRequest = js.newPromiseAndResolver<ReadResult>();

  KJ_IF_MAYBE(byobOptions, maybeByobOptions) {
    auto source = jsg::BufferSource(js, byobOptions->bufferView.getHandle(js));
    auto store = source.detach(js);
    auto pullInto = PendingPullInto {
      .store = kj::mv(store),
      .filled = 0,
      .atLeast = byobOptions->atLeast.orDefault(1),
      .type = PendingPullInto::Type::BYOB,
    };

    if (!pendingPullIntos.empty()) {
      pendingPullIntos.push_back(kj::mv(pullInto));
      impl.readRequests.push_back(kj::mv(readRequest.resolver));
      return kj::mv(readRequest.promise);
    }

    if (!impl.queue.empty()) {
      if (fillPullInto(pullInto)) {
        pullInto.store.trim(pullInto.store.size() - pullInto.filled);
        v8::Local<v8::Value> view = pullInto.store.createHandle(js);
        queueDrain(js);
        readRequest.resolver.resolve(ReadResult {
          .value = js.v8Ref(view),
          .done = false,
        });
        return kj::mv(readRequest.promise);
      }

      if (impl.closeRequested) {
        auto error = js.v8TypeError("This ReadableStream is closed."_kj);
        impl.doError(js, error);
        readRequest.resolver.reject(error);
        return kj::mv(readRequest.promise);
      }
    }

    pendingPullIntos.push_back(kj::mv(pullInto));
    impl.readRequests.push_back(kj::mv(readRequest.resolver));

    impl.pullIfNeeded(js, JSG_THIS);
    return kj::mv(readRequest.promise);
  }

  // Using the default reader!
  pull(js, kj::mv(readRequest.resolver));
  return kj::mv(readRequest.promise);
}

void ReadableByteStreamController::respondInternal(jsg::Lock& js, size_t bytesWritten) {
// TODO(stream-tee): This likely goes away and is handled by the ByteQueue::Consumer.
  auto& pullInto = pendingPullIntos.front();
  KJ_DEFER(KJ_IF_MAYBE(byobRequest, maybeByobRequest) {
    (*byobRequest)->invalidate(js);
  });

  if (impl.state.is<StreamStates::Closed>()) {
    KJ_ASSERT(bytesWritten == 0);
    KJ_ASSERT(pullInto.filled == 0);
    if (impl.getOwner().isLockedReaderByteOriented()) {
      while (!impl.readRequests.empty()) {
        commitPullInto(js, dequeuePendingPullInto());
      }
    }
  } else {
    auto elementSize = pullInto.store.getElementSize();
    KJ_ASSERT(pullInto.filled + bytesWritten <= pullInto.store.size());
    KJ_ASSERT(pendingPullIntos.empty() || &pendingPullIntos.front() == &pullInto);
    KJ_ASSERT(maybeByobRequest == nullptr);
    pullInto.filled += bytesWritten;
    if (pullInto.filled < elementSize) {
      return;
    }
    pullInto = dequeuePendingPullInto();
    auto remainderSize = pullInto.filled % elementSize;
    if (remainderSize > 0) {
      auto end = pullInto.store.getOffset() + pullInto.filled;
      auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, remainderSize);
      memcpy(
        backing.asArrayPtr().begin(),
        pullInto.store.asArrayPtr().begin() + (end - remainderSize),
        remainderSize);
      impl.queue.push(jscontroller::ByteQueueEntry { .store = kj::mv(backing) });
    }

    pullInto.filled -= remainderSize;
    commitPullInto(js, kj::mv(pullInto));
    pullIntoUsingQueue(js);
  }
  impl.pullIfNeeded(js, JSG_THIS);
}

void ReadableByteStreamController::setup(
    jsg::Lock& js,
    UnderlyingSource underlyingSource,
    StreamQueuingStrategy queuingStrategy) {
  int autoAllocateChunkSize =
      underlyingSource.autoAllocateChunkSize.orDefault(
          UnderlyingSource::DEFAULT_AUTO_ALLOCATE_CHUNK_SIZE);
  JSG_REQUIRE(autoAllocateChunkSize > 0,
               TypeError,
               "The autoAllocateChunkSize option cannot be zero.");
  this->autoAllocateChunkSize = autoAllocateChunkSize;

  impl.setup(js, JSG_THIS, kj::mv(underlyingSource), kj::mv(queuingStrategy));
}

size_t ReadableByteStreamController::updatePullInto(jsg::Lock& js, jsg::BufferSource view) {
// TODO(stream-tee): This likely goes away and is handled by the ByteQueue::Consumer.
  auto& pullInto = pendingPullIntos.front();
  auto byteLength = view.size();
  JSG_REQUIRE(view.canDetach(js), TypeError,
               "Unable to use non-detachable ArrayBuffer.");
  JSG_REQUIRE(pullInto.store.getOffset() + pullInto.filled == view.getOffset(),
               RangeError,
               "The given view has an invalid byte offset.");
  JSG_REQUIRE(pullInto.store.size() == view.underlyingArrayBufferSize(js),
               RangeError,
               "The underlying ArrayBuffer is not the correct length.");
  JSG_REQUIRE(pullInto.filled + byteLength <= pullInto.store.size(),
               RangeError,
               "The view is not the correct length.");
  pullInto.store = view.detach(js);
  return byteLength;
}

// ======================================================================================

// TODO(stream-tee): Everything JsTeeController goes away

ReadableStreamJsTeeController::Attached::Attached(
    jsg::Ref<ReadableStream> ref,
    TeeController& controller)
    : ref(kj::mv(ref)), controller(controller) {};

ReadableStreamJsTeeController::ReadableStreamJsTeeController(
    jsg::Ref<ReadableStream> baseStream,
    TeeController& teeController)
    : state(Readable()),
      innerState(Attached(kj::mv(baseStream), teeController)) {}

ReadableStreamJsTeeController::ReadableStreamJsTeeController(
    jsg::Lock& js,
    kj::Maybe<Attached> attached,
    Queue& queue)
    : state(Readable()),
      innerState(kj::mv(attached)),
      queue(copyQueue(queue, js)) {}

ReadableStreamJsTeeController::ReadableStreamJsTeeController(ReadableStreamJsTeeController&& other)
    : owner(kj::mv(other.owner)),
      state(kj::mv(other.state)),
      innerState(kj::mv(other.innerState)),
      lock(kj::mv(other.lock)),
      disturbed(other.disturbed) {}

ReadableStreamJsTeeController::Queue ReadableStreamJsTeeController::copyQueue(
    Queue& queue,
    jsg::Lock& js) {
  ReadableStreamJsTeeController::Queue newQueue;
  for (auto& item : queue) {
    KJ_IF_MAYBE(value, item.value) {
      newQueue.push_back(ReadResult { .value = value->addRef(js), .done = item.done });
    } else {
      newQueue.push_back(ReadResult { .done = item.done });
    }
  }
  return kj::mv(newQueue);
}

ReadableStreamJsTeeController::~ReadableStreamJsTeeController() noexcept(false) {
  // There's a good chance that we're cleaning up during garbage collection here.
  // In that case, we don't want detach to go off and cancel any remainin read
  // promises as that would potentially involve allocating JS stuff during GC,
  // which is a no no.
  detach(nullptr);
};

jsg::Ref<ReadableStream> ReadableStreamJsTeeController::addRef() {
  return KJ_ASSERT_NONNULL(owner).addRef();
}

jsg::Promise<void> ReadableStreamJsTeeController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> reason) {
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<void>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      doCancel(js, reason.orDefault(js.v8Undefined()));
      return js.resolvedPromise();
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsTeeController::detach(kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_MAYBE(inner, innerState) {
    inner->controller.removeBranch(this, maybeJs);
  }
  innerState = nullptr;
}

void ReadableStreamJsTeeController::doCancel(jsg::Lock& js, v8::Local<v8::Value> reason) {
  // Canceling a tee controller does several things:
  // 1. Clears the queue
  // 2. Sets both the state and innerState to closed.
  // 3. Flushes remaining read requests
  queue.clear();
  finishClosing(js);
}

void ReadableStreamJsTeeController::doClose() {
  // doClose is called by the inner TeeController to signal that the inner side is closed.
  closePending = true;
}

void ReadableStreamJsTeeController::drain(kj::Maybe<v8::Local<v8::Value>> maybeReason) {
  KJ_IF_MAYBE(reason, maybeReason) {
    while (!readRequests.empty()) {
      auto request = kj::mv(readRequests.front());
      readRequests.pop_front();
      request.reject(*reason);
    }
    return;
  }
  while (!readRequests.empty()) {
    auto request = kj::mv(readRequests.front());
    readRequests.pop_front();
    request.resolve({ .done = true });
  }
}

void ReadableStreamJsTeeController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  // doError is called by the inner TeeController to signal that the inner side has
  // errored. This outer controller must detach itself, clear the queue, and transition
  // itself into the errored state as well.
  detach(js);
  state.init<StreamStates::Errored>(js.v8Ref(reason));
  queue.clear();

  drain(reason);

  KJ_SWITCH_ONEOF(lock.state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      maybeRejectPromise<void>(locked.getClosedFulfiller(), reason);
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      lock.state.init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::TeeLocked) {
      // This state is unreachable because the TeeLocked state is not
      // used by ReadableStreamJsTeeController.
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(locked, Unlocked) {}
    KJ_CASE_ONEOF(locked, Locked) {}
  }
}

void ReadableStreamJsTeeController::finishClosing(jsg::Lock& js) {
  detach(js);
  state.init<StreamStates::Closed>();

  drain(nullptr);

  KJ_SWITCH_ONEOF(lock.state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      maybeResolvePromise(locked.getClosedFulfiller());
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      lock.state.init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::TeeLocked) {
      // This state is unreachable because the TeeLocked state is not
      // used by ReadableStreamJsTeeController.
      KJ_UNREACHABLE;
    }
    KJ_CASE_ONEOF(locked, Unlocked) {}
    KJ_CASE_ONEOF(locked, Locked) {}
  }
}

void ReadableStreamJsTeeController::handleData(jsg::Lock& js, ReadResult result) {
  // handleData is called by the inner TeeController when data has been ready from the underlying
  // source. If there are pending read requests, fulfill the first one immediately, otherwise
  // push the item on the queue.
  if (!readRequests.empty()) {
    KJ_ASSERT(queue.empty());
    auto request = kj::mv(readRequests.front());
    readRequests.pop_front();
    request.resolve(kj::mv(result));

    // If the innerState has been detached and there are no further read requests,
    // transition into the closed state.
    if (closePending) {
      finishClosing(js);
    }

    return;
  }
  queue.push_back(kj::mv(result));
}

bool ReadableStreamJsTeeController::hasPendingReadRequests() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      KJ_ASSERT(readRequests.empty());
      return false;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      KJ_ASSERT(readRequests.empty());
      return false;
    }
    KJ_CASE_ONEOF(readable, Readable) {
      return !readRequests.empty();
    }
  }
  KJ_UNREACHABLE;
}

bool ReadableStreamJsTeeController::isByteOriented() const { return false; };

bool ReadableStreamJsTeeController::isDisturbed() { return disturbed; }

bool ReadableStreamJsTeeController::isLockedToReader() const {
  return lock.isLockedToReader();
}

bool ReadableStreamJsTeeController::isClosedOrErrored() const {
  return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
}

bool ReadableStreamJsTeeController::lockReader(jsg::Lock& js, Reader& reader) {
  return lock.lockReader(js, *this, reader);
}

jsg::Promise<void> ReadableStreamJsTeeController::pipeTo(
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
      js.v8TypeError("This ReadableStream cannot be piped to this WritableStream."_kj));
}

kj::Maybe<jsg::Promise<ReadResult>> ReadableStreamJsTeeController::read(
    jsg::Lock& js,
    kj::Maybe<ByobOptions> byobOptions) {
  disturbed = true;
  // Per the streams specification, ReadableStream tee branches do not support BYOB reads.
  // The byobOptions should never be set here, but let's make sure.
  KJ_ASSERT(byobOptions == nullptr);

  if (state.is<StreamStates::Closed>()) {
    KJ_ASSERT(queue.empty());
    return js.resolvedPromise(ReadResult { .done = true });
  }

  KJ_IF_MAYBE(errored, state.tryGet<StreamStates::Errored>()) {
    KJ_ASSERT(queue.empty());
    return js.rejectedPromise<ReadResult>(errored->addRef(js));
  }

  // Every tee controller has its own internal queue.
  // If that internal queue is not empty, read will pull from it,
  // otherwise, the read request will be queued and the underlying tee controller
  // will be asked to pull data. When the controller does pull data, it will be
  // delivered to every branch. If the branch queue is not empty, or there
  // are no pending reads, the data will be appended into the tee controller's
  // queue. If there are pending reads, the queue should be empty and the
  // next pending read will be fulfilled.

  // First, let's check the internal queue. If there's data, we can resolve
  // the read promise immediately.
  if (!queue.empty()) {
    // The tee controller queue will only ever have value items.
    auto item = kj::mv(queue.front());
    queue.pop_front();

    // If the innerState has been detached and there are no further read requests,
    // transition into the closed state.
    if (innerState == nullptr && readRequests.empty()) {
      finishClosing(js);
    }

    return js.resolvedPromise(kj::mv(item));
  }

  auto& controller = KJ_ASSERT_NONNULL(innerState).controller;
  auto readRequest = js.newPromiseAndResolver<ReadResult>();
  readRequests.push_back(kj::mv(readRequest.resolver));
  controller.ensurePulling(js);
  return kj::mv(readRequest.promise);
}

void ReadableStreamJsTeeController::releaseReader(Reader& reader, kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseReader(*this, reader, maybeJs);
}

kj::Maybe<kj::Own<ReadableStreamSource>>
ReadableStreamJsTeeController::removeSource(jsg::Lock& js) {
  JSG_REQUIRE(!isLockedToReader(), TypeError, "This ReadableStream is locked to a reader.");

  lock.state.init<Locked>();
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::refcounted<ReadableStreamJsTeeSource>(StreamStates::Closed());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      kj::throwFatalException(js.exceptionToKj(errored.addRef(js)));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      // It is possible that the tee controller queue already has data in it that data needs to
      // be moved over into the ReadableStreamJsTeeSource we are going to create. However, we
      // have to make sure that it's all byte data, otherwise we need to error. Also, to make
      // reading that data as efficient as possible in the source, we copy it into a queue rather
      // than keeping it as individual ReadResult objects.
      std::deque<kj::byte> bytes;
      while (!queue.empty()) {
        auto& item = queue.front();
        KJ_IF_MAYBE(value, item.value) {
          auto view = value->getHandle(js);
          JSG_REQUIRE(view->IsArrayBufferView() || view->IsArrayBuffer(), TypeError,
                       "This ReadableStream does not contain bytes.");
          jsg::BufferSource source(js, view);
          auto ptr = source.asArrayPtr();
          std::copy(ptr.begin(), ptr.end(), std::back_inserter(bytes));
          queue.pop_front();
          continue;
        }
        if (item.done) {
          break;
        }
      }

      KJ_DEFER(state.init<StreamStates::Closed>());
      auto& inner = KJ_ASSERT_NONNULL(innerState);
      auto& controller = inner.controller;
      auto ref = inner.ref.addRef();
      detach(js);
      return kj::refcounted<ReadableStreamJsTeeSource>(kj::mv(ref), controller, kj::mv(bytes));
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsTeeController::setOwnerRef(ReadableStream& owner) {
  this->owner = owner;
  KJ_ASSERT_NONNULL(innerState).controller.addBranch(this);
}

void ReadableStreamJsTeeController::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(lock);
  KJ_IF_MAYBE(error, state.tryGet<StreamStates::Errored>()) {
    visitor.visit(*error);
  }
  for (auto& item : queue) {
    visitor.visit(item);
  }
  visitor.visitAll(readRequests);
}

ReadableStreamController::Tee ReadableStreamJsTeeController::tee(jsg::Lock& js) {
  JSG_REQUIRE(!isLockedToReader(), TypeError,
               "This ReadableStream is currently locked to a reader.");
  disturbed = true;
  lock.state.init<Locked>();

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(ReadableStreamJsController(closed)),
        .branch2 = jsg::alloc<ReadableStream>(ReadableStreamJsController(closed)),
      };
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(ReadableStreamJsController(errored.addRef(js))),
        .branch2 = jsg::alloc<ReadableStream>(ReadableStreamJsController(errored.addRef(js))),
      };
    }
    KJ_CASE_ONEOF(readable, Readable) {
      if (closePending && queue.empty()) {
          finishClosing(js);
          return Tee {
            .branch1 = jsg::alloc<ReadableStream>(
                ReadableStreamJsController(StreamStates::Closed())),
            .branch2 = jsg::alloc<ReadableStream>(
                ReadableStreamJsController(StreamStates::Closed())),
          };
      }

      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(
            ReadableStreamJsTeeController(js,
                innerState.map([](Attached& attached) -> Attached {
                  return Attached(attached.ref->addRef(), attached.controller);
                }),
                queue)),
        .branch2 = jsg::alloc<ReadableStream>(
            ReadableStreamJsTeeController(js,
                innerState.map([](Attached& attached) -> Attached {
                  return Attached(attached.ref->addRef(), attached.controller);
                }),
                queue)),
      };
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<ReadableStreamController::PipeController&> ReadableStreamJsTeeController::tryPipeLock(
    jsg::Ref<WritableStream> destination) {
  return lock.tryPipeLock(*this, kj::mv(destination));
}

// ======================================================================================

ReadableStreamJsController::ReadableStreamJsController() {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Closed closed)
    : state(closed) {}

ReadableStreamJsController::ReadableStreamJsController(StreamStates::Errored errored)
    : state(kj::mv(errored)) {}

jsg::Ref<ReadableStream> ReadableStreamJsController::addRef() {
  return KJ_REQUIRE_NONNULL(owner).addRef();
}

jsg::Promise<void> ReadableStreamJsController::cancel(
    jsg::Lock& js,
    jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  disturbed = true;

  auto reason = js.v8Ref(maybeReason.orDefault(js.v8Undefined()));

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise();
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<void>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(controller, ByobController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(byteReadable, kj::Own<ByteReadable>) {
//   return byteReadable->cancel(js, kj::mv(maybeReason));

      return controller->cancel(js, reason.getHandle(js));
    }
    KJ_CASE_ONEOF(controller, DefaultController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
//   return valueReadable->cancel(js, kj::mv(maybeReason));

      return controller->cancel(js, reason.getHandle(js));
    }
  }

  KJ_UNREACHABLE;
}

void ReadableStreamJsController::doCancel(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Likely not necessary to implement this with the new model?
// This is intended to allow completion of canceling the controller which will be
// done via the ByteReadable or ValueReadable when those are canceled.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return;
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return;
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->doCancel(js, reason);
    }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->doCancel(js, reason);
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::detachFromController() {
// TODO(stream-tee): With the new model, the ValueReadable or ByteReadable struct
// will become the new owner of the relationship with the underlying controller.
// There will no longer be a single owner known to the controller, only multiple
// consumers. This function likely goes away entirely in favor of some mechanism
// on the ValueReadable/ByteReadable.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {}
    KJ_CASE_ONEOF(controller, DefaultController) {
      controller->setOwner(nullptr);
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      controller->setOwner(nullptr);
    }
  }
}

void ReadableStreamJsController::doClose() {
// TODO(stream-tee): doClose() finalizes the closed state of this ReadableStream.
// The connection to the underlying controller is released with no further action.
// Importantly, this method is triggered by the underlying controller as a result
// of that controller closing or being canceled. We detach ourselves from the
// underlying controller by releasing the ValueReadable or ByteReadable in the
// state and changing that to closed. We also clean up other state here.
// Since the underlying controller will no longer have a single owner, we will
// need to modify things such this signal is triggered by the ValueReadable or
// ByteReadable.
  detachFromController();
  state.init<StreamStates::Closed>();

  KJ_SWITCH_ONEOF(lock.state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      maybeResolvePromise(locked.getClosedFulfiller());
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      lock.state.init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::TeeLocked) {
      locked.close();
    }
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, Unlocked) {}
  }
}

void ReadableStreamJsController::controllerClose(jsg::Lock& js) {
// TODO(stream-tee): This is obsolete and no longer needed.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->close(js);
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->close(js);
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::controllerError(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
// TODO(stream-tee): This is obsolete and no longer needed.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->error(js, reason);
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->error(js, reason);
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): As with doClose(), doError() finalizes the error state of this
// ReadableStream. The connection to the underlying controller is released with no
// further action. This method is triggered by the underlying controller as a result
// of that controller erroring or being canceled. We detach ourselves from the
// underlying controller by releasing the ValueReadable or ByteReadable in the state
// and changing that to errored. We also clean up other state here.
// Since the underlying controller will no longer have a single owner, we will
// need to modify things such that this signal is triggered by the ValueReadable or
// ByteReadable.
  detachFromController();
  state.init<StreamStates::Errored>(js.v8Ref(reason));

  KJ_SWITCH_ONEOF(lock.state) {
    KJ_CASE_ONEOF(locked, ReaderLocked) {
      maybeRejectPromise<void>(locked.getClosedFulfiller(), reason);
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::PipeLocked) {
      lock.state.init<Unlocked>();
    }
    KJ_CASE_ONEOF(locked, ReadableLockImpl::TeeLocked) {
      locked.error(js, reason);
    }
    KJ_CASE_ONEOF(locked, Locked) {}
    KJ_CASE_ONEOF(locked, Unlocked) {}
  }
}

bool ReadableStreamJsController::hasPendingReadRequests() {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return false; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return false; }
    KJ_CASE_ONEOF(controller, DefaultController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
//   return valueReadable->consumer->hasPendingReadRequests();

      return controller->hasPendingReadRequests();
    }
    KJ_CASE_ONEOF(controller, ByobController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(byteReadable, kj::Own<ByteReadable>) {
//   return byteReadable->consumer->hasPendingReadRequests();

      return controller->hasPendingReadRequests();
    }
  }
  KJ_UNREACHABLE;
}

bool ReadableStreamJsController::isByteOriented() const {
// TODO(stream-tee):
// return state.is<kj::Own<ByteReadable>>();
  return state.is<ByobController>();
}

bool ReadableStreamJsController::isClosedOrErrored() const {
  return state.is<StreamStates::Closed>() || state.is<StreamStates::Errored>();
}

bool ReadableStreamJsController::isDisturbed() { return disturbed; }

bool ReadableStreamJsController::isLocked() const { return isLockedToReader(); }

bool ReadableStreamJsController::isLockedReaderByteOriented() {
  KJ_IF_MAYBE(locked, lock.state.tryGet<ReaderLocked>()) {
    return locked->getReader().isByteOriented();
  }
  return false;
}

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
      return js.resolvedPromise(ReadResult { .done = true });
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      return js.rejectedPromise<ReadResult>(errored.addRef(js));
    }
    KJ_CASE_ONEOF(controller, DefaultController) {

// TODO(stream-tee): Read for both ValueReadable and ByteReadable must be updated
// in terms of, e.g. valueReadable->consumer->read(...)

      // The ReadableStreamDefaultController does not support ByobOptions.
      // It should never happen, but let's make sure.
      KJ_ASSERT(maybeByobOptions == nullptr);
      return controller->read(js);
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->read(js, kj::mv(maybeByobOptions));
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::releaseReader(
    Reader& reader,
    kj::Maybe<jsg::Lock&> maybeJs) {
  lock.releaseReader(*this, reader, maybeJs);
}

kj::Maybe<kj::Own<ReadableStreamSource>>
ReadableStreamJsController::removeSource(jsg::Lock& js) {
  JSG_REQUIRE(!isLockedToReader(), TypeError, "This ReadableStream is locked to a reader.");

  lock.state.init<Locked>();
  disturbed = true;
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return kj::refcounted<ReadableStreamJsSource>(StreamStates::Closed());
    }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) {
      kj::throwFatalException(js.exceptionToKj(errored.addRef(js)));
    }
    KJ_CASE_ONEOF(controller, ByobController) {

// TODO(stream-tee): Removing the source here becomes as simple as transferring the
// ValueReadable and ByteReadable from the ReadableStreamJsController to the
// ReadableStreamJsSource that we're creating. All of the relevant state transfers
// with it. We must, however, ensure that there is still a way for that to communicate
// state changes (e.g. closed, errored) with whichever thing is currently owning it.

      KJ_DEFER(state.init<StreamStates::Closed>());
      return kj::refcounted<ReadableStreamJsSource>(kj::mv(controller));
    }
    KJ_CASE_ONEOF(controller, DefaultController) {
      KJ_DEFER(state.init<StreamStates::Closed>());
      return kj::refcounted<ReadableStreamJsSource>(kj::mv(controller));
    }
  }
  KJ_UNREACHABLE;
}

ReadableStreamController::Tee ReadableStreamJsController::tee(jsg::Lock& js) {
// TODO(stream-tee): Here, rather than creating new ReadableStreamJsTeeController-backed things,
// we are going to just create new ReadableStreamJsController things that have their own
// clones of this streams ValueReadable or ByteReadable.
  KJ_IF_MAYBE(teeController, lock.tryTeeLock(*this)) {
    disturbed = true;

    if (state.is<StreamStates::Closed>()) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(ReadableStreamJsController(StreamStates::Closed())),
        .branch2 = jsg::alloc<ReadableStream>(ReadableStreamJsController(StreamStates::Closed())),
      };
    }

    KJ_IF_MAYBE(errored, state.tryGet<StreamStates::Errored>()) {
      return Tee {
        .branch1 = jsg::alloc<ReadableStream>(ReadableStreamJsController(errored->addRef(js))),
        .branch2 = jsg::alloc<ReadableStream>(ReadableStreamJsController(errored->addRef(js))),
      };
    }

    return Tee {
      .branch1 = jsg::alloc<ReadableStream>(
          ReadableStreamJsTeeController(addRef(), *teeController)),
      .branch2 = jsg::alloc<ReadableStream>(
          ReadableStreamJsTeeController(addRef(), *teeController)),
    };
  }
  JSG_FAIL_REQUIRE(TypeError, "This ReadableStream is currently locked to a reader.");
}

void ReadableStreamJsController::setOwnerRef(ReadableStream& stream) {
  KJ_ASSERT(owner == nullptr);
  owner = stream;
}

void ReadableStreamJsController::setup(
    jsg::Lock& js,
    jsg::Optional<UnderlyingSource> maybeUnderlyingSource,
    jsg::Optional<StreamQueuingStrategy> maybeQueuingStrategy) {
  auto underlyingSource = kj::mv(maybeUnderlyingSource).orDefault({});
  auto queuingStrategy = kj::mv(maybeQueuingStrategy).orDefault({});
  auto type = underlyingSource.type.map([](kj::StringPtr s) { return s; }).orDefault(""_kj);

  maybeTransformer = kj::mv(underlyingSource.maybeTransformer);

// TODO(stream-tee): Here, we wrap create the underlying controllers but we need to
// create the ValueReadable or ByteReadable and use that for the state instead.

  if (type == "bytes") {
    state = jsg::alloc<ReadableByteStreamController>(*this);
    state.get<ByobController>()->setup(
        js,
        kj::mv(underlyingSource),
        kj::mv(queuingStrategy));
  } else {
    JSG_REQUIRE(type == "",
                 TypeError,
                 kj::str("\"", type, "\" is not a valid type of ReadableStream."));
    state = jsg::alloc<ReadableStreamDefaultController>(*this);
    state.get<DefaultController>()->setup(
        js,
        kj::mv(underlyingSource),
        kj::mv(queuingStrategy));
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
    KJ_CASE_ONEOF(controller, DefaultController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(valueReadable, kj::Own<ValueReadable>) {
//   visitor.visit(*valueReadable);

      visitor.visit(controller);
    }
    KJ_CASE_ONEOF(controller, ByobController) {

// TODO(stream-tee):
// KJ_CASE_ONEOF(byteReadable, kj::Own<byteReadable>) {
//   visitor.visit(*byteReadable);

      visitor.visit(controller);
    }
  }
  visitor.visit(lock, maybeTransformer);
};

kj::Maybe<int> ReadableStreamJsController::getDesiredSize() {
// TODO(stream-tee): This is used by the TransformStream implementation. This needs to be
// implemented in terms of the underlying controllers backpressure, such that even if there
// is no data waiting in this particular stream's buffer, backpressure is still being signalled
// correctly through every consumer branch.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return nullptr; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return nullptr; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->getDesiredSize();
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->getDesiredSize();
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
// TODO(stream-tee): This is used by the TransformStream implementation. The implementation
// here won't need to change much.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return false; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return false; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->canCloseOrEnqueue();
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->canCloseOrEnqueue();
    }
  }
  KJ_UNREACHABLE;
}

bool ReadableStreamJsController::hasBackpressure() {
// TODO(stream-tee): This is used by the TransformStream implementation. Need to determine
// however if we can get rid of this and just use negative or zero desiredSize.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return false; }
    KJ_CASE_ONEOF(errored, StreamStates::Errored) { return false; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->hasBackpressure();
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      return controller->hasBackpressure();
    }
  }
  KJ_UNREACHABLE;
}

void ReadableStreamJsController::defaultControllerEnqueue(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
// TODO(stream-tee): Is this necessary still?
  auto& controller = KJ_ASSERT_NONNULL(state.tryGet<DefaultController>(),
      "defaultControllerEnqueue() can only be called with a ReadableStreamDefaultController");
  controller->doEnqueue(js, chunk);
}

// ======================================================================================

void ReadableStreamJsSource::cancel(kj::Exception reason) {
  const auto doCancel = [this](auto& controller, auto reason) {
    JSG_REQUIRE(!canceling, TypeError, "The stream has already been canceled.");
    canceling = true;

// TODO(stream-tee): The change here will echo what is happening over in JsController.
// Specifically, the JsSource will have either a ValueReadable or ByteReadable that
// is one of several owning a reference to the underlying controller. When the last
// one canceled is the one that actually triggers the underlying controller cancel.

    ioContext.addTask(ioContext.run([this, &controller, reason = kj::mv(reason)]
                                              (Worker::Lock& lock) mutable -> kj::Promise<void> {
      detachFromController();
      jsg::Lock& js = lock;
      state.init<kj::Exception>(kj::cp(reason));
      v8::HandleScope handleScope(js.v8Isolate);
      return ioContext.awaitJs(
          controller->cancel(js, js.exceptionToJs(kj::cp(reason)).getHandle(js)).then(js,
              [this](jsg::Lock& js) { canceling = false; },
              [this](jsg::Lock& js, jsg::Value reason) {
        canceling = false;
        js.throwException(kj::mv(reason));
      }));
    }).attach(controller.addRef(), kj::addRef(*this)));
  };

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return; }
    KJ_CASE_ONEOF(errored, kj::Exception) { kj::throwFatalException(kj::cp(errored)); }
    KJ_CASE_ONEOF(controller, ByobController) { doCancel(controller, kj::mv(reason)); }
    KJ_CASE_ONEOF(controller, DefaultController) { doCancel(controller, kj::mv(reason)); }
  }
}

void ReadableStreamJsSource::detachFromController() {
// TODO(stream-tee): Since it will be the ValueReadable or ByteReadable that deals
// with this now, hopefully we can remove this.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
    KJ_CASE_ONEOF(errored, kj::Exception) {}
    KJ_CASE_ONEOF(controller, DefaultController) {
      controller->setOwner(nullptr);
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      controller->setOwner(nullptr);
    }
  }
}

void ReadableStreamJsSource::doClose() {
// TODO(stream-tee): Similar change here as JsController::doClose
  detachFromController();
  state.init<StreamStates::Closed>();
}

void ReadableStreamJsSource::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
// TODO(stream-tee): Similar change here as JsController::doError
  detachFromController();
  state.init<kj::Exception>(js.exceptionToKj(js.v8Ref(reason)));
}

bool ReadableStreamJsSource::isLocked() const { return true; }

bool ReadableStreamJsSource::isLockedReaderByteOriented() { return true; }

// TODO(stream-tee): The read mechanisms below must be updated in terms of using
// the ValueReadable or ByteReadable.

jsg::Promise<size_t> ReadableStreamJsSource::readFromByobController(
    jsg::Lock& js,
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  // This should go without saying, but let's be safe.
  KJ_ASSERT(maxBytes >= minBytes);
  KJ_ASSERT(minBytes >= 1);

  // Unfortunately, we don't own the buffer here, and if the kj::Promise for the read
  // is canceled while the promise is still pending, there's a chance that the
  // destination buffer will disappear on us leading to a use after free and Bad Things.
  // To address this, we create a BackingStore of the same size and read into that,
  // then in the kj::Promise continuation we copy over. If the promise gets canceled,
  // that continuation will never happen so we should be safe.

  std::shared_ptr<v8::BackingStore> backing =
      v8::ArrayBuffer::NewBackingStore(js.v8Isolate, maxBytes);
  auto view = v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, backing), 0, maxBytes);
  auto byobOptions = ReadableStreamController::ByobOptions {
    .bufferView = js.v8Ref(view.As<v8::ArrayBufferView>()),
    .byteOffset = 0,
    .byteLength = maxBytes,
    .atLeast = minBytes,
  };

  auto& controller = KJ_ASSERT_NONNULL(state.tryGet<ByobController>());

  return controller->read(js, kj::mv(byobOptions))
      .then(js, [this, buffer, maxBytes, minBytes]
            (jsg::Lock& js, ReadResult result) mutable -> jsg::Promise<size_t> {
    size_t byteLength = 0;
    KJ_IF_MAYBE(value, result.value) {
      jsg::BufferSource source(js, value->getHandle(js));
      KJ_ASSERT(source.size() <= maxBytes);
      byteLength = source.size();
      memcpy(reinterpret_cast<kj::byte*>(buffer), source.asArrayPtr().begin(), byteLength);
    }
    if (result.done) {
      doClose();
    } else if (byteLength < minBytes) {
      // If byteLength is less than minBytes and we're not done, we should do another read in
      // order to fulfill the minBytes contract. When doing so, we adjust the buffer pointer up
      // by byteLength and reduce both the minBytes and maxBytes by byteLength. Ideally this
      // isn't necessary. The ReadableStream controller should be paying attention to the
      // atLeast property and should fill the buffer to at least that number of bytes,
      // unfortunately we can't really trust that it will do so since the atLeast property is
      // an optional non-standard extension and some ReadableStreams may just be poorly behaved.
      return readFromByobController(
          js,
          reinterpret_cast<kj::byte*>(buffer) + byteLength,
          minBytes - byteLength,
          maxBytes - byteLength);
    }
    return js.resolvedPromise(kj::cp(byteLength));
  }, [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<size_t> {
    detachFromController();
    state.init<kj::Exception>(js.exceptionToKj(reason.addRef(js)));
    js.throwException(kj::mv(reason));
  });
}

jsg::Promise<size_t> ReadableStreamJsSource::readFromDefaultController(
    jsg::Lock& js,
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  // This should go without saying, but let's be safe.
  KJ_ASSERT(maxBytes >= minBytes);
  KJ_ASSERT(minBytes >= 1);

  auto bytes = static_cast<kj::byte*>(buffer);
  auto ptr = kj::ArrayPtr<kj::byte>(bytes, maxBytes);

  if (queue.size() >= minBytes) {
    // Good news, we can fulfill the minimum requirements of this tryRead
    // synchronously from the queue.
    auto bytesToCopy = kj::min(maxBytes, queue.size());
    std::copy(queue.begin(),
              queue.begin() + bytesToCopy,
              ptr.begin());
    queue.erase(queue.begin(), queue.begin() + bytesToCopy);
    return js.resolvedPromise(kj::cp(bytesToCopy));
  }

  auto bytesToCopy = queue.size();
  // If there are at least some bytes in the queue! Let's copy what we
  // can into the buffer, we'll try pulling the rest from the controller.
  if (bytesToCopy > 0) {
    // This should be true because if it wasn't we would have caught it above.
    KJ_ASSERT(bytesToCopy < minBytes);
    std::copy(queue.begin(),
              queue.begin() + bytesToCopy,
              ptr.begin());
    queue.clear();
    bytes += bytesToCopy;
    minBytes -= bytesToCopy;
    maxBytes -= bytesToCopy;
    // This should continue to hold true, otherwise we need to relearn math.
    KJ_ASSERT(minBytes >= 1);
  }

  return readLoop(js, bytes, minBytes, maxBytes, bytesToCopy);
}

jsg::Promise<size_t> ReadableStreamJsSource::readLoop(
    jsg::Lock& js,
    kj::byte* bytes,
    size_t minBytes,
    size_t maxBytes,
    size_t amount) {
  // At the start of each iteration, we have to make sure that we're still in the expected
  // state. If we're closed, just return amount. If we're errored, reject the promise. If we're
  // using a ByobController, we shouldn't be here at all.
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      return js.resolvedPromise<size_t>(0);
    }
    KJ_CASE_ONEOF(errored, kj::Exception) {
      return js.rejectedPromise<size_t>(js.exceptionToJs(kj::cp(errored)));
    }
    KJ_CASE_ONEOF(controller, ByobController) { KJ_UNREACHABLE; }
    KJ_CASE_ONEOF(controller, DefaultController) {
      return controller->read(js).then(js,
          [this, bytes, minBytes, maxBytes, amount]
          (jsg::Lock& js, ReadResult result) mutable -> jsg::Promise<size_t> {

        // First, let's check if result.done is true. If it is, then result.value
        // should be undefined and we'll be done.
        if (result.done) {
          doClose();
          KJ_ASSERT(result.value == nullptr);
          return js.resolvedPromise(kj::cp(amount));
        }

        // Next, let's check if result.value is something we can reasonably
        // interpret as bytes. If not, we'll reject the promise.
        auto handle = KJ_ASSERT_NONNULL(result.value).getHandle(js);
        if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
          return js.rejectedPromise<size_t>(
              js.v8TypeError("This ReadableStream did not return bytes."_kj));
        }

        jsg::BufferSource bufferSource(js, handle);
        auto ptr = bufferSource.asArrayPtr();

        // If byteLength > maxBytes, we copy maxBytes into the destination bytes buffer,
        // increment amount by maxBytes, push the remaining bytes onto the queue, and
        // return amount.
        if (bufferSource.size() > maxBytes) {
          memcpy(bytes, ptr.begin(), maxBytes);
          std::copy(ptr.begin() + maxBytes, ptr.end(), std::back_inserter(queue));
          amount += maxBytes;
          return js.resolvedPromise(kj::cp(amount));
        }

        KJ_ASSERT(bufferSource.size() <= maxBytes);
        memcpy(bytes, ptr.begin(), bufferSource.size());
        amount += bufferSource.size();

        // We've met the minimum requirements! Go ahead and return. The worst case
        // here is that minBytes is 1 and the stream is serving only a single byte
        // with each pull. That's terrible, but it beats us having to do another
        // pull.
        if (amount >= minBytes) {
          return js.resolvedPromise(kj::cp(amount));
        }

        // Unfortunately, we're not done. Pull again!
        minBytes -= bufferSource.size();
        maxBytes -= bufferSource.size();

        return readLoop(js, bytes, minBytes, maxBytes, amount);
      }, [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<size_t> {
        detachFromController();
        state.init<kj::Exception>(js.exceptionToKj(reason.addRef(js)));
        js.throwException(kj::mv(reason));
      });
    }
  }
  KJ_UNREACHABLE;
}

kj::Promise<size_t> ReadableStreamJsSource::tryRead(
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  return ioContext.run([this, buffer, minBytes, maxBytes](Worker::Lock& lock)
      -> kj::Promise<size_t> {
    jsg::Lock& js = lock;
    // Of particular note here: Notice that we attach a reference to this and the controller
    // if it exists. This is to ensure that both the kj and js heap objects are live until
    // the promise resolves.
    auto promise = ioContext.awaitJs(internalTryRead(js, buffer, minBytes, maxBytes))
        .attach(kj::addRef((*this)));

    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(errored, kj::Exception) {}
      KJ_CASE_ONEOF(controller, DefaultController) {
        promise = promise.attach(controller.addRef());
      }
      KJ_CASE_ONEOF(controller, ByobController) {
        promise = promise.attach(controller.addRef());
      }
    }

    return kj::mv(promise);
  });
}

jsg::Promise<size_t> ReadableStreamJsSource::internalTryRead(
    jsg::Lock& js,
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      if (queue.size() > 0) {
        // There's still data in the queue. Copy it out until the queue is empty.
        auto bytesToCopy = kj::min(maxBytes, queue.size());
        auto ptr = kj::ArrayPtr<kj::byte>(static_cast<kj::byte*>(buffer), bytesToCopy);
        std::copy(queue.begin(),
                  queue.begin() + bytesToCopy,
                  ptr.begin());
        queue.erase(queue.begin(), queue.begin() + bytesToCopy);
        return js.resolvedPromise(kj::cp(bytesToCopy));
      }
      return js.resolvedPromise((size_t)0);
    }
    KJ_CASE_ONEOF(errored, kj::Exception) {
      return js.rejectedPromise<size_t>(js.exceptionToJs(kj::cp(errored)));
    }
    KJ_CASE_ONEOF(controller, DefaultController) {
      JSG_REQUIRE(!readPending, TypeError, "There is already a read pending.");
      readPending = true;

      return readFromDefaultController(js, buffer, minBytes, maxBytes)
          .then(js, [this](jsg::Lock& js, size_t amount) -> size_t {
        readPending = false;
        return amount;
      }, [this](jsg::Lock& js, jsg::Value reason) -> size_t {
        readPending = false;
        js.throwException(kj::mv(reason));
      });
    }
    KJ_CASE_ONEOF(controller, ByobController) {
      JSG_REQUIRE(!readPending, TypeError, "There is already a read pending.");
      readPending = true;

      return readFromByobController(js, buffer, minBytes, maxBytes)
          .then(js, [this](jsg::Lock& js, size_t amount) -> size_t {
        readPending = false;
        return amount;
      }, [this](jsg::Lock& js, jsg::Value reason) -> size_t {
        readPending = false;
        js.throwException(kj::mv(reason));
      });
    }
  }
  KJ_UNREACHABLE;
}

kj::Promise<DeferredProxy<void>> ReadableStreamJsSource::pumpTo(
    WritableStreamSink& output, bool end) {
  // Here, the IoContext has to remain live throughout the entire
  // pipe operation, so our deferred proxy will be a non-op.
  return addNoopDeferredProxy(ioContext.run([this, &output, end](Worker::Lock& lock) {
    jsg::Lock& js = lock;
    // Of particular note here: Notice that we attach a reference to this and the controller
    // if it exists. This is to ensure that both the kj and js heap objects are live until
    // the promise resolves.
    auto promise = ioContext.awaitJs(pipeLoop(js, output, end, kj::heapArray<kj::byte>(4096)))
        .attach(kj::addRef(*this));

    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, StreamStates::Closed) {}
      KJ_CASE_ONEOF(errored, kj::Exception) {}
      KJ_CASE_ONEOF(controller, ByobController) {
        promise = promise.attach(controller.addRef());
      }
      KJ_CASE_ONEOF(controller, DefaultController) {
        promise = promise.attach(controller.addRef());
      }
    }

    return kj::mv(promise);
  }));
}

jsg::Promise<void> ReadableStreamJsSource::pipeLoop(
    jsg::Lock& js,
    WritableStreamSink& output,
    bool end,
    kj::Array<kj::byte> bytes) {
  return internalTryRead(js, bytes.begin(), 1, bytes.size())
      .then(js, [this, &output, end, bytes = kj::mv(bytes)]
            (jsg::Lock& js, size_t amount) mutable {
    // Although we have a captured reference to the ioContext already,
    // we should not assume that it is still valid here. Let's just grab
    // IoContext::current() to move things along.
    auto& ioContext = IoContext::current();
    if (amount == 0) {
      return end ?
          ioContext.awaitIo(output.end(), []() {}) :
          js.resolvedPromise();
    }
    return ioContext.awaitIo(js, output.write(bytes.begin(), amount),
        [this, &output, end, bytes = kj::mv(bytes)] (jsg::Lock& js) mutable {
      return pipeLoop(js, output, end, kj::mv(bytes));
    });
  });
}

// ======================================================================================

// TODO(stream-tee): Everything ReadableStreamJsTeeSource goes away.

void ReadableStreamJsTeeSource::cancel(kj::Exception reason) {
  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) { return; }
    KJ_CASE_ONEOF(errored, kj::Exception) { kj::throwFatalException(kj::cp(errored)); }
    KJ_CASE_ONEOF(readable, Readable) {
      // For the tee adapter, the only thing we need to do here is
      // reject and clear our own pending read, which is handled by
      // when calling detach with an exception. The tee adapter will
      // handle cleaning up the underlying controller when necessary
      // to do so.
      ioContext.addTask(ioContext.run(
          [this, reason = kj::mv(reason)](Worker::Lock&) {
        detach(kj::mv(reason), nullptr);
        KJ_ASSERT(pendingRead == nullptr);
      }));
    }
  }
}

void ReadableStreamJsTeeSource::detach(
    kj::Maybe<kj::Exception> maybeException,
    kj::Maybe<jsg::Lock&> maybeJs) {
  KJ_IF_MAYBE(controller, teeController) {
    controller->removeBranch(this, maybeJs);
    teeController = nullptr;
  }
  KJ_IF_MAYBE(exception, maybeException) {
    KJ_IF_MAYBE(js, maybeJs) {
      KJ_IF_MAYBE(read, pendingRead) {
        read->resolver.reject(js->exceptionToJs(kj::cp(*exception)).getHandle(js->v8Isolate));
        pendingRead = nullptr;
      }
    }
    state.init<kj::Exception>(kj::mv(*exception));
  } else {
    // When maybeJs is nullptr, we are detaching while there is no isolate lock held.
    // We only want to resolve the read promise and clear the pendingRead while we
    // are within the isolate lock.
    if (maybeJs != nullptr) {
      KJ_IF_MAYBE(read, pendingRead) {
        read->resolver.resolve(0);
        pendingRead = nullptr;
      }
    }
    state.init<StreamStates::Closed>();
  }
}

void ReadableStreamJsTeeSource::doClose() {
  KJ_IF_MAYBE(read, pendingRead) {
    read->resolver.resolve(0);
    pendingRead = nullptr;
  }
  state.init<StreamStates::Closed>();
}

void ReadableStreamJsTeeSource::doError(jsg::Lock& js, v8::Local<v8::Value> reason) {
  detach(js.exceptionToKj(js.v8Ref(reason)), js);
}

void ReadableStreamJsTeeSource::handleData(jsg::Lock& js, ReadResult result) {
  KJ_IF_MAYBE(read, pendingRead) {
    // Make sure the pendingRead hasn't been canceled. If it has, we're just going to clear
    // it and buffer the data.
    KJ_IF_MAYBE(value, result.value) {
      auto handle = value->getHandle(js);
      if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
        auto reason = js.v8TypeError("This ReadableStream did not not return bytes."_kj);
        read->resolver.reject(reason);
        detach(js.exceptionToKj(js.v8Ref(reason)), js);
        pendingRead = nullptr;
        return;
      }

      jsg::BufferSource source(js, handle);
      auto ptr = source.asArrayPtr();
      // If we got too much data back, fulfill the remaining read and buffer the
      // rest in the queue.
      if (ptr.size() > read->bytes.size() - read->filled) {
        auto bytesToCopy = read->bytes.size() - read->filled;
        memcpy(read->bytes.begin() + read->filled, ptr.begin(), bytesToCopy);
        std::copy(ptr.begin() + bytesToCopy, ptr.end(), std::back_inserter(queue));
        read->filled += bytesToCopy;
        read->resolver.resolve(kj::cp(read->filled));
        pendingRead = nullptr;
        return;
      }

      // Otherwise, copy what we got into the read.
      KJ_ASSERT(ptr.size() <= read->bytes.size() - read->filled);
      memcpy(read->bytes.begin() + read->filled, ptr.begin(), ptr.size());
      read->filled += ptr.size();

      // If we've filled up to or beyond the minBytes, we're done! Fulfill
      // the promise, clear the pending read, and return.
      if (read->filled >= read->minBytes) {
        read->resolver.resolve(kj::cp(read->filled));
        pendingRead = nullptr;
        return;
      }

      // We have not yet met the minimum byte requirements, so we keep
      // the current pending read in place, adjust the remaining minBytes
      // down and call ensurePulling again.
      read->minBytes -= ptr.size();
      KJ_ASSERT_NONNULL(teeController).ensurePulling(js);
      return;
    }

    KJ_ASSERT(result.done);
    read->resolver.resolve(0);
    pendingRead = nullptr;
  }

  // If there is no waiting pending read, then we're just going to queue the bytes.
  // If bytes were not returned, then transition to an errored state.

  KJ_IF_MAYBE(value, result.value) {
    auto handle = value->getHandle(js);
    if (!handle->IsArrayBufferView() && !handle->IsArrayBuffer()) {
      detach(JSG_KJ_EXCEPTION(FAILED, TypeError, "This ReadableStream did not return bytes."), js);
      return;
    }
    jsg::BufferSource source(js, handle);
    auto ptr = source.asArrayPtr();
    std::copy(ptr.begin(), ptr.end(), std::back_inserter(queue));
    return;
  }

  KJ_ASSERT(result.done);
  detach(nullptr, nullptr);
}

kj::Promise<size_t> ReadableStreamJsTeeSource::tryRead(
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  return ioContext.run([this, buffer, minBytes, maxBytes](Worker::Lock& lock) {
    jsg::Lock& js = lock;
    // Of particular note here: Notice that we attach a reference to this and the controller
    // if it exists. This is to ensure that both the kj and js heap objects are live until
    // the promise resolves.
    auto promise = ioContext.awaitJs(internalTryRead(js, buffer, minBytes, maxBytes))
        .attach(kj::addRef(*this));
    KJ_IF_MAYBE(readable, state.tryGet<Readable>()) {
      promise = promise.attach(readable->addRef());
    }
    return kj::mv(promise);
  });
}

jsg::Promise<size_t> ReadableStreamJsTeeSource::internalTryRead(
    jsg::Lock& js,
    void* buffer,
    size_t minBytes,
    size_t maxBytes) {
  auto bytes = static_cast<kj::byte*>(buffer);
  auto ptr = kj::ArrayPtr<kj::byte>(bytes, maxBytes);

  KJ_SWITCH_ONEOF(state) {
    KJ_CASE_ONEOF(closed, StreamStates::Closed) {
      if (queue.size() > 0) {
        // There's still data in the queue. Copy it out until the queue is empty.
        auto bytesToCopy = kj::min(maxBytes, queue.size());
        std::copy(queue.begin(),
                  queue.begin() + bytesToCopy,
                  ptr.begin());
        queue.erase(queue.begin(), queue.begin() + bytesToCopy);
        return js.resolvedPromise(kj::cp(bytesToCopy));
      }
      return js.resolvedPromise((size_t)0);
    }
    KJ_CASE_ONEOF(errored, kj::Exception) {
      return js.rejectedPromise<size_t>(js.exceptionToJs(kj::cp(errored)));
    }
    KJ_CASE_ONEOF(readable, Readable) {
      if (pendingRead != nullptr) {
        return js.rejectedPromise<size_t>(js.v8TypeError("There is already a read pending."_kj));
      }

      if (queue.size() >= minBytes) {
        // Good news, we can fulfill the minimum requirements of this tryRead
        // synchronously from the queue.
        // If there is any data at all in the queue, this is going to be the
        // most likely path taken since we typically pass minBytes = 1.
        auto bytesToCopy = kj::min(maxBytes, queue.size());
        std::copy(queue.begin(),
                  queue.begin() + bytesToCopy,
                  ptr.begin());
        queue.erase(queue.begin(), queue.begin() + bytesToCopy);
        return js.resolvedPromise(kj::cp(bytesToCopy));
      }

      auto bytesToCopy = queue.size();
      if (bytesToCopy > 0) {
        // This branch is unlikely to be taken unless we pass minBytes > 1.
        // Otherwise, if the queue has any data at all and minBytes =1 ,
        // the above queue.size() >= minBytes path would be taken.
        KJ_ASSERT(bytesToCopy < minBytes);
        std::copy(queue.begin(),
                  queue.begin() + bytesToCopy,
                  ptr.begin());
        queue.clear();
        bytes += bytesToCopy;
        minBytes -= bytesToCopy;
        maxBytes -= bytesToCopy;
        KJ_ASSERT(minBytes >= 1);
      }

      auto prp = js.newPromiseAndResolver<size_t>();
      pendingRead = PendingRead {
        .resolver = kj::mv(prp.resolver),
        .bytes = kj::ArrayPtr(bytes, maxBytes),
        .minBytes = minBytes,
        .filled = bytesToCopy,
      };

      KJ_ASSERT_NONNULL(teeController).ensurePulling(js);

      return prp.promise.catch_(js, [this](jsg::Lock& js, jsg::Value reason) mutable -> size_t {
        state.init<kj::Exception>(js.exceptionToKj(reason.addRef(js)));
        js.throwException(kj::mv(reason));
      });
    }
  }
  KJ_UNREACHABLE;
}

kj::Promise<DeferredProxy<void>> ReadableStreamJsTeeSource::pumpTo(
    WritableStreamSink& output, bool end) {
  // Here, the IoContext has to remain live throughout the entire
  // pipe operation, so our deferred proxy will be a non-op.
  return addNoopDeferredProxy(ioContext.run([this, &output, end](Worker::Lock& lock) {
    jsg::Lock& js = lock;
    // Of particular note here: Notice that we attach a reference to this and the controller
    // if it exists. This is to ensure that both the kj and js heap objects are live until
    // the promise resolves.
    auto promise = ioContext.awaitJs(pipeLoop(js, output, end,
                                                   kj::heapArray<kj::byte>(4096)))
        .attach(kj::addRef(*this));
    KJ_IF_MAYBE(readable, state.tryGet<Readable>()) {
      promise = promise.attach(readable->addRef());
    }
    return kj::mv(promise);
  }));
}

jsg::Promise<void> ReadableStreamJsTeeSource::pipeLoop(
    jsg::Lock& js,
    WritableStreamSink& output,
    bool end,
    kj::Array<kj::byte> bytes) {
  return internalTryRead(js, bytes.begin(), 1, bytes.size())
      .then(js, [this, &output, end, bytes = kj::mv(bytes)]
            (jsg::Lock& js, size_t amount) mutable -> jsg::Promise<void> {
    // Although we have captured reference to ioContext here,
    // we should not assume that the reference is still valid in
    // the continuation. Let's just grab IoContext::current()
    // to move things along.
    auto& ioContext = IoContext::current();
    if (amount == 0) {
      return end ?
          ioContext.awaitIo(output.end(), []{}) :
          js.resolvedPromise();
    }
    return ioContext.awaitIo(js, output.write(bytes.begin(), amount),
        [this, &output, end, bytes = kj::mv(bytes)] (jsg::Lock& js) mutable {
      return pipeLoop(js, output, end, kj::mv(bytes));
    });
  });
}

// ======================================================================================

WritableStreamDefaultController::WritableStreamDefaultController(WriterOwner& owner)
    : impl(owner) {}

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

WritableStreamJsController::WritableStreamJsController() {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Closed closed)
    : state(closed) {}

WritableStreamJsController::WritableStreamJsController(StreamStates::Errored errored)
    : state(kj::mv(errored)) {}

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

  maybeTransformer = kj::mv(underlyingSink.maybeTransformer);
  state = jsg::alloc<WritableStreamDefaultController>(*this);
  state.get<Controller>()->setup(js, kj::mv(underlyingSink), kj::mv(queuingStrategy));
}

kj::Maybe<jsg::Promise<void>> WritableStreamJsController::tryPipeFrom(
    jsg::Lock& js,
    jsg::Ref<ReadableStream> source,
    PipeToOptions options) {

  // The ReadableStream source here can be either a JavaScript-backed ReadableStream
  // or ReadableStreamSource-backed. In either case, however, this WritableStream is
  // JavaScript-based and must use a JavaScript promise-based data flow for piping data.
  // We'll treat all ReadableStreams as if they are JavaScript-backed.
  //
  // This method will return a JavaScript promise that is resolved when the pipe operation
  // completes, or is rejected if the pipe operation is aborted or errored.

  // Let's also acquire the destination pipe lock.
  lock.pipeLock(KJ_ASSERT_NONNULL(owner), kj::mv(source), options);

  return pipeLoop(js);
}

jsg::Promise<void> WritableStreamJsController::pipeLoop(jsg::Lock& js) {
  auto& pipeLock = lock.getPipe();
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
      return abort(js, *errored).then(js,
          JSG_VISITABLE_LAMBDA((pipeThrough, reason = js.v8Ref(*errored)),
                                (reason), (jsg::Lock& js) {
        return rejectedMaybeHandledPromise<void>(
          js,
          reason.getHandle(js),
          pipeThrough);
      }));
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

  return pipeLock.source.read(js).then(js,
      JSG_VISITABLE_LAMBDA((this, preventCancel, pipeThrough, &source, ref = addRef()),
                            (ref), (jsg::Lock& js, ReadResult result) -> jsg::Promise<void> {

    auto& pipeLock = lock.getPipe();

    KJ_IF_MAYBE(promise, pipeLock.checkSignal(js, *this)) {
      lock.releasePipeLock();
      return kj::mv(*promise);
    }

    if (result.done) {
      // We'll handle the close at the start of the next iteration.
      return pipeLoop(js);
    }

    return write(js, result.value.map([&](jsg::Value& value) {
      return value.getHandle(js.v8Isolate);
    })).then(js, [this](jsg::Lock& js) { return pipeLoop(js); },
             [&source, preventCancel, pipeThrough] (jsg::Lock& js, jsg::Value value) {
      // The write failed. We handle it here because the pipe lock will have been released.
      auto reason = value.getHandle(js);
      if (!preventCancel) {
        source.release(js, reason);
      } else {
        source.release(js);
      }
      return rejectedMaybeHandledPromise<void>(js, reason, pipeThrough);
    });
  }), [this] (jsg::Lock& js, jsg::Value value) {
    // The read failed. We will handle the error at the start of the next iteration.
    return pipeLoop(js);
  });
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
      return controller->write(js, value.orDefault(js.v8Undefined()));
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
  visitor.visit(maybeAbortPromise, lock, maybeTransformer);
}

// =======================================================================================
kj::Maybe<int> TransformStreamDefaultController::getDesiredSize() {
  return getReadableController().getDesiredSize();
}

void TransformStreamDefaultController::enqueue(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
  auto& readableController = getReadableController();
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
  getReadableController().error(js, reason);
  errorWritableAndUnblockWrite(js, reason);
}

void TransformStreamDefaultController::terminate(jsg::Lock& js) {
  getReadableController().close(js);
  errorWritableAndUnblockWrite(js, js.v8TypeError("The transform stream has been terminated"_kj));
}

jsg::Promise<void> TransformStreamDefaultController::write(
    jsg::Lock& js,
    v8::Local<v8::Value> chunk) {
  auto& writableController = getWritableController();

  KJ_IF_MAYBE(error, writableController.isErroredOrErroring(js)) {
    return js.rejectedPromise<void>(*error);
  }

  KJ_ASSERT(writableController.isWritable());

  if (backpressure) {
    auto chunkRef = js.v8Ref(chunk);
    return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved().then(js,
        JSG_VISITABLE_LAMBDA((this, chunkRef = kj::mv(chunkRef), self = JSG_THIS),
                              (chunkRef, self), (jsg::Lock& js) {
      auto& writableController = getWritableController();
      KJ_IF_MAYBE(error, writableController.isErroring(js)) {
        return js.rejectedPromise<void>(*error);
      }
      KJ_ASSERT(writableController.isWritable());
      return performTransform(js, chunkRef.getHandle(js));
    }));
  }
  return performTransform(js, chunk);
}

jsg::Promise<void> TransformStreamDefaultController::abort(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
  error(js, reason);
  return js.resolvedPromise();
}

jsg::Promise<void> TransformStreamDefaultController::close(jsg::Lock& js) {
  return jscontroller::maybeRunAlgorithm(
      js,
      algorithms.flush,
      [this](jsg::Lock& js) -> jsg::Promise<void> {
        auto& readableController = getReadableController();

        // Allows for a graceful close of the readable side. Close will
        // complete once all of the queued data is read or the stream
        // errors.
        readableController.close(js);
        return js.resolvedPromise();
      },
      [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<void> {
        error(js, reason.getHandle(js));
        return js.rejectedPromise<void>(kj::mv(reason));
      },
      JSG_THIS);
}

jsg::Promise<void> TransformStreamDefaultController::pull(jsg::Lock& js) {
  KJ_ASSERT(backpressure);
  setBackpressure(js, false);
  return KJ_ASSERT_NONNULL(maybeBackpressureChange).promise.whenResolved();
}

jsg::Promise<void> TransformStreamDefaultController::cancel(
    jsg::Lock& js,
    v8::Local<v8::Value> reason) {
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
        [this](jsg::Lock& js, jsg::Value reason) -> jsg::Promise<void> {
          error(js, reason.getHandle(js));
          return js.rejectedPromise<void>(kj::mv(reason));
        },
        chunk,
        JSG_THIS);
  }
  // If we got here, there is no transform algorithm. Per the spec, the default
  // behavior then is to just pass along the value untransformed.
  enqueue(js, chunk);
  return js.resolvedPromise();
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
  auto& writableController = getWritableController();
  if (writableController.isWritable()) {
    writableController.doError(js, reason);
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
  maybeWritableController = static_cast<WritableStreamJsController&>(writable->getController());

  auto& readableController = static_cast<ReadableStreamJsController&>(readable->getController());
  auto readableRef = KJ_ASSERT_NONNULL(readableController.getController());
  maybeReadableController = kj::mv(KJ_ASSERT_NONNULL(
      readableRef.tryGet<jsg::Ref<ReadableStreamDefaultController>>()));

  auto transformer = kj::mv(maybeTransformer).orDefault({});

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
      [this](jsg::Lock& js) {
        algorithms.starting = nullptr;
        startPromise.resolver.resolve();
      },
      [this](jsg::Lock& js, jsg::Value reason) {
        algorithms.starting = nullptr;
        startPromise.resolver.reject(reason.getHandle(js));
      },
      JSG_THIS);
}

}  // namespace workerd::api
