// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>

#include <kj/common.h>

namespace workerd::api {

// ======================================================================================
// ValueQueue
#pragma region ValueQueue

#pragma region ValueQueue::ReadRequest

void ValueQueue::ReadRequest::resolveAsDone(jsg::Lock& js) {
  resolver.resolve(js, ReadResult{.done = true});
}

void ValueQueue::ReadRequest::resolve(jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) {
  resolver.resolve(js,
      ReadResult{
        .value = kj::mv(value),
        .done = false,
      });
}

void ValueQueue::ReadRequest::reject(jsg::Lock& js, jsg::JsValue value) {
  resolver.reject(js, value);
}

#pragma endregion ValueQueue::ReadRequest

#pragma region ValueQueue::Entry

ValueQueue::Entry::Entry(jsg::JsRef<jsg::JsValue> value, size_t size)
    : value(kj::mv(value)),
      size(size) {}

jsg::JsRef<jsg::JsValue> ValueQueue::Entry::getValue(jsg::Lock& js) {
  return value.addRef(js);
}

size_t ValueQueue::Entry::getSize() const {
  return size;
}

void ValueQueue::Entry::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(value);
}

#pragma endregion ValueQueue::Entry

#pragma region ValueQueue::QueueEntry

kj::Rc<ValueQueue::Entry> ValueQueue::Entry::clone(jsg::Lock& js) {
  return addRefToThis();
}

ValueQueue::QueueEntry ValueQueue::QueueEntry::clone(jsg::Lock& js) {
  return QueueEntry{.entry = entry->clone(js)};
}

#pragma endregion ValueQueue::QueueEntry

#pragma region ValueQueue::Consumer

ValueQueue::Consumer::Consumer(
    ValueQueue& queue, kj::Weak<ConsumerImpl::StateListener> stateListener)
    : impl(queue.impl->addWeakToThis(), kj::mv(stateListener)) {}

ValueQueue::Consumer::Consumer(
    kj::Weak<QueueImpl> impl, kj::Weak<ConsumerImpl::StateListener> stateListener)
    : impl(kj::mv(impl), kj::mv(stateListener)) {}

void ValueQueue::Consumer::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  impl.cancel(js, maybeReason);
}

void ValueQueue::Consumer::close(jsg::Lock& js) {
  impl.close(js);
};

bool ValueQueue::Consumer::empty() {
  return impl.empty();
}

void ValueQueue::Consumer::error(jsg::Lock& js, jsg::JsValue reason) {
  impl.error(js, reason);
};

void ValueQueue::Consumer::read(jsg::Lock& js, kj::Own<ReadRequest> request) {
  impl.read(js, kj::mv(request));
}

void ValueQueue::Consumer::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

void ValueQueue::Consumer::reset() {
  impl.reset();
};

size_t ValueQueue::Consumer::size() {
  return impl.size();
}

kj::Own<ValueQueue::Consumer> ValueQueue::Consumer::clone(
    jsg::Lock& js, kj::Weak<ConsumerImpl::StateListener> stateListener) {
  // If the queue was destroyed (e.g., stream was closed), we can still clone
  // the consumer - the cloneTo() will copy the closed/errored state.
  auto consumer = kj::heap<Consumer>(impl.queue, stateListener);
  impl.cloneTo(js, consumer->impl);
  return kj::mv(consumer);
}

bool ValueQueue::Consumer::hasReadRequests() {
  return impl.hasReadRequests();
}

bool ValueQueue::Consumer::hasPendingDrainingRead() {
  return impl.state.whenActiveOr(
      [](const ConsumerImpl::Ready& ready) { return ready.hasPendingDrainingRead; }, false);
}

namespace {
// Helper to convert a JS value to bytes. Returns kj::none if the value cannot be converted.
kj::Maybe<kj::Array<kj::byte>> valueToBytes(jsg::Lock& js, jsg::JsRef<jsg::JsValue> value) {
  auto jsval = value.getHandle(js);
  // Try ArrayBuffer first.
  KJ_IF_SOME(ab, jsval.tryCast<jsg::JsArrayBuffer>()) {
    auto src = ab.asArrayPtr();
    return kj::heapArray(src);
  }

  // Try ArrayBufferView.
  KJ_IF_SOME(abView, jsval.tryCast<jsg::JsArrayBufferView>()) {
    auto src = abView.asArrayPtr();
    return kj::heapArray(src);
  }

  // Try string - convert to UTF-8.
  KJ_IF_SOME(str, jsval.tryCast<jsg::JsString>()) {
    auto data = str.toUSVString(js);
    return kj::heapArray(data.asBytes());
  }

  // Unsupported type.
  return kj::none;
}
}  // namespace

jsg::Promise<DrainingReadResult> ValueQueue::Consumer::drainingRead(jsg::Lock& js, size_t maxRead) {
  // If there are pending regular reads, reject - mutual exclusion.
  if (hasReadRequests()) {
    return js.rejectedPromise<DrainingReadResult>(
        js.typeError("Cannot call drainingRead while there are pending reads"_kj));
  }

  // Check if already closed or errored.
  if (impl.state.template is<ConsumerImpl::Closed>()) {
    return js.resolvedPromise(DrainingReadResult{.chunks = nullptr, .done = true});
  }
  KJ_IF_SOME(errored, impl.state.tryGetErrorUnsafe()) {
    return js.rejectedPromise<DrainingReadResult>(errored.reason.getHandle(js));
  }

  auto& ready = impl.state.requireActiveUnsafe();
  ConsumerImpl::UpdateBackpressureScope scope(impl.addWeakToThis());

  // Mark that we're doing a draining read. This allows onConsumerWantsData()
  // to use forcePull() which bypasses backpressure checks. The flag is cleared
  // either synchronously (for immediate returns) or in promise callbacks (for async).
  ready.hasPendingDrainingRead = true;

  // Collect all buffered data, converting values to bytes.
  kj::Vector<kj::Array<kj::byte>> chunks;
  bool isClosing = false;
  size_t totalRead = 0;

  // Drains buffered data, converting values to bytes. Returns a rejected promise if a value
  // cannot be converted; otherwise returns kj::none to indicate success. Stops draining
  // when totalRead reaches or exceeds maxRead (after finishing the current item).
  static const auto drainBuffer =
      [](jsg::Lock& js, ConsumerImpl& impl, ConsumerImpl::Ready& ready,
          kj::Vector<kj::Array<kj::byte>>& chunks, size_t& totalRead, bool& isClosing,
          size_t maxRead) -> kj::Maybe<jsg::Promise<DrainingReadResult>> {
    while (!ready.buffer.empty() && !isClosing && totalRead < maxRead) {
      auto& item = ready.buffer.front();
      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(close, ConsumerImpl::Close) {
          isClosing = true;
          break;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          KJ_IF_SOME(bytes, valueToBytes(js, entry.entry->getValue(js))) {
            totalRead += bytes.size();
            chunks.add(kj::mv(bytes));
            ready.queueTotalSize -= entry.entry->getSize();
            ready.buffer.pop_front();
          } else {
            auto error = js.typeError(
                "Draining read encountered a value that cannot be converted to bytes"_kj);
            impl.error(js, error);
            return js.rejectedPromise<DrainingReadResult>(error);
          }
        }
      }
    }
    return kj::none;
  };

  // Drain the buffer up to maxRead bytes, then pump for more if under the limit.
  KJ_IF_SOME(errorPromise, drainBuffer(js, impl, ready, chunks, totalRead, isClosing, maxRead)) {
    return kj::mv(errorPromise);
  }

  // Pump the controller for more synchronously available data.
  // maxRead is checked here: we only proceed with pumping if we haven't exceeded it.
  while (!isClosing && totalRead < maxRead) {
    KJ_IF_SOME(listener, impl.stateListener) {
      size_t prevChunkCount = chunks.size();
      bool pullCompletedSync = consume(kj::mv(listener))->onConsumerWantsData(js);

      // The pull callback may have closed or errored the consumer, which
      // destroys the Ready state (and its RingBuffer). We must not touch
      // `ready` after that.
      if (!impl.state.isActive()) break;

      // Drain buffered data that was added by the pull, respecting maxRead.
      KJ_IF_SOME(errorPromise,
          drainBuffer(js, impl, ready, chunks, totalRead, isClosing, maxRead)) {
        return kj::mv(errorPromise);
      }

      // If pull is async or no new data was added, stop pumping.
      if (!pullCompletedSync || chunks.size() == prevChunkCount) {
        break;
      }
    } else {
      break;
    }
  }

  // If the consumer was closed or errored during pumping, the `ready`
  // reference is dangling. Return what we have or the appropriate error.
  if (!impl.state.isActive()) {
    KJ_IF_SOME(errored, impl.state.tryGetErrorUnsafe()) {
      return js.rejectedPromise<DrainingReadResult>(errored.reason.getHandle(js));
    }
    // Closed — all data was already drained. Return collected chunks.
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = true,
    });
  }

  // If the controller was canceled during pumping (e.g., pull callback called
  // controller.cancel()), the QueueImpl is destroyed and the consumer's queue
  // reference is detached (set to kj::none). The consumer state is still Active
  // because cancel on the controller doesn't notify consumers — it only closes
  // the controller's own state. No more data will ever arrive. Drain remaining
  // buffer data respecting maxRead, and signal done only when the buffer is empty.
  if (impl.queue == nullptr) {
    // Drain remaining buffer up to maxRead. If there's still more, the caller
    // will loop back and we'll drain the rest on subsequent calls.
    KJ_IF_SOME(errorPromise, drainBuffer(js, impl, ready, chunks, totalRead, isClosing, maxRead)) {
      return kj::mv(errorPromise);
    }
    ready.hasPendingDrainingRead = false;
    bool done = ready.buffer.empty() || isClosing;
    // If isClosing, finalize the consumer so onConsumerClose fires promptly.
    // maybeDrainAndSetState may transition consumer to Closed, making `ready` dangling.
    if (isClosing) {
      impl.maybeDrainAndSetState(js);
    }
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = done,
    });
  }

  // If we collected data, return it immediately.
  if (!chunks.empty() || isClosing) {
    ready.hasPendingDrainingRead = false;
    // If isClosing, finalize the consumer so onConsumerClose fires promptly.
    // maybeDrainAndSetState may transition consumer to Closed, making `ready` dangling.
    if (isClosing) {
      impl.maybeDrainAndSetState(js);
    }
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = isClosing,
    });
  }

  // No data available - need to wait. Queue a pending draining read.
  // We create a ReadResult promise and transform it to DrainingReadResult.
  // The flag remains set (was set at the start) and will be cleared by the promise callbacks.
  auto prp = js.newPromiseAndResolver<ReadResult>();

  ReadRequest request{.resolver = kj::mv(prp.resolver)};
  ready.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));

  KJ_IF_SOME(listener, impl.stateListener) {
    consume(kj::mv(listener))->onConsumerWantsData(js);
  }

  // Transform the ReadResult promise to DrainingReadResult.
  return prp.promise.then(js,
      [weakThis = addWeakToThis()](jsg::Lock& js, ReadResult result) mutable -> DrainingReadResult {
    auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "consumer destroyed before read completed");
    KJ_IF_SOME(ready, self.impl.state.tryGetActiveUnsafe()) {
      ready.hasPendingDrainingRead = false;
    }

    if (result.done) {
      return DrainingReadResult{.chunks = nullptr, .done = true};
    }

    // Convert the value to bytes.
    kj::Vector<kj::Array<kj::byte>> chunks;
    KJ_IF_SOME(val, result.value) {
      KJ_IF_SOME(bytes, valueToBytes(js, val.addRef(js))) {
        chunks.add(kj::mv(bytes));
      }
      // If valueToBytes returned kj::none, we just return empty chunks.
      // The error case should have been caught earlier.
    }

    return DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = false,
    };
  },
      [weakThis = addWeakToThis()](
          jsg::Lock& js, jsg::Value exception) mutable -> DrainingReadResult {
    auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "consumer destroyed before read failed");
    KJ_IF_SOME(ready, self.impl.state.tryGetActiveUnsafe()) {
      ready.hasPendingDrainingRead = false;
    }
    js.throwException(kj::mv(exception));
  });
}

void ValueQueue::Consumer::cancelPendingReads(jsg::Lock& js, jsg::JsValue reason) {
  impl.cancelPendingReads(js, reason);
}

void ValueQueue::Consumer::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

#pragma endregion ValueQueue::Consumer

ValueQueue::ValueQueue(size_t highWaterMark): impl(kj::rc<QueueImpl>(highWaterMark)) {}

void ValueQueue::close(jsg::Lock& js) {
  auto queue = impl.addRef();
  queue->close(js);
}

ssize_t ValueQueue::desiredSize() const {
  return impl->desiredSize();
}

void ValueQueue::error(jsg::Lock& js, jsg::JsValue reason) {
  auto queue = impl.addRef();
  queue->error(js, reason);
}

void ValueQueue::maybeUpdateBackpressure() {
  impl->maybeUpdateBackpressure();
}

void ValueQueue::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl->push(js, kj::mv(entry));
}

size_t ValueQueue::size() const {
  return impl->size();
}

void ValueQueue::handlePush(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    kj::Weak<ConsumerImpl> consumer,
    kj::Rc<Entry> entry) {
  // If there are no pending reads, just add the entry to the buffer and return, adjusting
  // the size of the queue in the process.
  if (state.readRequests.empty()) {
    state.queueTotalSize += entry->getSize();
    state.buffer.push_back(QueueEntry{.entry = kj::mv(entry)});
    return;
  }

  // Otherwise, pop the next pending read and resolve it. There should be nothing in the queue.
  KJ_REQUIRE(state.buffer.empty() && state.queueTotalSize == 0);
  auto request = kj::mv(state.readRequests.front());
  state.readRequests.pop_front();
  request->resolve(js, entry->getValue(js));
}

void ValueQueue::handleRead(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    kj::Weak<ConsumerImpl> consumer,
    kj::Weak<QueueImpl>,
    kj::Own<ReadRequest> request) {
  // If there are no pending read requests and there is data in the buffer,
  // we will try to fulfill the read request immediately.
  if (state.queueTotalSize > 0 && state.buffer.empty()) {
    // Is our queue accounting correct?
    LOG_WARNING_ONCE("ValueQueue::handleRead encountered a queueTotalSize > 0 "
                     "with an empty buffer. This should not happen.",
        state.queueTotalSize);
  }
  if (state.readRequests.empty() && !state.buffer.empty()) {
    auto& entry = state.buffer.front();

    KJ_SWITCH_ONEOF(entry) {
      KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
        // This case shouldn't actually happen. The queueTotalSize should be zero if the
        // only item remaining in the queue is the close sentinel because we decrement the
        // queueTotalSize every time we remove an item. If we get here, something is wrong.
        // We'll handle it by resolving the read request and keep going but let's emit a log
        // warning so we can investigate.
        // Note that we do not want to remove the close sentinel here so that the next call to
        // maybeDrainAndSetState will see it and handle the transition to the closed state.
        KJ_LOG(ERROR,
            "ValueQueue::handleRead encountered a close sentinel in the queue "
            "with queueTotalSize > 0. This should not happen.",
            state.queueTotalSize);
        request->resolveAsDone(js);
        return;
      }
      KJ_CASE_ONEOF(entry, QueueEntry) {
        auto freed = kj::mv(entry);
        state.buffer.pop_front();
        auto size = freed.entry->getSize();
        {
          jsg::DisallowJavaScriptScope noJs(js);
          request->resolve(js, freed.entry->getValue(js));
        }
        KJ_IF_SOME(c, consumer) {
          c->state.requireActiveUnsafe().queueTotalSize -= size;
        }
        return;
      }
    }
    KJ_UNREACHABLE;
  } else if (state.queueTotalSize == 0 && consumer.assertLive().isClosing()) {
    // Otherwise, if state.queueTotalSize is zero and isClosing() is true there won't be any
    // more data coming. Just resolve the read as done and move on.
    request->resolveAsDone(js);
  } else {
    // Otherwise, push the read request into the pending readRequests. It will be
    // resolved either as soon as there is data available or the consumer closes
    // or errors.
    state.readRequests.push_back(kj::mv(request));
    KJ_IF_SOME(c, consumer) {
      KJ_IF_SOME(listener, consume(kj::mv(c))->stateListener) {
        consume(kj::mv(listener))->onConsumerWantsData(js);
      }
    }
  }
}

bool ValueQueue::handleMaybeClose(
    jsg::Lock& js, ConsumerImpl::Ready& state, kj::Weak<ConsumerImpl> consumer) {
  // Buffered values require another read before the close sentinel can be reached.
  return state.queueTotalSize == 0;
}

size_t ValueQueue::getConsumerCount() {
  return impl->getConsumerCount();
}

bool ValueQueue::wantsRead() const {
  return impl->wantsRead();
}

bool ValueQueue::hasPartiallyFulfilledRead() {
  // A ValueQueue can never have a partially fulfilled read.
  return false;
}

void ValueQueue::visitForGc(jsg::GcVisitor& visitor) {}

#pragma endregion ValueQueue

// ======================================================================================
// ByteQueue
#pragma region ByteQueue

#pragma region ByteQueue::ReadRequest

namespace {
void maybeInvalidateByobRequest(kj::Weak<ByteQueue::ByobRequest>& req) {
  KJ_IF_SOME(byobRequest, req) {
    consume(kj::mv(byobRequest))->invalidate();
    // The call to byobRequest->invalidate() should have cleared the reference.
    KJ_ASSERT(req == nullptr);
  }
}
}  // namespace

ByteQueue::ReadRequest::ReadRequest(
    jsg::Promise<ReadResult>::Resolver resolver, ByteQueue::ReadRequest::PullInto pullInto)
    : resolver(kj::mv(resolver)),
      byobReadRequest(nullptr),
      pullInto(kj::mv(pullInto)) {}

ByteQueue::ReadRequest::~ReadRequest() noexcept(false) {
  maybeInvalidateByobRequest(byobReadRequest);
}

void ByteQueue::ReadRequest::resolveAsDone(jsg::Lock& js) {
  auto handle = pullInto.view.getHandle(js);
  // If there's been at least some data written, we need to respond
  // but not set done to true since that's what the streams spec
  // requires.
  auto view = handle.slice(js, 0, pullInto.filled);
  maybeInvalidateByobRequest(byobReadRequest);
  resolver.resolve(js,
      ReadResult{
        .value = jsg::JsValue(view).addRef(js),
        .done = !(pullInto.filled > 0),
      });
}

void ByteQueue::ReadRequest::resolve(jsg::Lock& js) {
  auto handle = pullInto.view.getHandle(js);
  auto view = handle.slice(js, 0, pullInto.filled);
  maybeInvalidateByobRequest(byobReadRequest);
  resolver.resolve(js,
      ReadResult{
        .value = jsg::JsValue(view).addRef(js),
        .done = false,
      });
}

void ByteQueue::ReadRequest::reject(jsg::Lock& js, jsg::JsValue value) {
  maybeInvalidateByobRequest(byobReadRequest);
  resolver.reject(js, value);
}

kj::Own<ByteQueue::ByobRequest> ByteQueue::ReadRequest::makeByobReadRequest(
    kj::Weak<ConsumerImpl> consumer, kj::Weak<QueueImpl> queue) {
  auto req = kj::heap<ByobRequest>(addWeakToThis(), kj::mv(consumer), kj::mv(queue));
  byobReadRequest = req->addWeakRef();
  nativeHandleIssued = true;
  return kj::mv(req);
}

#pragma endregion ByteQueue::ReadRequest

#pragma region ByteQueue::Entry

ByteQueue::Entry::Entry(jsg::Lock& js, jsg::JsBufferSource store)
    : store(store.addRef(js)),
      size(store.size()),
      offset(store.getOffset()) {}

kj::ArrayPtr<kj::byte> ByteQueue::Entry::toArrayPtr(jsg::Lock& js) {
  auto handle = store.getHandle(js);
  // Size and offset should not have changed since construction.
  // This is purely defensive. The handle.asArrayPtr below is safe
  // as it alays returns a correctly sized ArrayPtr for the live
  // data, and KJ_IREQUIRE bounds checks the kj::ArrayPtr operations
  // themselves, but we want to catch any unexpected variance early.
  // A ByteQueue::Entry is not supposed to be mutable after construction.
  KJ_ASSERT(size == handle.size());
  KJ_ASSERT(offset == handle.getOffset());
  return handle.asArrayPtr();
}

size_t ByteQueue::Entry::getSize() const {
  return size;
}

kj::Rc<ByteQueue::Entry> ByteQueue::Entry::clone(jsg::Lock& js) {
  return addRefToThis();
}

void ByteQueue::Entry::visitForGc(jsg::GcVisitor& visitor) {}

#pragma endregion ByteQueue::Entry

#pragma region ByteQueue::QueueEntry

ByteQueue::QueueEntry ByteQueue::QueueEntry::clone(jsg::Lock& js) {
  return QueueEntry{
    .entry = entry->clone(js),
    .offset = offset,
  };
}

#pragma endregion ByteQueue::QueueEntry

#pragma region ByteQueue::Consumer

ByteQueue::Consumer::Consumer(ByteQueue& queue, kj::Weak<ConsumerImpl::StateListener> stateListener)
    : impl(queue.impl->addWeakToThis(), kj::mv(stateListener)) {}

ByteQueue::Consumer::Consumer(
    kj::Weak<QueueImpl> impl, kj::Weak<ConsumerImpl::StateListener> stateListener)
    : impl(kj::mv(impl), kj::mv(stateListener)) {}

void ByteQueue::Consumer::cancel(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  KJ_IF_SOME(ready, impl.state.tryGetActiveUnsafe()) {
    // Cancellation discards accepted bytes as well as queued bytes.
    for (auto& request: ready.readRequests) {
      request->pullInto.filled = 0;
    }
  }
  impl.cancel(js, maybeReason);
}

void ByteQueue::Consumer::close(jsg::Lock& js) {
  impl.close(js);
}

bool ByteQueue::Consumer::empty() const {
  return impl.empty();
}

void ByteQueue::Consumer::error(jsg::Lock& js, jsg::JsValue reason) {
  impl.error(js, reason);
}

void ByteQueue::Consumer::read(jsg::Lock& js, kj::Own<ReadRequest> request) {
  impl.read(js, kj::mv(request));
}

void ByteQueue::Consumer::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  auto weak = impl.addWeakToThis();
  impl.push(js, kj::mv(entry));
  ByteQueue::drain(js, kj::mv(weak));
}

void ByteQueue::Consumer::reset() {
  impl.reset();
}

size_t ByteQueue::Consumer::size() const {
  return impl.size();
}

kj::Own<ByteQueue::Consumer> ByteQueue::Consumer::clone(
    jsg::Lock& js, kj::Weak<ConsumerImpl::StateListener> stateListener) {
  // If the queue was destroyed (e.g., stream was closed), we can still clone
  // the consumer - the cloneTo() will copy the closed/errored state.
  auto consumer = kj::heap<Consumer>(impl.queue, stateListener);
  impl.cloneTo(js, consumer->impl);
  return kj::mv(consumer);
}

bool ByteQueue::Consumer::hasReadRequests() {
  return impl.hasReadRequests();
}

bool ByteQueue::Consumer::hasPendingDrainingRead() {
  return impl.state.whenActiveOr(
      [](const ConsumerImpl::Ready& ready) { return ready.hasPendingDrainingRead; }, false);
}

jsg::Promise<DrainingReadResult> ByteQueue::Consumer::drainingRead(jsg::Lock& js, size_t maxRead) {
  // If there are pending regular reads, reject - mutual exclusion.
  if (hasReadRequests()) {
    return js.rejectedPromise<DrainingReadResult>(
        js.typeError("Cannot call drainingRead while there are pending reads"_kj));
  }

  // Check if already closed or errored.
  if (impl.state.template is<ConsumerImpl::Closed>()) {
    return js.resolvedPromise(DrainingReadResult{.chunks = nullptr, .done = true});
  }
  KJ_IF_SOME(errored, impl.state.tryGetErrorUnsafe()) {
    return js.rejectedPromise<DrainingReadResult>(errored.reason.getHandle(js));
  }

  auto& ready = impl.state.requireActiveUnsafe();
  ConsumerImpl::UpdateBackpressureScope scope(impl.addWeakToThis());

  // Mark that we're doing a draining read. This allows onConsumerWantsData()
  // to use forcePull() which bypasses backpressure checks. The flag is cleared
  // either synchronously (for immediate returns) or in promise callbacks (for async).
  ready.hasPendingDrainingRead = true;

  // Collect all buffered data (already bytes for ByteQueue).
  kj::Vector<kj::Array<kj::byte>> chunks;
  bool isClosing = false;
  size_t totalRead = 0;

  // Drains buffered byte data into chunks. Stops draining when totalRead reaches
  // or exceeds maxRead (after finishing the current item).
  static const auto drainBuffer = [](jsg::Lock& js, ConsumerImpl::Ready& ready,
                                      kj::Vector<kj::Array<kj::byte>>& chunks, size_t& totalRead,
                                      bool& isClosing, size_t maxRead) {
    while (!ready.buffer.empty() && !isClosing && totalRead < maxRead) {
      auto& item = ready.buffer.front();
      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(close, ConsumerImpl::Close) {
          isClosing = true;
          break;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          auto ptr = entry.entry->toArrayPtr(js);
          auto offset = entry.offset;
          auto size = ptr.size() - offset;
          totalRead += size;
          chunks.add(kj::heapArray(ptr.slice(offset, offset + size)));
          ready.queueTotalSize -= size;
          ready.buffer.pop_front();
        }
      }
    }
  };

  // Drain the buffer up to maxRead bytes, then pump for more if under the limit.
  drainBuffer(js, ready, chunks, totalRead, isClosing, maxRead);

  // Pump the controller for more synchronously available data.
  // maxRead is checked here: we only proceed with pumping if we haven't exceeded it.
  while (!isClosing && totalRead < maxRead) {
    KJ_IF_SOME(listener, impl.stateListener) {
      size_t prevChunkCount = chunks.size();
      bool pullCompletedSync = consume(kj::mv(listener))->onConsumerWantsData(js);

      // The pull callback may have closed or errored the consumer, which
      // destroys the Ready state (and its RingBuffer). We must not touch
      // `ready` after that.
      if (!impl.state.isActive()) break;

      // Drain buffered data that was added by the pull, respecting maxRead.
      drainBuffer(js, ready, chunks, totalRead, isClosing, maxRead);

      // If pull is async or no new data was added, stop pumping.
      if (!pullCompletedSync || chunks.size() == prevChunkCount) {
        break;
      }
    } else {
      break;
    }
  }

  // If the consumer was closed or errored during pumping, the `ready`
  // reference is dangling. Return what we have or the appropriate error.
  if (!impl.state.isActive()) {
    KJ_IF_SOME(errored, impl.state.tryGetErrorUnsafe()) {
      return js.rejectedPromise<DrainingReadResult>(errored.reason.getHandle(js));
    }
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = true,
    });
  }

  // If the controller was canceled during pumping (e.g., pull callback called
  // controller.cancel()), the QueueImpl is destroyed and the consumer's queue
  // reference is detached (set to kj::none). The consumer state is still Active
  // because cancel on the controller doesn't notify consumers — it only closes
  // the controller's own state. No more data will ever arrive. Drain remaining
  // buffer data respecting maxRead, and signal done only when the buffer is empty.
  if (impl.queue == nullptr) {
    // Drain remaining buffer up to maxRead. If there's still more, the caller
    // will loop back and we'll drain the rest on subsequent calls.
    drainBuffer(js, ready, chunks, totalRead, isClosing, maxRead);
    ready.hasPendingDrainingRead = false;
    bool done = ready.buffer.empty() || isClosing;
    // If isClosing, finalize the consumer so onConsumerClose fires promptly.
    // maybeDrainAndSetState may transition consumer to Closed, making `ready` dangling.
    if (isClosing) {
      impl.maybeDrainAndSetState(js);
    }
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = done,
    });
  }

  // If we collected data, return it immediately.
  if (!chunks.empty() || isClosing) {
    ready.hasPendingDrainingRead = false;
    // If isClosing, finalize the consumer so onConsumerClose fires promptly.
    // maybeDrainAndSetState may transition consumer to Closed, making `ready` dangling.
    if (isClosing) {
      impl.maybeDrainAndSetState(js);
    }
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = isClosing,
    });
  }

  // No data available - need to wait. Create a default read request.
  // We allocate a buffer for the read - the data will be copied into it.
  // The flag remains set (was set at the start) and will be cleared by the promise callbacks.
  constexpr size_t kDefaultReadSize = 16384;  // 16KB default buffer
  KJ_IF_SOME(store, jsg::JsUint8Array::tryCreate(js, kDefaultReadSize)) {
    auto prp = js.newPromiseAndResolver<ReadResult>();

    ReadRequest::PullInto pullInto{
      .view = jsg::JsArrayBufferView(store).addRef(js),
      .elementSize = 1,
      .originalOffset = 0,
      .filled = 0,
      .atLeast = 1,
      .type = ReadRequest::Type::DEFAULT,
    };
    auto request = kj::heap<ReadRequest>(kj::mv(prp.resolver), kj::mv(pullInto));
    ready.readRequests.push_back(kj::mv(request));

    KJ_IF_SOME(listener, impl.stateListener) {
      consume(kj::mv(listener))->onConsumerWantsData(js);
    }

    // Transform the ReadResult promise to DrainingReadResult.
    return prp.promise.then(js,
        [weakThis = addWeakToThis()](
            jsg::Lock& js, ReadResult result) mutable -> DrainingReadResult {
      auto& self =
          KJ_REQUIRE_NONNULL(weakThis.tryGet(), "consumer destroyed before read completed");
      KJ_IF_SOME(ready, self.impl.state.tryGetActiveUnsafe()) {
        ready.hasPendingDrainingRead = false;
      }

      if (result.done) {
        return DrainingReadResult{.chunks = nullptr, .done = true};
      }

      kj::Vector<kj::Array<kj::byte>> chunks;
      KJ_IF_SOME(val, result.value) {
        auto jsval = jsg::JsValue(val.getHandle(js));
        KJ_IF_SOME(ab, jsval.tryCast<jsg::JsArrayBuffer>()) {
          chunks.add(kj::heapArray(ab.asArrayPtr()));
        } else KJ_IF_SOME(abView, jsval.tryCast<jsg::JsArrayBufferView>()) {
          chunks.add(kj::heapArray(abView.asArrayPtr()));
        }
      }

      return DrainingReadResult{
        .chunks = chunks.releaseAsArray(),
        .done = false,
      };
    },
        [weakThis = addWeakToThis()](
            jsg::Lock& js, jsg::Value exception) mutable -> DrainingReadResult {
      auto& self = KJ_REQUIRE_NONNULL(weakThis.tryGet(), "consumer destroyed before read failed");
      KJ_IF_SOME(ready, self.impl.state.tryGetActiveUnsafe()) {
        ready.hasPendingDrainingRead = false;
      }
      js.throwException(kj::mv(exception));
    });
  } else {
    return js.rejectedPromise<DrainingReadResult>(
        js.error("Failed to allocate buffer for draining read"_kj));
  }
}

void ByteQueue::Consumer::cancelPendingReads(jsg::Lock& js, jsg::JsValue reason) {
  impl.cancelPendingReads(js, reason);
}

void ByteQueue::Consumer::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(impl);
}

#pragma endregion ByteQueue::Consumer

#pragma region ByteQueue::ByobRequest

ByteQueue::ByobRequest::~ByobRequest() noexcept(false) {
  invalidate();
}

void ByteQueue::ByobRequest::invalidate() {
  KJ_IF_SOME(req, request) {
    req->byobReadRequest = nullptr;
    request = nullptr;
  }
}

bool ByteQueue::ByobRequest::isPartiallyFulfilled() {
  return !isInvalidated() && getRequest().pullInto.filled % getRequest().pullInto.elementSize != 0;
}

bool ByteQueue::ByobRequest::respond(jsg::Lock& js, size_t amount) {
  JSG_REQUIRE(!responding, TypeError, "A BYOB response is already in progress.");
  auto self = addWeakToThis();
  auto pending = request;
  auto weakConsumer = consumer;
  auto weakQueue = queue;
  responding = true;
  KJ_DEFER({
    if (pending != nullptr) {
      pending.assertLive().responseInProgress = false;
    }
    if (self != nullptr) {
      self.assertLive().responding = false;
    }
  });
  auto view = pending.assertLive().pullInto.view.getHandle(js);
  const auto filled = pending.assertLive().pullInto.filled;
  JSG_REQUIRE(filled <= view.size() && amount <= view.size() - filled, RangeError,
      kj::str("Too many bytes [", amount, "] in response to a BYOB read request."));

  const auto copyEntry = [&](size_t offset, size_t size) -> kj::Rc<Entry> {
    KJ_IF_SOME(store, jsg::JsUint8Array::tryCreate(js, size)) {
      auto entry = kj::rc<Entry>(js, jsg::JsBufferSource(store));
      entry->toArrayPtr(js).write(view.asArrayPtr().slice(offset).first(size));
      return entry;
    }
    js.throwException(js.error("Failed to allocate memory for the byob read response."_kj));
  };
  kj::Maybe<kj::Rc<Entry>> siblings;
  if (weakQueue.assertLive().getConsumerCount() > 1) {
    siblings = copyEntry(filled, amount);
  }
  if (pending == nullptr) return true;
  const bool complete = amount >= pending.assertLive().pullInto.atLeast;
  const auto unaligned =
      complete ? (filled + amount) % pending.assertLive().pullInto.elementSize : 0;
  kj::Maybe<kj::Rc<Entry>> excess;
  if (unaligned > 0) {
    excess = copyEntry(filled + amount - unaligned, unaligned);
  }

  if (self == nullptr || pending == nullptr || self.assertLive().isInvalidated()) return true;
  KJ_IF_SOME(liveConsumer, weakConsumer) {
    if (!liveConsumer->state.isActive()) return true;
    auto& ready = liveConsumer->state.requireActiveUnsafe();
    if (ready.readRequests.empty() || ready.readRequests.front().get() != &pending.assertLive()) {
      return true;
    }
    // Progress and the typed tail precede sibling settlement and any overlapping enqueue.
    auto& req = pending.assertLive();
    req.responseInProgress = true;
    req.pullInto.filled = filled + amount - unaligned;
    req.pullInto.atLeast = complete ? 0 : req.pullInto.atLeast - amount;
    KJ_IF_SOME(entry, excess) {
      ready.queueTotalSize += entry->getSize();
      ready.buffer.emplace_back(QueueEntry{.entry = kj::mv(entry), .offset = 0});
    }
  } else {
    return true;
  }

  KJ_IF_SOME(entry, siblings) {
    KJ_IF_SOME(liveQueue, weakQueue) {
      consume(kj::mv(liveQueue))->push(js, kj::mv(entry), weakConsumer);
    }
  }
  KJ_IF_SOME(req, pending) {
    req->responseInProgress = false;
  }
  // Revocation removes the source's handle, not the accepted bytes or the pending read.
  ByteQueue::drain(js, weakConsumer);
  KJ_IF_SOME(c, weakConsumer) {
    consume(kj::mv(c))->maybeDrainAndSetState(js);
  }
  return self == nullptr || pending == nullptr || self.assertLive().isInvalidated();
}

bool ByteQueue::ByobRequest::respondWithNewView(jsg::Lock& js, jsg::JsBufferSource view) {
  // Reject a nested response before transferring its buffer or changing the destination.
  JSG_REQUIRE(!responding, TypeError, "A BYOB response is already in progress.");
  // The idea here is that rather than filling the view that the controller was given,
  // it chose to create its own view and fill that, likely over the same ArrayBuffer.
  // What we do here is perform some basic validations on what we were given, and if
  // those pass, we'll replace the backing store held in the req.pullInto with the one
  // given, then continue on issuing the respond as normal.
  auto self = addWeakToThis();
  auto pending = request;
  auto& req = pending.assertLive();
  auto amount = view.size();
  auto destView = req.pullInto.view.getHandle(js);

  JSG_REQUIRE(view.isDetachable(), TypeError, "Unable to use non-detachable ArrayBuffer.");
  JSG_REQUIRE(destView.getOffset() + req.pullInto.filled == view.getOffset(), RangeError,
      "The given view has an invalid byte offset.");
  JSG_REQUIRE(destView.size() == view.underlyingArrayBufferSize(js), RangeError,
      "The underlying ArrayBuffer is not the correct length.");
  JSG_REQUIRE(req.pullInto.filled + amount <= destView.size(), RangeError,
      "The view is not the correct length.");

  const auto offset = destView.getOffset();
  const auto size = destView.size();
  KJ_IF_SOME(replacement, jsg::JsValue(view).tryCast<jsg::JsArrayBufferView>()) {
    // The destination adopts the replacement's element size, so a size it doesn't divide could
    // never be filled. Check before the transfer so the read stays usable.
    JSG_REQUIRE(size % replacement.getElementSize() == 0, RangeError,
        "The view's element size must divide the destination size.");
  }
  auto prefix = kj::heapArray(destView.asArrayPtr().first(req.pullInto.filled));
  auto detached = view.detachAndTake(js);
  if (self == nullptr || pending == nullptr || self.assertLive().isInvalidated()) return true;
  auto newView = ([&]() -> jsg::JsArrayBufferView {
    KJ_IF_SOME(replacement, jsg::JsValue(detached).tryCast<jsg::JsArrayBufferView>()) {
      const auto elementSize = replacement.getElementSize();
      JSG_REQUIRE(offset % elementSize == 0, RangeError, "The view has an invalid byte offset.");
      const auto elements = size / elementSize;
      auto buffer = replacement.getBuffer();
      // Keep the full destination region, not just the current response's subview.
      if (replacement.isUint8Array()) return buffer.newUint8View(offset, elements);
      if (replacement.isInt8Array()) return buffer.newInt8View(offset, elements);
      if (replacement.isUint8ClampedArray()) return buffer.newUint8ClampedView(offset, elements);
      if (replacement.isUint16Array()) return buffer.newUint16View(offset, elements);
      if (replacement.isInt16Array()) return buffer.newInt16View(offset, elements);
      if (replacement.isUint32Array()) return buffer.newUint32View(offset, elements);
      if (replacement.isInt32Array()) return buffer.newInt32View(offset, elements);
      if (replacement.isFloat16Array()) return buffer.newFloat16View(offset, elements);
      if (replacement.isFloat32Array()) return buffer.newFloat32View(offset, elements);
      if (replacement.isFloat64Array()) return buffer.newFloat64View(offset, elements);
      if (replacement.isBigInt64Array()) return buffer.newBigInt64View(offset, elements);
      if (replacement.isBigUint64Array()) return buffer.newBigUint64View(offset, elements);
      return buffer.newDataView(offset, size);
    }
    jsg::JsUint8Array u8 = detached;
    return u8.getBuffer().newUint8View(offset, size);
  })();
  newView.asArrayPtr().first(prefix.size()).write(prefix);
  if (self == nullptr || pending == nullptr || self.assertLive().isInvalidated()) return true;
  auto& current = pending.assertLive();
  current.pullInto.view = newView.addRef(js);
  current.pullInto.elementSize = newView.getElementSize();

  return respond(js, amount);
}

size_t ByteQueue::ByobRequest::getAtLeast() const {
  KJ_IF_SOME(req, request) {
    return req->pullInto.atLeast;
  }
  return 0;
}

bool ByteQueue::ByobRequest::prepareView(jsg::Lock& js) {
  if (request == nullptr) return false;
  KJ_IF_SOME(c, consumer) {
    if (c->state.isActive()) {
      auto& ready = c->state.requireActiveUnsafe();
      if (ready.readRequests.empty() || ready.readRequests.front().get() != &request.assertLive()) {
        return false;
      }
      // Older bytes precede the next response. A sufficient backlog can settle this read.
      ByteQueue::drain(js, consumer, request);
      return !isInvalidated();
    }
  }
  return false;
}

kj::Maybe<jsg::JsUint8Array> ByteQueue::ByobRequest::getView(jsg::Lock& js) {
  if (!prepareView(js)) return kj::none;
  KJ_IF_SOME(req, request) {
    // The view handed out to JS sits on its own ArrayBuffer instance rather than the one
    // backing pullInto.view. ReadableStreamBYOBRequest detaches this view to revoke the
    // user's access to it once the request settles or the fill offset moves; sharing an
    // ArrayBuffer with pullInto.view would make that detach also destroy the read
    // request's destination buffer and any result view already sliced from it. clone()
    // shares the backing store without sharing the ArrayBuffer, so the two detach
    // independently.
    //
    // Normalizing to a Uint8Array first matters: `filled` is a byte count and need not be
    // a multiple of the view's element size, so slicing at that offset while preserving
    // the original element type would build a misaligned typed array.
    jsg::JsUint8Array view = req->pullInto.view.getHandle(js);
    auto cloned = view.clone(js);
    return cloned.slice(js, req->pullInto.filled, cloned.size() - req->pullInto.filled);
  }
  return kj::none;
}

size_t ByteQueue::ByobRequest::getOriginalBufferByteLength(jsg::Lock& js) const {
  KJ_IF_SOME(req, request) {
    auto view = req->pullInto.view.getHandle(js);
    return view.getBuffer().size();
  }
  return 0;
}

size_t ByteQueue::ByobRequest::getOriginalByteOffsetPlusBytesFilled() const {
  KJ_IF_SOME(req, request) {
    return req->pullInto.originalOffset + req->pullInto.filled;
  }
  return 0;
}

#pragma endregion ByteQueue::ByobRequest

ByteQueue::ByteQueue(size_t highWaterMark): impl(kj::rc<QueueImpl>(highWaterMark)) {}

void ByteQueue::invalidateReadRequests(ConsumerImpl& consumer) {
  // Every native handle must be disconnected before the first consumer settles. The JS
  // wrapper retains its view and metadata for closed-state response validation.
  KJ_IF_SOME(ready, consumer.state.tryGetActiveUnsafe()) {
    for (auto& request: ready.readRequests) {
      maybeInvalidateByobRequest(request->byobReadRequest);
    }
  }
}

void ByteQueue::close(jsg::Lock& js) {
  auto queue = impl.addRef();
  // Pedantic closed-state responses retain the JS wrapper and validate its stored metadata.
  // Native handles are invalidated for every consumer before terminal fan-out.
  if (!FeatureFlags::get(js).getPedanticWpt()) {
    KJ_IF_SOME(ready, queue->state.tryGetUnsafe<ByteQueue::QueueImpl::Ready>()) {
      while (!ready.pendingByobReadRequests.empty()) {
        ready.pendingByobReadRequests.front()->invalidate();
        ready.pendingByobReadRequests.pop_front();
      }
    }
  }
  if (queue->state.isActive()) {
    queue->allConsumers.forEach([](auto& consumer) { invalidateReadRequests(consumer); });
  }
  queue->close(js);
}

ssize_t ByteQueue::desiredSize() const {
  return impl->desiredSize();
}

void ByteQueue::error(jsg::Lock& js, jsg::JsValue reason) {
  auto queue = impl.addRef();
  if (queue->state.isActive()) {
    queue->allConsumers.forEach([](auto& consumer) { invalidateReadRequests(consumer); });
  }
  queue->error(js, reason);
}

void ByteQueue::maybeUpdateBackpressure() {
  KJ_IF_SOME(state, impl->getState()) {
    // Invalidated byob read requests will accumulate if we do not take
    // care of them from time to time. Since maybeUpdateBackpressure
    // is going to be called regularly while the queue is actively in use,
    // this is as good a place to clean them out as any.
    //
    // We iterate through the ring buffer and remove invalidated items from the front.
    // Since items are typically invalidated in order, this should be efficient for
    // the common case.
    while (!state.pendingByobReadRequests.empty() &&
        state.pendingByobReadRequests.front()->isInvalidated()) {
      state.pendingByobReadRequests.pop_front();
    }
  }
  impl->maybeUpdateBackpressure();
}

void ByteQueue::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl->push(js, kj::mv(entry));
}

size_t ByteQueue::size() const {
  return impl->size();
}

void ByteQueue::handlePush(
    jsg::Lock&, ConsumerImpl::Ready& state, kj::Weak<ConsumerImpl>, kj::Rc<Entry> newEntry) {
  state.queueTotalSize += newEntry->getSize();
  state.buffer.emplace_back(QueueEntry{.entry = kj::mv(newEntry), .offset = 0});
}

void ByteQueue::drain(jsg::Lock& js,
    kj::Weak<ConsumerImpl> consumer,
    kj::Weak<ReadRequest> partialRead,
    DrainMode mode) {
  ConsumerImpl::UpdateBackpressureScope scope(consumer);
  const bool closing = mode == DrainMode::CLOSING;
  while (consumer != nullptr) {
    auto& current = consumer.assertLive();
    if (!current.state.isActive() || current.isClosing() != closing) return;
    auto& state = current.state.requireActiveUnsafe();
    if (state.readRequests.empty()) return;
    auto& pending = *state.readRequests.front();
    if (pending.responseInProgress) return;

    auto& into = pending.pullInto;
    if (into.atLeast > 0) {
      // Empty EOF reads are settled after the consumer publishes Closed.
      if (closing && state.queueTotalSize == 0 && into.filled == 0) return;
      auto handle = into.view.getHandle(js);
      auto available = kj::min(state.queueTotalSize, handle.size() - into.filled);
      bool complete = available >= into.atLeast;
      if (complete) {
        // Leave fractional elements in the queue, including across entry boundaries.
        auto aligned = into.filled + available;
        aligned -= aligned % into.elementSize;
        complete = aligned >= into.filled && aligned - into.filled >= into.atLeast;
        if (complete) available = aligned - into.filled;
      }
      if (closing && !complete) {
        // Legacy EOF flushing returns the whole-element prefix even below the minimum.
        complete = true;
      }
      if (!closing && !complete && into.type == ReadRequest::Type::BYOB &&
          !pending.nativeHandleIssued) {
        KJ_IF_SOME(q, current.queue) {
          KJ_IF_SOME(queueState, q->getState()) {
            // Only a new front read can receive its first handle.
            queueState.pendingByobReadRequests.push_back(
                pending.makeByobReadRequest(consumer, current.queue));
          }
        }
      }
      // New reads and newly exposed BYOB views absorb older below-minimum bytes.
      // Enqueue otherwise leaves such data buffered until it can satisfy the minimum.
      if (!complete && (partialRead == nullptr || &partialRead.assertLive() != &pending)) return;
      auto remaining = available;
      while (remaining > 0 && !state.buffer.front().is<ConsumerImpl::Close>()) {
        auto& entry = state.buffer.front().get<QueueEntry>();
        auto source = entry.entry->toArrayPtr(js).slice(entry.offset);
        auto amount = kj::min(source.size(), remaining);
        handle.asArrayPtr().slice(into.filled).write(source.first(amount));
        into.filled += amount;
        entry.offset += amount;
        state.queueTotalSize -= amount;
        remaining -= amount;
        if (entry.offset == entry.entry->getSize()) {
          state.buffer.pop_front();
        }
      }
      KJ_ASSERT(remaining == 0);
      if (closing && partialRead != nullptr && &partialRead.assertLive() == &pending &&
          into.filled < into.elementSize) {
        // A fresh read at a fractional EOF returns an empty done view after Closed is published.
        into.filled = 0;
        return;
      }
      // A replacement view can change alignment without changing the remaining minimum.
      const auto minimum = into.atLeast > available ? into.atLeast - available : 0;
      const auto alignment = (into.elementSize - into.filled % into.elementSize) % into.elementSize;
      into.atLeast = complete ? 0 : kj::max(minimum, alignment);
      if (!complete) return;
    }

    // Retire all queue accounting before settlement; reentry may drain the same consumer.
    auto request = kj::mv(state.readRequests.front());
    state.readRequests.pop_front();
    request->resolve(js);
  }
}

void ByteQueue::handleRead(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    kj::Weak<ConsumerImpl> consumer,
    kj::Weak<QueueImpl>,
    kj::Own<ReadRequest> request) {
  auto pending = request->getWeak();
  state.readRequests.push_back(kj::mv(request));
  drain(js, consumer, pending,
      consumer.assertLive().isClosing() ? DrainMode::CLOSING : DrainMode::OPEN);
  if (pending == nullptr || consumer == nullptr) return;
  if (!consumer.assertLive().state.isActive() || consumer.assertLive().isClosing()) return;
  KJ_IF_SOME(c, consumer) {
    KJ_IF_SOME(listener, consume(kj::mv(c))->stateListener) {
      consume(kj::mv(listener))->onConsumerWantsData(js);
    }
  }
}

bool ByteQueue::handleMaybeClose(
    jsg::Lock& js, ConsumerImpl::Ready&, kj::Weak<ConsumerImpl> consumer) {
  drain(js, consumer, nullptr, DrainMode::CLOSING);
  KJ_IF_SOME(c, consumer) {
    if (!c->state.isActive()) return true;
    auto& ready = c->state.requireActiveUnsafe();
    if (!ready.readRequests.empty() && ready.readRequests.front()->responseInProgress) return false;
    return ready.queueTotalSize == 0;
  }
  return true;
}

kj::Maybe<kj::Own<ByteQueue::ByobRequest>> ByteQueue::nextPendingByobReadRequest(jsg::Lock& js) {
  // Preparing a view can settle reads and remove the controller's queue.
  auto queue = impl.addRef();
  while (true) {
    KJ_IF_SOME(state, queue->getState()) {
      if (state.pendingByobReadRequests.empty()) return kj::none;
      auto request = kj::mv(state.pendingByobReadRequests.front());
      state.pendingByobReadRequests.pop_front();
      if (request->prepareView(js)) {
        return kj::mv(request);
      }
    } else {
      return kj::none;
    }
  }
}

bool ByteQueue::hasPartiallyFulfilledRead() {
  KJ_IF_SOME(state, impl->getState()) {
    if (!state.pendingByobReadRequests.empty()) {
      auto& pending = state.pendingByobReadRequests.front();
      if (pending->isPartiallyFulfilled()) {
        return true;
      }
    }
  }
  return false;
}

bool ByteQueue::wantsRead() const {
  return impl->wantsRead();
}

size_t ByteQueue::getConsumerCount() {
  return impl->getConsumerCount();
}

void ByteQueue::visitForGc(jsg::GcVisitor& visitor) {}

#pragma endregion ByteQueue

}  // namespace workerd::api
