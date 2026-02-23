// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>

#include <kj/common.h>

#include <algorithm>

namespace workerd::api {

// ======================================================================================
// ValueQueue
#pragma region ValueQueue

#pragma region ValueQueue::ReadRequest

void ValueQueue::ReadRequest::resolveAsDone(jsg::Lock& js) {
  resolver.resolve(js, ReadResult{.done = true});
}

void ValueQueue::ReadRequest::resolve(jsg::Lock& js, jsg::Value value) {
  resolver.resolve(js, ReadResult{.value = kj::mv(value), .done = false});
}

void ValueQueue::ReadRequest::reject(jsg::Lock& js, jsg::Value& value) {
  resolver.reject(js, value.getHandle(js));
}

#pragma endregion ValueQueue::ReadRequest

#pragma region ValueQueue::Entry

ValueQueue::Entry::Entry(jsg::Value value, size_t size): value(kj::mv(value)), size(size) {}

jsg::Value ValueQueue::Entry::getValue(jsg::Lock& js) {
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
    ValueQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(queue.impl, stateListener) {}

ValueQueue::Consumer::Consumer(
    QueueImpl& impl, kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(impl, stateListener) {}

ValueQueue::Consumer::Consumer(kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(stateListener) {}

void ValueQueue::Consumer::cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  impl.cancel(js, maybeReason);
}

void ValueQueue::Consumer::close(jsg::Lock& js) {
  impl.close(js);
};

bool ValueQueue::Consumer::empty() {
  return impl.empty();
}

void ValueQueue::Consumer::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
};

void ValueQueue::Consumer::read(jsg::Lock& js, ReadRequest request) {
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
    jsg::Lock& js, kj::Maybe<ConsumerImpl::StateListener&> stateListener) {
  // If the queue was destroyed (e.g., stream was closed), we can still clone
  // the consumer - the cloneTo() will copy the closed/errored state.
  kj::Own<Consumer> consumer;
  KJ_IF_SOME(q, impl.queue) {
    consumer = kj::heap<Consumer>(q, stateListener);
  } else {
    consumer = kj::heap<Consumer>(stateListener);
  }
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
kj::Maybe<kj::Array<kj::byte>> valueToBytes(jsg::Lock& js, jsg::Value& value) {
  auto jsval = jsg::JsValue(value.getHandle(js));

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
  ConsumerImpl::UpdateBackpressureScope scope(impl);

  // Mark that we're doing a draining read. This allows onConsumerWantsData()
  // to use forcePull() which bypasses backpressure checks. The flag is cleared
  // either synchronously (for immediate returns) or in promise callbacks (for async).
  ready.hasPendingDrainingRead = true;

  // Collect all buffered data, converting values to bytes.
  kj::Vector<kj::Array<kj::byte>> chunks;
  bool isClosing = false;
  size_t totalRead = 0;

  // Drains buffered data, converting values to bytes. Returns a rejected promise if a value
  // cannot be converted; otherwise returns kj::none to indicate success.
  static const auto drainBuffer =
      [](jsg::Lock& js, ConsumerImpl& impl, ConsumerImpl::Ready& ready,
          kj::Vector<kj::Array<kj::byte>>& chunks, size_t& totalRead,
          bool& isClosing) -> kj::Maybe<jsg::Promise<DrainingReadResult>> {
    while (!ready.buffer.empty() && !isClosing) {
      auto& item = ready.buffer.front();
      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(close, ConsumerImpl::Close) {
          isClosing = true;
          break;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          auto value = entry.entry->getValue(js);
          KJ_IF_SOME(bytes, valueToBytes(js, value)) {
            totalRead += bytes.size();
            chunks.add(kj::mv(bytes));
            ready.queueTotalSize -= entry.entry->getSize();
            ready.buffer.pop_front();
          } else {
            auto error = js.typeError(
                "Draining read encountered a value that cannot be converted to bytes"_kj);
            impl.error(js, jsg::Value(js.v8Isolate, error));
            return js.rejectedPromise<DrainingReadResult>(error);
          }
        }
      }
    }
    return kj::none;
  };

  // Drain the buffer. Note: the initial buffer drain ignores maxRead - it always
  // drains all currently buffered data. maxRead only gates subsequent pump attempts.
  KJ_IF_SOME(errorPromise, drainBuffer(js, impl, ready, chunks, totalRead, isClosing)) {
    return kj::mv(errorPromise);
  }

  // Pump the controller for more synchronously available data.
  // maxRead is checked here: we only proceed with pumping if we haven't exceeded it.
  KJ_IF_SOME(listener, impl.stateListener) {
    while (!isClosing && totalRead < maxRead) {
      size_t prevChunkCount = chunks.size();
      bool pullCompletedSync = listener.onConsumerWantsData(js);

      // Drain all buffered data that was added by the pull.
      KJ_IF_SOME(errorPromise, drainBuffer(js, impl, ready, chunks, totalRead, isClosing)) {
        return kj::mv(errorPromise);
      }

      // If pull is async or no new data was added, stop pumping.
      if (!pullCompletedSync || chunks.size() == prevChunkCount) {
        break;
      }
    }
  }

  // If we collected data, return it immediately.
  if (!chunks.empty() || isClosing) {
    ready.hasPendingDrainingRead = false;
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
    listener.onConsumerWantsData(js);
  }

  // Transform the ReadResult promise to DrainingReadResult.
  return prp.promise.then(
      js, [this](jsg::Lock& js, ReadResult result) mutable -> DrainingReadResult {
    KJ_IF_SOME(ready, impl.state.tryGetActiveUnsafe()) {
      ready.hasPendingDrainingRead = false;
    }

    if (result.done) {
      return DrainingReadResult{.chunks = nullptr, .done = true};
    }

    // Convert the value to bytes.
    kj::Vector<kj::Array<kj::byte>> chunks;
    KJ_IF_SOME(val, result.value) {
      KJ_IF_SOME(bytes, valueToBytes(js, val)) {
        chunks.add(kj::mv(bytes));
      }
      // If valueToBytes returned kj::none, we just return empty chunks.
      // The error case should have been caught earlier.
    }

    return DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = false,
    };
  }, [this](jsg::Lock& js, jsg::Value exception) mutable -> DrainingReadResult {
    KJ_IF_SOME(ready, impl.state.tryGetActiveUnsafe()) {
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

ValueQueue::ValueQueue(size_t highWaterMark): impl(highWaterMark) {}

void ValueQueue::close(jsg::Lock& js) {
  impl.close(js);
}

ssize_t ValueQueue::desiredSize() const {
  return impl.desiredSize();
}

void ValueQueue::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ValueQueue::maybeUpdateBackpressure() {
  impl.maybeUpdateBackpressure();
}

void ValueQueue::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

size_t ValueQueue::size() const {
  return impl.size();
}

void ValueQueue::handlePush(
    jsg::Lock& js, ConsumerImpl::Ready& state, kj::Maybe<QueueImpl&> queue, kj::Rc<Entry> entry) {
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
    ConsumerImpl& consumer,
    kj::Maybe<QueueImpl&> queue,
    ReadRequest request) {
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
        request.resolveAsDone(js);
        return;
      }
      KJ_CASE_ONEOF(entry, QueueEntry) {
        auto freed = kj::mv(entry);
        state.buffer.pop_front();
        request.resolve(js, freed.entry->getValue(js));
        state.queueTotalSize -= freed.entry->getSize();
        return;
      }
    }
    KJ_UNREACHABLE;
  } else if (state.queueTotalSize == 0 && consumer.isClosing()) {
    // Otherwise, if state.queueTotalSize is zero and isClosing() is true there won't be any
    // more data coming. Just resolve the read as done and move on.
    request.resolveAsDone(js);
  } else {
    // Otherwise, push the read request into the pending readRequests. It will be
    // resolved either as soon as there is data available or the consumer closes
    // or errors.
    state.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));
    KJ_IF_SOME(listener, consumer.stateListener) {
      listener.onConsumerWantsData(js);
    }
  }
}

bool ValueQueue::handleMaybeClose(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    ConsumerImpl& consumer,
    kj::Maybe<QueueImpl&> queue) {
  // If the value queue is not yet empty we have to keep waiting for more reads to consume it.
  // Return false to indicate that we cannot close yet.
  return false;
}

size_t ValueQueue::getConsumerCount() {
  return impl.getConsumerCount();
}

bool ValueQueue::wantsRead() const {
  return impl.wantsRead();
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
void maybeInvalidateByobRequest(kj::Maybe<ByteQueue::ByobRequest&>& req) {
  KJ_IF_SOME(byobRequest, req) {
    byobRequest.invalidate();
    // The call to byobRequest->invalidate() should have cleared the reference.
    KJ_ASSERT(req == kj::none);
  }
}
}  // namespace

ByteQueue::ReadRequest::ReadRequest(
    jsg::Promise<ReadResult>::Resolver resolver, ByteQueue::ReadRequest::PullInto pullInto)
    : resolver(kj::mv(resolver)),
      pullInto(kj::mv(pullInto)) {}

ByteQueue::ReadRequest::~ReadRequest() noexcept(false) {
  maybeInvalidateByobRequest(byobReadRequest);
}

void ByteQueue::ReadRequest::resolveAsDone(jsg::Lock& js) {
  if (pullInto.filled > 0) {
    // There's been at least some data written, we need to respond but not
    // set done to true since that's what the streams spec requires.
    pullInto.store.trim(js, pullInto.store.size() - pullInto.filled);
    resolver.resolve(
        js, ReadResult{.value = js.v8Ref(pullInto.store.getHandle(js)), .done = false});
  } else {
    // Otherwise, we set the length to zero
    pullInto.store.trim(js, pullInto.store.size());
    KJ_ASSERT(pullInto.store.size() == 0);
    resolver.resolve(js, ReadResult{.value = js.v8Ref(pullInto.store.getHandle(js)), .done = true});
  }
  maybeInvalidateByobRequest(byobReadRequest);
}

void ByteQueue::ReadRequest::resolve(jsg::Lock& js) {
  pullInto.store.trim(js, pullInto.store.size() - pullInto.filled);
  resolver.resolve(js, ReadResult{.value = js.v8Ref(pullInto.store.getHandle(js)), .done = false});
  maybeInvalidateByobRequest(byobReadRequest);
}

void ByteQueue::ReadRequest::reject(jsg::Lock& js, jsg::Value& value) {
  resolver.reject(js, value.getHandle(js));
  maybeInvalidateByobRequest(byobReadRequest);
}

kj::Own<ByteQueue::ByobRequest> ByteQueue::ReadRequest::makeByobReadRequest(
    ConsumerImpl& consumer, QueueImpl& queue) {
  auto req = kj::heap<ByobRequest>(*this, consumer, queue);
  byobReadRequest = *req;
  return kj::mv(req);
}

#pragma endregion ByteQueue::ReadRequest

#pragma region ByteQueue::Entry

ByteQueue::Entry::Entry(jsg::BufferSource store): store(kj::mv(store)) {}

kj::ArrayPtr<kj::byte> ByteQueue::Entry::toArrayPtr() {
  return store.asArrayPtr();
}

size_t ByteQueue::Entry::getSize() const {
  return store.size();
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

ByteQueue::Consumer::Consumer(
    ByteQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(queue.impl, stateListener) {}

ByteQueue::Consumer::Consumer(
    QueueImpl& impl, kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(impl, stateListener) {}

ByteQueue::Consumer::Consumer(kj::Maybe<ConsumerImpl::StateListener&> stateListener)
    : impl(stateListener) {}

void ByteQueue::Consumer::cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
  impl.cancel(js, maybeReason);
}

void ByteQueue::Consumer::close(jsg::Lock& js) {
  impl.close(js);
}

bool ByteQueue::Consumer::empty() const {
  return impl.empty();
}

void ByteQueue::Consumer::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ByteQueue::Consumer::read(jsg::Lock& js, ReadRequest request) {
  impl.read(js, kj::mv(request));
}

void ByteQueue::Consumer::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

void ByteQueue::Consumer::reset() {
  impl.reset();
}

size_t ByteQueue::Consumer::size() const {
  return impl.size();
}

kj::Own<ByteQueue::Consumer> ByteQueue::Consumer::clone(
    jsg::Lock& js, kj::Maybe<ConsumerImpl::StateListener&> stateListener) {
  // If the queue was destroyed (e.g., stream was closed), we can still clone
  // the consumer - the cloneTo() will copy the closed/errored state.
  kj::Own<Consumer> consumer;
  KJ_IF_SOME(q, impl.queue) {
    consumer = kj::heap<Consumer>(q, stateListener);
  } else {
    consumer = kj::heap<Consumer>(stateListener);
  }
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
  ConsumerImpl::UpdateBackpressureScope scope(impl);

  // Mark that we're doing a draining read. This allows onConsumerWantsData()
  // to use forcePull() which bypasses backpressure checks. The flag is cleared
  // either synchronously (for immediate returns) or in promise callbacks (for async).
  ready.hasPendingDrainingRead = true;

  // Collect all buffered data (already bytes for ByteQueue).
  kj::Vector<kj::Array<kj::byte>> chunks;
  bool isClosing = false;
  size_t totalRead = 0;

  // Drains buffered byte data into chunks.
  static const auto drainBuffer = [](ConsumerImpl::Ready& ready,
                                      kj::Vector<kj::Array<kj::byte>>& chunks, size_t& totalRead,
                                      bool& isClosing) {
    while (!ready.buffer.empty() && !isClosing) {
      auto& item = ready.buffer.front();
      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(close, ConsumerImpl::Close) {
          isClosing = true;
          break;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          auto ptr = entry.entry->toArrayPtr();
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

  // Drain the buffer. Note: the initial buffer drain ignores maxRead - it always
  // drains all currently buffered data. maxRead only gates subsequent pump attempts.
  drainBuffer(ready, chunks, totalRead, isClosing);

  // Pump the controller for more synchronously available data.
  // maxRead is checked here: we only proceed with pumping if we haven't exceeded it.
  KJ_IF_SOME(listener, impl.stateListener) {
    while (!isClosing && totalRead < maxRead) {
      size_t prevChunkCount = chunks.size();
      bool pullCompletedSync = listener.onConsumerWantsData(js);

      // Drain all buffered data that was added by the pull.
      drainBuffer(ready, chunks, totalRead, isClosing);

      // If pull is async or no new data was added, stop pumping.
      if (!pullCompletedSync || chunks.size() == prevChunkCount) {
        break;
      }
    }
  }

  // If we collected data, return it immediately.
  if (!chunks.empty() || isClosing) {
    ready.hasPendingDrainingRead = false;
    return js.resolvedPromise(DrainingReadResult{
      .chunks = chunks.releaseAsArray(),
      .done = isClosing,
    });
  }

  // No data available - need to wait. Create a default read request.
  // We allocate a buffer for the read - the data will be copied into it.
  // The flag remains set (was set at the start) and will be cleared by the promise callbacks.
  constexpr size_t kDefaultReadSize = 16384;  // 16KB default buffer
  KJ_IF_SOME(store, jsg::BufferSource::tryAllocUnsafe(js, kDefaultReadSize)) {
    auto prp = js.newPromiseAndResolver<ReadResult>();

    ReadRequest::PullInto pullInto{
      .store = kj::mv(store),
      .filled = 0,
      .atLeast = 1,
      .type = ReadRequest::Type::DEFAULT,
    };
    ReadRequest request(kj::mv(prp.resolver), kj::mv(pullInto));
    ready.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));

    KJ_IF_SOME(listener, impl.stateListener) {
      listener.onConsumerWantsData(js);
    }

    // Transform the ReadResult promise to DrainingReadResult.
    return prp.promise.then(
        js, [this](jsg::Lock& js, ReadResult result) mutable -> DrainingReadResult {
      KJ_IF_SOME(ready, impl.state.tryGetActiveUnsafe()) {
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
    }, [this](jsg::Lock& js, jsg::Value exception) mutable -> DrainingReadResult {
      KJ_IF_SOME(ready, impl.state.tryGetActiveUnsafe()) {
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
    req.byobReadRequest = kj::none;
    request = kj::none;
  }
}

bool ByteQueue::ByobRequest::isPartiallyFulfilled() {
  return !isInvalidated() && getRequest().pullInto.filled > 0 &&
      getRequest().pullInto.store.getElementSize() > 1;
}

bool ByteQueue::ByobRequest::respond(jsg::Lock& js, size_t amount) {
  // So what happens here? The read request has been fulfilled directly by writing
  // into the storage buffer of the request. Unfortunately, this will only resolve
  // the data for the one consumer from which the request was received. We have to
  // copy the data into a refcounted ByteQueue::Entry that is pushed into the other
  // known consumers.

  // First, we check to make sure that the request hasn't been invalidated already.
  // Here, invalidated is a fancy word for the promise having been resolved or
  // rejected already.
  auto& req = KJ_REQUIRE_NONNULL(request, "the pending byob read request was already invalidated");

  // The amount cannot be more than the total space in the request store.
  JSG_REQUIRE(req.pullInto.filled + amount <= req.pullInto.store.size(), RangeError,
      kj::str("Too many bytes [", amount, "] in response to a BYOB read request."));

  auto sourcePtr = req.pullInto.store.asArrayPtr();

  if (queue.getConsumerCount() > 1) {
    // Allocate the entry into which we will be copying the provided data for the
    // other consumers of the queue.
    KJ_IF_SOME(store, jsg::BufferSource::tryAllocUnsafe(js, amount)) {
      auto entry = kj::rc<Entry>(kj::mv(store));

      auto start = sourcePtr.slice(req.pullInto.filled);

      // Safely copy the data over into the entry.
      entry->toArrayPtr().first(amount).copyFrom(start.first(amount));

      // Push the entry into the other consumers.
      queue.push(js, kj::mv(entry), consumer);
    } else {
      js.throwException(js.error("Failed to allocate memory for the byob read response."_kj));
    }
  }

  // For this consumer, if the number of bytes provided in the response does not
  // align with the element size of the read into buffer, we need to shave off
  // those extra bytes and push them into the consumers queue so they can be picked
  // up by the next read.
  req.pullInto.filled += amount;

  if (amount < req.pullInto.atLeast) {
    // The response has not yet met the minimal requirement of this byob read.
    // In this case, we do not want to resolve the read yet, and we do not
    // want the byob request to be invalidated. We don't need to worry about
    // unaligned bytes yet. We're just going to return false to tell the caller
    // not to invalidate and to update the view over this store.

    // We do want to decrease the atLeast by the amount of bytes we received.
    req.pullInto.atLeast -= amount;
    return false;
  }

  // There is no need to adjust the pullInto.atLeast here because we are resolving
  // the read immediately.

  auto unaligned = req.pullInto.filled % req.pullInto.store.getElementSize();
  // It is possible that the request was partially filled already.
  req.pullInto.filled -= unaligned;

  // Fulfill this request!
  consumer.resolveRead(js, req);

  if (unaligned > 0) {
    auto start = sourcePtr.slice(amount - unaligned);

    KJ_IF_SOME(store, jsg::BufferSource::tryAllocUnsafe(js, unaligned)) {
      auto excess = kj::rc<Entry>(kj::mv(store));
      excess->toArrayPtr().first(unaligned).copyFrom(start.first(unaligned));
      consumer.push(js, kj::mv(excess));
    } else {
      js.throwException(js.error("Failed to allocate memory for the byob read response."_kj));
    }
  }

  return true;
}

bool ByteQueue::ByobRequest::respondWithNewView(jsg::Lock& js, jsg::BufferSource view) {
  // The idea here is that rather than filling the view that the controller was given,
  // it chose to create its own view and fill that, likely over the same ArrayBuffer.
  // What we do here is perform some basic validations on what we were given, and if
  // those pass, we'll replace the backing store held in the req.pullInto with the one
  // given, then continue on issuing the respond as normal.
  auto& req = KJ_REQUIRE_NONNULL(request, "the pending byob read request was already invalidated");
  auto amount = view.size();

  JSG_REQUIRE(view.canDetach(js), TypeError, "Unable to use non-detachable ArrayBuffer.");
  JSG_REQUIRE(req.pullInto.store.getOffset() + req.pullInto.filled == view.getOffset(), RangeError,
      "The given view has an invalid byte offset.");
  JSG_REQUIRE(req.pullInto.store.size() == view.underlyingArrayBufferSize(js), RangeError,
      "The underlying ArrayBuffer is not the correct length.");
  JSG_REQUIRE(req.pullInto.filled + amount <= req.pullInto.store.size(), RangeError,
      "The view is not the correct length.");

  req.pullInto.store = jsg::BufferSource(js, view.detach(js));
  return respond(js, amount);
}

size_t ByteQueue::ByobRequest::getAtLeast() const {
  KJ_IF_SOME(req, request) {
    return req.pullInto.atLeast;
  }
  return 0;
}

v8::Local<v8::Uint8Array> ByteQueue::ByobRequest::getView(jsg::Lock& js) {
  KJ_IF_SOME(req, request) {
    return req.pullInto.store
        .getTypedViewSlice<v8::Uint8Array>(js, req.pullInto.filled, req.pullInto.store.size())
        .getHandle(js)
        .As<v8::Uint8Array>();
  }
  return v8::Local<v8::Uint8Array>();
}

size_t ByteQueue::ByobRequest::getOriginalBufferByteLength(jsg::Lock& js) const {
  KJ_IF_SOME(req, request) {
    KJ_IF_SOME(size, req.pullInto.store.underlyingArrayBufferSize(js)) {
      return size;
    }
  }
  return 0;
}

size_t ByteQueue::ByobRequest::getOriginalByteOffsetPlusBytesFilled() const {
  KJ_IF_SOME(req, request) {
    return req.pullInto.store.getOffset() + req.pullInto.filled;
  }
  return 0;
}

#pragma endregion ByteQueue::ByobRequest

ByteQueue::ByteQueue(size_t highWaterMark): impl(highWaterMark) {}

void ByteQueue::close(jsg::Lock& js) {
  // Note: We intentionally do NOT invalidate pending byob requests here.
  // According to the spec, the byobRequest should remain accessible after close
  // so that respondWithNewView() can be called on it (which should throw
  // appropriate errors for invalid views). The byob request will be invalidated
  // when respond() or respondWithNewView() is called.
  if (!FeatureFlags::get(js).getPedanticWpt()) {
    KJ_IF_SOME(ready, impl.state.tryGetUnsafe<ByteQueue::QueueImpl::Ready>()) {
      while (!ready.pendingByobReadRequests.empty()) {
        ready.pendingByobReadRequests.front()->invalidate();
        ready.pendingByobReadRequests.pop_front();
      }
    }
  }
  impl.close(js);
}

ssize_t ByteQueue::desiredSize() const {
  return impl.desiredSize();
}

void ByteQueue::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ByteQueue::maybeUpdateBackpressure() {
  KJ_IF_SOME(state, impl.getState()) {
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
  impl.maybeUpdateBackpressure();
}

void ByteQueue::push(jsg::Lock& js, kj::Rc<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

size_t ByteQueue::size() const {
  return impl.size();
}

void ByteQueue::handlePush(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    kj::Maybe<QueueImpl&> queue,
    kj::Rc<Entry> newEntry) {
  const auto bufferData = [&](size_t offset) {
    state.queueTotalSize += newEntry->getSize() - offset;
    state.buffer.emplace_back(QueueEntry{
      .entry = kj::mv(newEntry),
      .offset = offset,
    });
  };

  // If there are no pending reads add the entry to the buffer.
  if (state.readRequests.empty()) {
    return bufferData(0);
  }

  // Otherwise, check the the pending reads in the buffer. If the amount
  // of data in the queue + the amount of data provided by this entry
  // are >= the pending reads atLeast, then we will fulfill the pending
  // read, and keep fulfilling pending reads as long as they are available.
  // Once we are out of pending reads, we will buffer the remaining data.
  auto entrySize = newEntry->getSize();
  auto amountAvailable = state.queueTotalSize + entrySize;
  size_t entryOffset = 0;

  while (!state.readRequests.empty() && amountAvailable > 0) {
    auto& pending = *state.readRequests.front();

    // If the amountAvailable is less than the pending read request's atLeast,
    // then we're just going to buffer the data and bailout without fulfilling
    // the read. We will take care of fulfilling the read later once there
    // is enough data.

    if (amountAvailable < pending.pullInto.atLeast) {
      return bufferData(0);
    }

    // There might be at least some data in the buffer. If there is, it should
    // not be more than the current pending.pullInfo.atLeast or something went
    // wrong somewhere else.
    KJ_REQUIRE(state.queueTotalSize < pending.pullInto.atLeast);

    // First, we copy any data in the buffer out to the pending.pullInto. This
    // should completely consume the current buffer.
    while (!state.buffer.empty()) {
      auto& next = state.buffer.front();
      KJ_SWITCH_ONEOF(next) {
        KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
          // This should have been caught by the isClosing() check above.
          KJ_FAIL_ASSERT("The consumer is closed.");
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          auto sourcePtr = entry.entry->toArrayPtr();
          auto sourceSize = sourcePtr.size() - entry.offset;

          auto destPtr = pending.pullInto.store.asArrayPtr().slice(pending.pullInto.filled);
          auto destAmount = pending.pullInto.store.size() - pending.pullInto.filled;

          // sourceSize is the amount of data remaining in the current entry to copy.
          // destAmount is the amount of space remaining to be filled in the pending read.
          // Because destAmount should be greater than or equal to atLeast, and because we
          // already checked that the queueTotalSize is less than atLeast, it should not be
          // possible for sourceSize to be zero nor greater than or equal to destAmount,
          // so let's verify.
          KJ_REQUIRE(sourceSize > 0 && sourceSize < destAmount);

          // Safely copy sourceSize bytes from sourcePtr to destPtr
          destPtr.first(sourceSize).copyFrom(sourcePtr.slice(entry.offset));

          // We have completely consumed the data in this entry and can safely free
          // our reference to it now. Yay!
          auto released = kj::mv(next);
          state.buffer.pop_front();

          pending.pullInto.filled += sourceSize;

          // There is no reason to adjust the pullInto.atLeast here because we
          // will be immediately resolving the read in the next step.

          state.queueTotalSize -= sourceSize;
          amountAvailable -= sourceSize;
        }
      }
    }

    // At this point, there shouldn't be any data remaining in the buffer.
    KJ_REQUIRE(state.queueTotalSize == 0);

    // And there should be data remaining in the pending pullInto destination.
    KJ_REQUIRE(pending.pullInto.filled < pending.pullInto.store.size());

    // And the amountAvailable should be equal to the current push size.
    KJ_REQUIRE(amountAvailable == entrySize - entryOffset);

    // Now, we determine how much of the current entry we can copy into the
    // destination pullInto by taking the lesser of amountAvailable and
    // destination pullInto size - filled (which gives us the amount of space
    // remaining in the destination).
    auto amountToCopy =
        kj::min(amountAvailable, pending.pullInto.store.size() - pending.pullInto.filled);

    // The amountToCopy should not be more than the entry size minus the entryOffset
    // (which is the amount of data remaining to be consumed in the current entry).
    KJ_REQUIRE(amountToCopy <= entrySize - entryOffset);

    // The amountToCopy plus pending.pullInto.filled should be more than or equal to atLeast
    // and less than or equal pending.pullInto.store.size().
    KJ_REQUIRE(amountToCopy + pending.pullInto.filled >= pending.pullInto.atLeast &&
        amountToCopy + pending.pullInto.filled <= pending.pullInto.store.size());

    // Awesome, so now we safely copy amountToCopy bytes from the current entry into
    // the remaining space in pending.pullInto.store, being careful to account for
    // the entryOffset and pending.pullInto.filled offsets to determine the range
    // where we start copying.
    auto entryPtr = newEntry->toArrayPtr();
    auto destPtr = pending.pullInto.store.asArrayPtr().slice(pending.pullInto.filled);
    destPtr.first(amountToCopy).copyFrom(entryPtr.slice(entryOffset).first(amountToCopy));

    // Yay! this pending read has been fulfilled. There might be more tho. Let's adjust
    // the amountAvailable and continue trying to consume data.
    amountAvailable -= amountToCopy;
    entryOffset += amountToCopy;
    pending.pullInto.filled += amountToCopy;

    // We do not need to adjust the pullInto.atLeast here since we are immediately
    // fulfilling the read at this point.

    auto request = kj::mv(state.readRequests.front());
    state.readRequests.pop_front();
    request->resolve(js);
  }

  // If the entry was consumed completely by the pending read, then we're done!
  // We don't have to buffer any data and shouldn't have any data in the buffer!
  // Since we possibly consumed data from the buffer, however, let's make sure
  // we tell the queue to update backpressure signaling.
  if (entryOffset == entrySize) {
    KJ_REQUIRE(state.queueTotalSize == 0);
    return;
  }

  // Otherwise, we need to buffer the remaining data, being careful to set the offset
  // for the data that we have already consumed.
  bufferData(entryOffset);
}

void ByteQueue::handleRead(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    ConsumerImpl& consumer,
    kj::Maybe<QueueImpl&> queue,
    ReadRequest request) {
  const auto pendingRead = [&]() {
    bool isByob = request.pullInto.type == ReadRequest::Type::BYOB;
    state.readRequests.push_back(kj::heap<ReadRequest>(kj::mv(request)));
    if (isByob) {
      // Because ReadRequest is movable, and because the ByobRequest captures
      // a reference to the ReadRequest, we wait until after it is added to
      // state.readRequests to create the associated ByobRequest.
      // If the queue is none, the consumer was cloned from a closed stream
      // and we can't create a ByobRequest. If the queue state is none,
      // the queue has already been closed.
      KJ_IF_SOME(q, queue) {
        KJ_IF_SOME(queueState, q.getState()) {
          queueState.pendingByobReadRequests.push_back(
              state.readRequests.back()->makeByobReadRequest(consumer, q));
        }
      }
    }
    KJ_IF_SOME(listener, consumer.stateListener) {
      listener.onConsumerWantsData(js);
    }
  };

  const auto consume = [&](size_t amountToConsume) {
    while (amountToConsume > 0) {
      KJ_REQUIRE(!state.buffer.empty());
      // There must be at least one item in the buffer.
      auto& item = state.buffer.front();

      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
          // We reached the end of the buffer! All data has been consumed.
          return true;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          // The amount to copy is the lesser of the current entry size minus
          // offset and the data remaining in the destination to fill.
          auto entrySize = entry.entry->getSize();
          auto amountToCopy = kj::min(
              entrySize - entry.offset, request.pullInto.store.size() - request.pullInto.filled);
          auto elementSize = request.pullInto.store.getElementSize();
          if (amountToCopy > elementSize) {
            amountToCopy -= amountToCopy % elementSize;
          }
          if (amountToConsume > elementSize) {
            amountToConsume -= amountToConsume % elementSize;
          }

          // Once we have the amount, we safely copy amountToCopy bytes from the
          // entry into the destination request, accounting properly for the offsets.
          auto sourcePtr = entry.entry->toArrayPtr().slice(entry.offset);
          auto destPtr = request.pullInto.store.asArrayPtr().slice(request.pullInto.filled);

          destPtr.first(amountToCopy).copyFrom(sourcePtr.first(amountToCopy));

          request.pullInto.filled += amountToCopy;

          // If pullInto.atLeast is greater than amountToCopy, let's adjust
          // atLeast down by the number of bytes we've consumed, indicating
          // a smaller minimum read requirement.
          if (request.pullInto.atLeast > amountToCopy) {
            request.pullInto.atLeast -= amountToCopy;
          } else if (request.pullInto.atLeast == amountToCopy) {
            request.pullInto.atLeast = 1;
          }
          entry.offset += amountToCopy;
          amountToConsume -= amountToCopy;
          state.queueTotalSize -= amountToCopy;

          // If the entry.offset is equal to the size of the entry, then we've consumed the
          // entire thing and can free it and continue iterating. The amountToConsume might
          // be >= 0, we will check it at the start of the next iteration.
          if (entry.offset == entrySize) {
            auto released = kj::mv(item);
            state.buffer.pop_front();
            continue;
          }

          // Otherwise, it is OK that there is data remaining but the amountToConsume
          // should be 0. Specifically, we either consume the entire entry and there
          // is data left over to consume, or we did not consume the entire entry
          // but read all that we can.
          KJ_REQUIRE(amountToConsume == 0);
        }
      }
    }
    return false;
  };

  // If there are no pending read requests and there is data in the buffer,
  // we will try to fulfill the read request immediately.
  if (state.readRequests.empty() && state.queueTotalSize > 0) {
    // If the available size is less than the read requests atLeast, then
    // push the read request into the pending so we can wait for more data...

    if (state.queueTotalSize < request.pullInto.atLeast) {
      // If there is anything in the consumers queue at this point, We need to
      // copy those bytes into the byob buffer and advance the filled counter
      // forward that number of bytes.
      if (state.queueTotalSize > 0 && consume(state.queueTotalSize)) {
        return request.resolveAsDone(js);
      }
      return pendingRead();
    }

    // Awesome, ok, it looks like we have enough data in the queue for us
    // to minimally fill this read request! The amount to copy is the lesser
    // of the queue total size and the maximum amount of space in the request
    // pull into.
    if (consume(kj::min(state.queueTotalSize, request.pullInto.store.size()))) {

      // If consume returns true, the consumer hit the end and we need to
      // just resolve the request as done and return.
      return request.resolveAsDone(js);
    }

    // Now, we can resolve the read promise. Since we consumed data from the
    // buffer, we also want to make sure to notify the queue so it can update
    // backpressure signaling.
    request.resolve(js);
  } else if (state.queueTotalSize == 0 && consumer.isClosing()) {
    // Otherwise, if size() is zero and isClosing() is true, we should have already
    // drained but let's take care of that now. Specifically, in this case there's
    // no data in the queue and close() has already been called, so there won't be
    // any more data coming.
    request.resolveAsDone(js);
  } else {
    // Otherwise, push the read request into the pending readRequests. It will be
    // resolved either as soon as there is data available or the consumer closes
    // or errors.
    return pendingRead();
  }
}

bool ByteQueue::handleMaybeClose(jsg::Lock& js,
    ConsumerImpl::Ready& state,
    ConsumerImpl& consumer,
    kj::Maybe<QueueImpl&> queue) {
  // This is called when we know that we are closing and we still have data in
  // the queue. We want to see if we can drain as much of it into pending reads
  // as possible. If we're able to drain all of it, then yay! We can go ahead and
  // close. Otherwise we stay open and wait for more reads to consume the rest.

  // We should only be here if there is data remaining in the queue.
  KJ_ASSERT(state.queueTotalSize > 0);

  // We should also only be here if the consumer is closing.
  KJ_ASSERT(consumer.isClosing());

  const auto consume = [&] {
    // Consume will copy as much of the remaining data in the buffer as possible
    // to the next pending read. If the remaining data can fit into the remaining
    // space in the read, awesome, we've consumed everything and we will return
    // true. If the remaining data cannot fit into the remaining space in the read,
    // then we'll return false to indicate that there's more data to consume. In
    // either case, the pending read is popped off the pending queue and resolved.

    KJ_ASSERT(!state.readRequests.empty());
    auto& pending = *state.readRequests.front();

    while (!state.buffer.empty()) {
      auto& next = state.buffer.front();
      KJ_SWITCH_ONEOF(next) {
        KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
          // We've reached the end! queueTotalSize should be zero. We need to
          // resolve and pop the current read and return true to indicate that
          // we're all done.
          //
          // Technically, we really shouldn't get here but the case is covered
          // just in case.
          KJ_ASSERT(state.queueTotalSize == 0);
          auto request = kj::mv(state.readRequests.front());
          state.readRequests.pop_front();
          request->resolve(js);
          return true;
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          auto sourcePtr = entry.entry->toArrayPtr();
          auto sourceSize = sourcePtr.size() - entry.offset;

          auto destPtr = pending.pullInto.store.asArrayPtr().slice(pending.pullInto.filled);
          auto destAmount = pending.pullInto.store.size() - pending.pullInto.filled;

          // There should be space available to copy into and data to copy from, or
          // something else went wrong.
          KJ_ASSERT(destAmount > 0);
          KJ_ASSERT(sourceSize > 0);

          // sourceSize is the amount of data remaining in the current entry to copy.
          // destAmount is the amount of space remaining to be filled in the pending read.
          auto amountToCopy = kj::min(sourceSize, destAmount);

          auto sourceStart = sourcePtr.slice(entry.offset);

          // It shouldn't be possible for sourceEnd to extend past the sourcePtr.end()
          // but let's make sure just to be safe.
          KJ_ASSERT(amountToCopy <= sourceStart.size());

          // Safely copy amountToCopy bytes from the source into the destination.
          destPtr.first(amountToCopy).copyFrom(sourceStart.first(amountToCopy));
          pending.pullInto.filled += amountToCopy;

          // We do not need to adjust down the atLeast here because, no matter what,
          // the read is going to be resolved either here or in the next iteration.

          state.queueTotalSize -= amountToCopy;
          entry.offset += amountToCopy;

          KJ_ASSERT(entry.offset <= sourcePtr.size());

          if (amountToCopy == sourcePtr.size()) {
            // If amountToCopy is equal to sourcePtr.size(), we've consumed the entire entry
            // and we can free it.
            auto released = kj::mv(next);
            state.buffer.pop_front();

            if (amountToCopy == destAmount) {
              // If the amountToCopy is equal to destAmount, then we've completely filled
              // this read request with the data remaining. Resolve the read request. If
              // state.queueTotalSize happens to be zero, we can safely indicate that we
              // have read the remaining data as this may have been the last actual value
              // entry in the buffer.
              auto request = kj::mv(state.readRequests.front());
              state.readRequests.pop_front();
              request->resolve(js);

              if (state.queueTotalSize == 0) {
                // If the queueTotalSize is zero at this point, the next item in the queue
                // must be a close and we can return true. All of the data has been consumed.
                KJ_ASSERT(state.buffer.front().is<ConsumerImpl::Close>());
                return true;
              }

              // Otherwise, there's still data to consume, return false here to move on
              // to the next pending read (if any).
              return false;
            }

            // We know that amountToCopy cannot be greater than destAmount because
            // of the kj::min above.

            // Continuing here means that our pending read still has space to fill
            // and we might still have value entries to fill it. We'll iterate around
            // and see where we get.
            continue;
          }

          // This read did not consume everything in this entry but doesn't have
          // any more space to fill. We will resolve this read and return false
          // to indicate that the outer loop should continue with the next read
          // request if there is one.

          // At this point, it should be impossible for state.queueTotalSize to
          // be zero because there is still data remaining to be consumed in this
          // buffer.
          KJ_ASSERT(state.queueTotalSize > 0);

          auto request = kj::mv(state.readRequests.front());
          state.readRequests.pop_front();
          request->resolve(js);
          return false;
        }
      }
    }

    return state.queueTotalSize == 0;
  };

  // We can only consume here if there are pending reads!
  while (!state.readRequests.empty()) {
    // We ignore the read request atLeast here since we are closing. Our goal is to
    // consume as much of the data as possible.

    if (consume()) {
      // If consume returns true, we reached the end and have no more data to
      // consume. That's a good thing! It means we can go ahead and close down.
      return true;
    }

    // If consume() returns false, there is still data left to consume in the queue.
    // We will loop around and try again so long as there are still read requests
    // pending.
  }

  // At this point, we shouldn't have any read requests and there should be data
  // left in the queue. We have to keep waiting for more reads to consume the
  // remaining data.
  KJ_ASSERT(state.queueTotalSize > 0);
  KJ_ASSERT(state.readRequests.empty());

  return false;
}

kj::Maybe<kj::Own<ByteQueue::ByobRequest>> ByteQueue::nextPendingByobReadRequest() {
  KJ_IF_SOME(state, impl.getState()) {
    while (!state.pendingByobReadRequests.empty()) {
      auto request = kj::mv(state.pendingByobReadRequests.front());
      state.pendingByobReadRequests.pop_front();
      if (!request->isInvalidated()) {
        return kj::mv(request);
      }
    }
  }
  return kj::none;
}

bool ByteQueue::hasPartiallyFulfilledRead() {
  KJ_IF_SOME(state, impl.getState()) {
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
  return impl.wantsRead();
}

size_t ByteQueue::getConsumerCount() {
  return impl.getConsumerCount();
}

void ByteQueue::visitForGc(jsg::GcVisitor& visitor) {}

#pragma endregion ByteQueue

}  // namespace workerd::api
