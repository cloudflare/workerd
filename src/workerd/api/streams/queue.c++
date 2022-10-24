// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"
#include <algorithm>

namespace workerd::api {

// ======================================================================================
// ValueQueue
#pragma region ValueQueue

#pragma region ValueQueue::ReadRequest

void ValueQueue::ReadRequest::resolveAsDone(jsg::Lock&) {
  resolver.resolve(ReadResult { .done = true });
}

void ValueQueue::ReadRequest::resolve(jsg::Lock&, jsg::Value value) {
  resolver.resolve(ReadResult { .value = kj::mv(value), .done = false });
}

void ValueQueue::ReadRequest::reject(jsg::Lock& js, jsg::Value& value) {
  resolver.reject(value.getHandle(js));
}

#pragma endregion ValueQueue::ReadRequest

#pragma region ValueQueue::Entry

ValueQueue::Entry::Entry(jsg::Value value, size_t size)
    : value(kj::mv(value)), size(size) {}

jsg::Value ValueQueue::Entry::getValue(jsg::Lock& js) {
  return value.addRef(js);
}

size_t ValueQueue::Entry::getSize() const { return size; }

void ValueQueue::Entry::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(value);
}

ValueQueue::Entry ValueQueue::Entry::clone(jsg::Lock& js) {
  return Entry(value.addRef(js), size);
}

#pragma endregion ValueQueue::Entry

#pragma region ValueQueue::QueueEntry

ValueQueue::QueueEntry ValueQueue::QueueEntry::clone() {
  return QueueEntry {
    .entry = kj::addRef(*entry),
  };
}

#pragma endregion ValueQueue::QueueEntry

#pragma region ValueQueue::Consumer

ValueQueue::Consumer::Consumer(ValueQueue& queue) : impl(queue.impl) {}

ValueQueue::Consumer::Consumer(QueueImpl& impl) : impl(impl) {}

void ValueQueue::Consumer::close(jsg::Lock& js) { impl.close(js); };

bool ValueQueue::Consumer::empty() { return impl.empty(); }

void ValueQueue::Consumer::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
};

void ValueQueue::Consumer::read(jsg::Lock& js, ReadRequest request) {
  impl.read(js, kj::mv(request));
}

void ValueQueue::Consumer::push(jsg::Lock& js, kj::Own<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

void ValueQueue::Consumer::reset() { impl.reset(); };

size_t ValueQueue::Consumer::size() { return impl.size(); }

kj::Own<ValueQueue::Consumer> ValueQueue::Consumer::clone(jsg::Lock& js) {
  auto consumer = kj::heap<Consumer>(impl.queue);
  impl.cloneTo(js, consumer->impl);
  return kj::mv(consumer);
}

#pragma endregion ValueQueue::Consumer

ValueQueue::ValueQueue(size_t highWaterMark) : impl(highWaterMark) {}

void ValueQueue::close(jsg::Lock& js) {
  impl.close(js);
}

ssize_t ValueQueue::desiredSize() const { return impl.desiredSize(); }

void ValueQueue::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ValueQueue::maybeUpdateBackpressure() { impl.maybeUpdateBackpressure(); }

void ValueQueue::push(jsg::Lock& js, kj::Own<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

size_t ValueQueue::size() const { return impl.size(); }

void ValueQueue::handlePush(
    jsg::Lock& js,
    ConsumerImpl::Ready& state,
    QueueImpl& queue,
    kj::Own<Entry> entry) {
  // If there are no pending reads, just add the entry to the buffer and return, adjusting
  // the size of the queue in the process.
  if (state.readRequests.empty()) {
    state.queueTotalSize += entry->getSize();
    state.buffer.push_back(QueueEntry { .entry = kj::mv(entry) });
    return;
  }

  // Otherwise, pop the next pending read and resolve it. There should be nothing in the queue.
  KJ_REQUIRE(state.buffer.empty() && state.queueTotalSize == 0);
  auto pending = kj::mv(state.readRequests.front());
  state.readRequests.pop_front();
  pending.resolve(js, entry->getValue(js));
}

void ValueQueue::handleRead(
    jsg::Lock& js,
    ConsumerImpl::Ready& state,
    ConsumerImpl& consumer,
    QueueImpl& queue,
    ReadRequest request) {
  // If there are no pending read requests and there is data in the buffer,
  // we will try to fulfill the read request immediately.
  if (state.readRequests.empty() && state.queueTotalSize > 0) {
    auto entry = kj::mv(state.buffer.front());
    state.buffer.pop_front();

    KJ_SWITCH_ONEOF(entry) {
      KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
        // The next item was a close sentinel! Resolve the read immediately with a close indicator.
        request.resolveAsDone(js);
        state.readRequests.pop_front();
      }
      KJ_CASE_ONEOF(entry, QueueEntry) {
        request.resolve(js, entry.entry->getValue(js));
        state.queueTotalSize -= entry.entry->getSize();
      }
    }
  } else if (state.queueTotalSize == 0 && consumer.isClosing()) {
    // Otherwise, if state.queueTotalSize is zero and isClosing() is true, we should
    // have already drained but let's take care of that now. Specifically, in this case
    // there's no data in the queue and close() has already been called, so there won't
    // be any more data coming.
    request.resolveAsDone(js);
  } else {
    // Otherwise, push the read request into the pending readRequests. It will be
    // resolved either as soon as there is data available or the consumer closes
    // or errors.
    state.readRequests.push_back(kj::mv(request));
  }
}

#pragma endregion ValueQueue

// ======================================================================================
// ByteQueue
#pragma region ByteQueue

#pragma region ByteQueue::ReadRequest

void ByteQueue::ReadRequest::resolveAsDone(jsg::Lock& js) {
  pullInto.store.trim(pullInto.store.size() - pullInto.filled);
  resolver.resolve(ReadResult {
    .value = js.v8Ref(pullInto.store.createHandle(js)),
    .done = true
  });
  KJ_IF_MAYBE(byobRequest, byobReadRequest) {
    byobRequest->invalidate();
    byobReadRequest = nullptr;
  }
}

void ByteQueue::ReadRequest::resolve(jsg::Lock& js) {
  pullInto.store.trim(pullInto.store.size() - pullInto.filled);
  resolver.resolve(ReadResult {
    .value = js.v8Ref(pullInto.store.createHandle(js)),
    .done = false
  });
  KJ_IF_MAYBE(byobRequest, byobReadRequest) {
    byobRequest->invalidate();
    byobReadRequest = nullptr;
  }
}

void ByteQueue::ReadRequest::reject(jsg::Lock& js, jsg::Value& value) {
  resolver.reject(value.getHandle(js));
  KJ_IF_MAYBE(byobRequest, byobReadRequest) {
    byobRequest->invalidate();
    byobReadRequest = nullptr;
  }
}

#pragma endregion ByteQueue::ReadRequest

#pragma region ByteQueue::Entry

ByteQueue::Entry::Entry(jsg::BackingStore store) : store(kj::mv(store)) {}

kj::ArrayPtr<kj::byte> ByteQueue::Entry::toArrayPtr() { return store.asArrayPtr(); }

size_t ByteQueue::Entry::getSize() const { return store.size(); }

#pragma endregion ByteQueue::Entry

#pragma region ByteQueue::QueueEntry

ByteQueue::QueueEntry ByteQueue::QueueEntry::clone() {
  return QueueEntry {
    .entry = kj::addRef(*entry),
    .offset = offset,
  };
}

#pragma endregion ByteQueue::QueueEntry

#pragma region ByteQueue::Consumer

ByteQueue::Consumer::Consumer(ByteQueue& queue) : impl(queue.impl) {}

ByteQueue::Consumer::Consumer(QueueImpl& impl) : impl(impl) {}

void ByteQueue::Consumer::close(jsg::Lock& js) { impl.close(js); }

bool ByteQueue::Consumer::empty() const { return impl.empty(); }

void ByteQueue::Consumer::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ByteQueue::Consumer::read(jsg::Lock& js, ReadRequest request) {
  impl.read(js, kj::mv(request));
}

void ByteQueue::Consumer::push(jsg::Lock& js, kj::Own<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

void ByteQueue::Consumer::reset() { impl.reset(); }

size_t ByteQueue::Consumer::size() const { return impl.size(); }

kj::Own<ByteQueue::Consumer> ByteQueue::Consumer::clone(jsg::Lock& js) {
  auto consumer = kj::heap<Consumer>(impl.queue);
  impl.cloneTo(js, consumer->impl);
  return kj::mv(consumer);
}

#pragma endregion ByteQueue::Consumer

#pragma region ByteQueue::ByobReadRequest

void ByteQueue::ByobReadRequest::respond(jsg::Lock& js, size_t amount) {
  // So what happens here? The read request has been fulfilled directly by writing
  // into the storage buffer of the request. Unfortunately, this will only resolve
  // the data for the one consumer from which the request was received. We have to
  // copy the data into a refcounted ByteQueue::Entry that is pushed into the other
  // known consumers.

  // First, we check to make sure that the request hasn't been invalidated already.
  // Here, invalidated is a fancy word for the promise having been resolved or
  // rejected already.
  auto& req = KJ_REQUIRE_NONNULL(request, "the pending byob read request was already invalidated");

  // It is possible that the request was partially filled already.
  req.pullInto.filled += amount;

  // The amount cannot be more than the total space in the request store.
  KJ_REQUIRE(req.pullInto.filled <= req.pullInto.store.size());

  // Allocate the entry into which we will be copying the provided data.
  auto entry = kj::refcounted<Entry>(jsg::BackingStore::alloc(js, req.pullInto.filled));

  // Safely copy the data over into the entry.
  auto sourcePtr = req.pullInto.store.asArrayPtr();
  std::copy(sourcePtr.begin(),
            sourcePtr.begin() + req.pullInto.filled,
            entry->toArrayPtr().begin());

  // Push the entry into the other consumers.
  queue.push(js, kj::mv(entry), consumer);

  // Fullfill this request!
  consumer.resolveRead(js, req);
}

#pragma endregion ByteQueue::ByobReadRequest

ByteQueue::ByteQueue(size_t highWaterMark) : impl(highWaterMark) {}

void ByteQueue::close(jsg::Lock& js) { impl.close(js); }

ssize_t ByteQueue::desiredSize() const { return impl.desiredSize(); }

void ByteQueue::error(jsg::Lock& js, jsg::Value reason) {
  impl.error(js, kj::mv(reason));
}

void ByteQueue::maybeUpdateBackpressure() {
  KJ_IF_MAYBE(state, impl.getState()) {
    // Invalidated byob read requests will accumulate if we do not take
    // take of them from time to time since. Since maybeUpdateBackpressure
    // is going to be called regularly while the queue is actively in use,
    // this is as good a place to clean them out as any.
    auto pivot KJ_UNUSED = std::remove_if(
        state->pendingByobReadRequests.begin(),
        state->pendingByobReadRequests.end(),
        [](auto& item) {
      return item->isInvalidated();
    });
  }
  impl.maybeUpdateBackpressure();
}

void ByteQueue::push(jsg::Lock& js, kj::Own<Entry> entry) {
  impl.push(js, kj::mv(entry));
}

size_t ByteQueue::size() const { return impl.size(); }

void ByteQueue::handlePush(
    jsg::Lock& js,
    ConsumerImpl::Ready& state,
    QueueImpl& queue,
    kj::Own<Entry> newEntry) {
  const auto bufferData = [&](size_t offset) {
    state.queueTotalSize += newEntry->getSize() - offset;
    state.buffer.emplace_back(QueueEntry {
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
    auto& pending = state.readRequests.front();

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

          auto destPtr = pending.pullInto.store.asArrayPtr().begin() + pending.pullInto.filled;
          auto destAmount = pending.pullInto.store.size() -  pending.pullInto.filled;

          // sourceSize is the amount of data remaining in the current entry to copy.
          // destAmount is the amount of space remaining to be filled in the pending read.
          // Because destAmount should be greater than or equal to atLeast, and because we
          // already checked that the queueTotalSize is less than atLeast, it should not be
          // possible for sourceSize to be zero nor greater than or equal to destAmount,
          // so let's verify.
          KJ_REQUIRE(sourceSize > 0 && sourceSize < destAmount);

          // Safely copy amountToCopy bytes from sourcePtr to destPtr
          std::copy(sourcePtr.begin() + entry.offset, sourcePtr.end(), destPtr);

          // We have completely consumed the data in this entry and can safely free
          // our reference to it now. Yay!
          auto released = kj::mv(next);
          state.buffer.pop_front();

          pending.pullInto.filled += sourceSize;
          state.queueTotalSize -= sourceSize;
          amountAvailable -= sourceSize;

          // It shouldn't be possible for us to have already met the atLeast requirement
          // for the pending read here!
          KJ_REQUIRE(pending.pullInto.filled < pending.pullInto.atLeast);
        }
      }
    }

    // At this point, there shouldn't be any data remaining in the buffer.
    KJ_REQUIRE(state.queueTotalSize == 0);

    // And there should be data remaining in the pending pullInto destination.
    KJ_REQUIRE(pending.pullInto.filled < pending.pullInto.store.size());

    // And the pending.pullInfo.filled should still be below the pendingPullInto.atLeast.
    KJ_REQUIRE(pending.pullInto.filled < pending.pullInto.atLeast);

    // And the amountAvailable should be equal to the current push size.
    KJ_REQUIRE(amountAvailable == entrySize);

    // Now, we determine how much of the current entry we can copy into the
    // destination pullInto by taking the lesser of amountAvailable and
    // destination pullInto size - filled (which gives us the amount of space
    // remaining in the destination).
    auto amountToCopy = kj::min(amountAvailable,
                                pending.pullInto.store.size() - pending.pullInto.filled);

    // The amountToCopy should not be more than the entry size minus the entryOffset
    // (which is the amount of data remaining to be consumed in the current entry).
    KJ_REQUIRE(amountToCopy <= entrySize - entryOffset);

    // The amountToCopy plus pending.pullInto.filled should be more than or equal to atLeast
    // and less than or equal pending.pullInto.store.size().
    KJ_REQUIRE(amountToCopy + pending.pullInto.filled >= pending.pullInto.atLeast &&
               amountToCopy + pending.pullInto.filled <= pending.pullInto.store.size());

    // Awesome, so now we safely copy amountToCopy bytes from the current entry into
    // the remaining space in pending.pullInto.store, befing careful to account for
    // the entryOffset and pending.pullInto.filled offsets to determine the range
    // where we start copying.
    auto entryPtr = newEntry->toArrayPtr();
    auto destPtr = pending.pullInto.store.asArrayPtr().begin() + pending.pullInto.filled;
    std::copy(entryPtr.begin() + entryOffset,
              entryPtr.begin() + entryOffset + amountToCopy,
              destPtr);

    // Yay! this pending read has been fulfilled. There might be more tho. Let's adjust
    // the amountAvailable and continue trying to consume data.
    amountAvailable -= amountToCopy;
    entryOffset += amountToCopy;
    pending.pullInto.filled += amountToCopy;
    auto released = kj::mv(pending);
    state.readRequests.pop_front();
    released.resolve(js);
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

void ByteQueue::handleRead(
    jsg::Lock& js,
    ConsumerImpl::Ready& state,
    ConsumerImpl& consumer,
    QueueImpl& queue,
    ReadRequest request) {
  const auto pendingRead = [&]() {
    bool isByob = request.pullInto.type == ReadRequest::Type::BYOB;
    state.readRequests.push_back(kj::mv(request));
    if (isByob) {
      KJ_REQUIRE_NONNULL(queue.getState()).pendingByobReadRequests.push_back(
          kj::heap<ByobReadRequest>(state.readRequests.back(), consumer, queue));
    }
  };

  // If there are no pending read requests and there is data in the buffer,
  // we will try to fulfill the read request immediately.
  if (state.readRequests.empty() && state.queueTotalSize > 0) {
    // If the available size is less than the read requests atLeast, then
    // push the read request into the pending so we can wait for more data.
    if (state.queueTotalSize < request.pullInto.atLeast) {
      return pendingRead();
    }

    // Awesome, ok, it looks like we have enough data in the queue for us
    // to minimally fill this read request! The amount to copy is the lesser
    // of the queue total size and the maximum amount of space in the request
    // pull into.
    auto amountToConsume = kj::min(state.queueTotalSize, request.pullInto.store.size());

    while (amountToConsume > 0) {
      KJ_REQUIRE(!state.buffer.empty());
      // There must be at least one item in the buffer.
      auto& item = state.buffer.front();

      KJ_SWITCH_ONEOF(item) {
        KJ_CASE_ONEOF(c, ConsumerImpl::Close) {
          // We reached the end of the buffer! All data has been consumed.
          // We want to resolve the read request with everything we have
          // so far and transition the consumer into the closed state.
          return request.resolveAsDone(js);
        }
        KJ_CASE_ONEOF(entry, QueueEntry) {
          // The amount to copy is the lesser of the current entry size minus
          // offset and the data remaining in the destination to fill.
          auto entrySize = entry.entry->getSize();
          auto amountToCopy = kj::min(entrySize - entry.offset,
                                      request.pullInto.store.size() - request.pullInto.filled);

          // Once we have the amount, we safely copy amountToCopy bytes from the
          // entry into the destination request, accounting properly for the offsets.
          auto sourcePtr = entry.entry->toArrayPtr().begin() + entry.offset;
          auto destPtr = request.pullInto.store.asArrayPtr().begin() + request.pullInto.filled;

          std::copy(sourcePtr, sourcePtr + amountToCopy, destPtr);

          request.pullInto.filled += amountToCopy;
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

          // Otherwise, it is ok that there is data remaining but the amountToConsume
          // should be 0. Specifically, we either consume the entire entry and there
          // is data left over to consume, or we did not consume the entire entry
          // but read all that we can.
          KJ_REQUIRE(amountToConsume == 0);
        }
      }
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

kj::Maybe<kj::Own<ByteQueue::ByobReadRequest>> ByteQueue::nextPendingByobReadRequest() {
  KJ_IF_MAYBE(state, impl.getState()) {
    while (!state->pendingByobReadRequests.empty()) {
      auto request = kj::mv(state->pendingByobReadRequests.front());
      state->pendingByobReadRequests.pop_front();
      if (!request->isInvalidated()) {
        return kj::mv(request);
      }
    }
  }
  return nullptr;
}

#pragma endregion ByteQueue

} // namespace workerd::api
