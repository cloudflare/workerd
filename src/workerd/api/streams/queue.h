// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/buffersource.h>
#include <algorithm>
#include <deque>
#include <kj/table.h>
#include <set>

namespace workerd::api {

// ============================================================================
// Queues
//
// There are two kinds of queues used internally by the JavaScript-backed
// ReadableStream implementation: value queues and byte queues. Each operate
// in generally the same way but byte queues have a number of unique complexities
// that make them more difficult.
//
// A queue (of either type) has the following general characteristics:
//
//  - Every queue has a high water mark. This is the maximum amount of data
//    that should be stored in the queue pending consumption before backpressure
//    is signaled. Additional data can always be pushed into the queue beyond
//    the high water mark, but it is not advisable to do so.
//
//  - All data stored in the queue is in the form of refcounted entries. The
//    specific type of entry depends on the queue type. Every entry has a
//    calculated size, which is dependent on the type of entry. The reason
//    these entries are refcounted is because...(see the next bullet point)
//
//  - Every queue has one or more consumers. Each consumer maintains its own
//    internal buffer of refcounted entries that it has yet to consume. Because
//    entries are refcounted, there is ever only one copy of any given chunk of
//    data in memory, with each consumer possessing only a reference to it.
//    Whenever data is pushed into the queue, references are pushed into each of
//    the consumers. As data is consumed from the internal buffer, the reference
//    counted entries are freed. The underlying data is freed once the last
//    reference is released.
//
//  - Every consumer has an remaining buffer size, which is the sum of the sizes
//    of all entries remaining to be consumed in its internal buffer.
//
//  - A queue has a total queue size, which is the remaining buffer size of the
//    consumer with the most unconsumed data.
//
//  - A queue has a desired size, which is the amount of additional data that
//    can be pushed into the queue before backpressure is signaled. It is
//    calculated by subtracting the total queue size from the high water mark.
//
//  - Backpressure is signaled when desired size is equal to, or less than zero.
//
// Each type of queue has a specific kind of consumer. These generally operate
// in the same way but Byte Queue consumers have a number of unique details.
//
//  - As mentioned above, every consumer maintains an internal data buffer
//    consisting of refcounted pointers to the data that has been pushed into
//    the queue.
//
//  - Every consumer maintains a list of pending reads. A read is a request to
//    consume some amount of data from the internal data buffer. If there is
//    enough data in the internal buffer to immediately fulfill the read request
//    when it is received, then we do so. Otherwise, the read is moved into the
//    pending reads list and is fullfilled later once there is enough data provided
//    to the consumer to do so.
//
//  - When data is provided to a consumer by the queue, that data is added to
//    the internal buffer only if there are no pending reads capable of
//    immediately consuming the data.
//
//  - When data is added to the internal buffer, the remaining buffer size is
//    incremented. When data is removed from the internal buffer, the remaining
//    buffer size is decremented.
//
//  - Whenever the remaining buffer size for a consumer is modified, the queue
//    is asked to recalculate the desired size.
//
// For value queues, every individual entry is queued and consumed as a whole
// unit. It is not possible to partially consume a single value entry. The
// size of a value entry is calculated by a JavaScript function provided by
// user code (the "size algorithm") if one is provided. If a size algorithm
// is not provided, the default size of a value entry is exactly 1.
//
// The bookkeeping for a value queue is fairly simple:
//
//  - A single refcounted value entry is created.
//  - References to that single value entry are distributed to each of
//    the value queue consumers.
//  - If a consumer has a pending read, the read is fulfilled immediately
//    and the reference is never added to that consumer's internal buffer.
//  - If the consumer has no pending reads, the reference is added to the
//    consumer's internal buffer and the remaining buffer size is incremented
//    by the calculated size of the value entry.
//  - Once the value entry has been delivered to each of the consumers,
//    the total queue size is updated by setting it equal to the maximum
//    remaining buffer size among the consumers.
//  - Later, when a consumer receives a read that consumes data from the
//    internal buffer, the remaining buffer size is decremented by the calculated
//    size of the value entry, and the queue is notified to re-evaluated the
//    total queue size.
//
// For byte queues, the situation becomes much more complicated for two
// specific reasons: 1) All entries are in the form of arbitrarily long
// byte sequences that can be partially consumed, and 2) read requests
// made to the byte queue can be "BYOB" (bring your own buffer) in which
// the intent is to avoid being forced to copy data between buffers by
// having the reading code allocate and provide a buffer that the stream
// implementation will read data into. When there is only a single consumer
// for a streams data, the BYOB model is fairly straightforward and can be
// implemented to avoid copying entirely. However, when you have multiple
// consumers for a byte queue, all consuming data at different rates, it is
// not possible to avoid copying entirely. Reads that consume byte data can
// specify a range that crosses the boundaries of the individual entries that
// are stored within the internal buffer, further complicating the process of
// consuming data.
//
// To make matters even more complicated, a stream implementation is permitted
// to ignore the allocated buffers provided by the BYOB read request and push
// data into the queue as if the allocated buffer were not provided at all.
// In such cases, the BYOB read request still needs to be fulfilled with the
// provided buffer being written into it. Unfortunately, this is not uncommon.
// React server-side rendering, for instance, will create byte-oriented
// ReadableStreams that support BYOB reads, but will use the controller.enqueue()
// API to push data into the stream rather than paying any attention to the
// BYOB buffers provided by the readers.
//
// The requirement to support BYOB reads makes it critical to properly sequence
// the delivery of BYOB read requests to the stream controller implementation,
// ensuring the proper order of bytes delivered to each consumer while respecting
// backpressure signaling such that backpressure is always determined by the
// consumer that is being consumed at the slowest rate.
//
// On top of everything else, Workers introduces the concept of a minRead,
// that is, a minimum number of bytes that a read request should consume from
// the queue. The read promise should not be fulfilled unless either that
// minimum number of bytes has been provided, or the stream is closed or errored.

template <typename Self>
class ConsumerImpl;

template <typename Self>
class QueueImpl;

template <typename Self>
class QueueImpl final {
  // Provides the underlying implementation shared by ByteQueue and ValueQueue.
public:
  using ConsumerImpl = ConsumerImpl<Self>;
  using Entry = typename Self::Entry;
  using State = typename Self::State;

  explicit QueueImpl(size_t highWaterMark) : highWaterMark(highWaterMark) {}

  QueueImpl(QueueImpl&&) = default;
  QueueImpl& operator=(QueueImpl&&) = default;

  void close(jsg::Lock& js) {
    // Closes the queue. The close is forwarded on to all consumers.
    // If we are already closed or errored, do nothing here.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      // We copy the list of consumers in case the consumers remove themselves
      // from the queue during the close callback, invalidating the iterator.
      auto consumers = ready->consumers;
      for (auto consumer : consumers) {
        consumer->close(js);
      }
      state.template init<Closed>();
    }
  }

  inline ssize_t desiredSize() const {
    // The amount of data the Queue needs until it is considered full.
    // The value can be zero or negative, in which case backpressure is
    // signaled on the queue.
    // If the queue is already closed or errored, return 0.
    return state.template is<Ready>() ? highWaterMark - size() : 0;
  }

  void error(jsg::Lock& js, jsg::Value reason) {
    // Errors the queue. The error is forwarded on to all consumers,
    // which will, in turn, reset their internal buffers and reject
    // all pending consume promises.
    // If we are already closed or errored, do nothing here.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      // We copy the list of consumers in case the consumers remove themselves
      // from the queue during the error callback, invalidating the iterator.
      auto consumers = ready->consumers;
      for (auto consumer : consumers) {
        consumer->error(js, reason.addRef(js));
      }
      state = kj::mv(reason);
    }
  }

  void maybeUpdateBackpressure() {
    // Polls all known consumers to collect their current buffer sizes
    // so that the current queue size can be updated.
    // If we are already closed or errored, set totalQueueSize to zero.
    totalQueueSize = 0;
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      for (auto consumer : ready->consumers) {
        totalQueueSize = kj::max(totalQueueSize, consumer->size());
      }
    }
  }

  void push(jsg::Lock& js,
            kj::Own<Entry> entry,
            kj::Maybe<ConsumerImpl&> skipConsumer = nullptr) {
    // Forwards the entry to all consumers (except skipConsumer if given).
    // For each consumer, the entry will be used to fulfill any pending consume operations.
    // If the entry type is byteOriented and has not been fully consumed by pending consume
    // operations, then any left over data will be pushed into the consumer's buffer.
    // Asserts if the queue is closed or errored.
    auto& ready = KJ_REQUIRE_NONNULL(state.template tryGet<Ready>(),
        "The queue is closed or errored.");

    for (auto consumer : ready.consumers) {
      KJ_IF_MAYBE(skip, skipConsumer) {
        if (&(*skip) == consumer) {
          continue;
        }
      }

      consumer->push(js, entry->clone(js));
    }
  }

  size_t size() const { return totalQueueSize; }
  // The current size of consumer with the most stored data.

  size_t getConsumerCount() const {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, Closed) { return 0; }
      KJ_CASE_ONEOF(errored, Errored) { return 0; }
      KJ_CASE_ONEOF(ready, Ready) { return ready.consumers.size(); }
    }
    KJ_UNREACHABLE;
  }

  bool wantsRead() const {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, Closed) { return false; }
      KJ_CASE_ONEOF(errored, Errored) { return false; }
      KJ_CASE_ONEOF(ready, Ready) {
        for (auto consumer : ready.consumers) {
          if (consumer->hasReadRequests()) return true;
        }
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<State&> getState() KJ_LIFETIMEBOUND {
    // Specific queue implementations may provide additional state that is attached
    // to the Ready struct.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      return *ready;
    }
    return nullptr;
  }

private:
  struct Closed {};
  using Errored = jsg::Value;

  struct Ready final: public State {
    std::set<ConsumerImpl*> consumers;
  };

  size_t highWaterMark;
  size_t totalQueueSize = 0;
  kj::OneOf<Ready, Closed, Errored> state = Ready();

  void addConsumer(ConsumerImpl* consumer) {
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      ready->consumers.insert(consumer);
    }
  }

  void removeConsumer(ConsumerImpl* consumer) {
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      ready->consumers.erase(consumer);
      maybeUpdateBackpressure();
    }
  }

  friend Self;
  friend ConsumerImpl;
};

template <typename Self>
class ConsumerImpl final {
  // Provides the underlying implementation shared by ByteQueue::Consumer and ValueQueue::Consumer
public:
  struct StateListener {
    virtual void onConsumerClose(jsg::Lock& js) = 0;
    virtual void onConsumerError(jsg::Lock& js, jsg::Value reason) = 0;
    virtual void onConsumerWantsData(jsg::Lock& js) = 0;
  };

  using QueueImpl = QueueImpl<Self>;

  struct UpdateBackpressureScope final {
    // A simple utility to be allocated on any stack where consumer buffer data maybe consumed
    // or expanded. When the stack is unwound, it ensures the backpressure is appropriately
    // updated.
    QueueImpl& queue;
    UpdateBackpressureScope(QueueImpl& queue) : queue(queue) {};
    ~UpdateBackpressureScope() noexcept(false) {
      queue.maybeUpdateBackpressure();
    }
    KJ_DISALLOW_COPY_AND_MOVE(UpdateBackpressureScope);
  };

  using ReadRequest = typename Self::ReadRequest;
  using Entry = typename Self::Entry;
  using QueueEntry = typename Self::QueueEntry;

  ConsumerImpl(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr)
    : queue(queue), stateListener(stateListener) {
    queue.addConsumer(this);
  }

  ConsumerImpl(ConsumerImpl& other) = delete;
  ConsumerImpl(ConsumerImpl&&) = delete;
  ConsumerImpl& operator=(ConsumerImpl&) = delete;
  ConsumerImpl& operator=(ConsumerImpl&&) = delete;

  ~ConsumerImpl() noexcept(false) {
    queue.removeConsumer(this);
  }

  void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, Closed) {}
      KJ_CASE_ONEOF(errored, Errored) {}
      KJ_CASE_ONEOF(ready, Ready) {
        for (auto& request : ready.readRequests) {
          request.resolveAsDone(js);
        }
        state.template init<Closed>();
      }
    }
  }

  void close(jsg::Lock& js) {
    // If we are already closed or errored, then we do nothing here.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      // If we are not already closing, enqueue a Close sentinel.
      if (!isClosing()) {
        ready->buffer.push_back(Close {});
      }

      // Then check to see if we need to drain pending reads and
      // update the state to Closed.
      return maybeDrainAndSetState(js);
    }
  }

  inline bool empty() const { return size() == 0; }

  void error(jsg::Lock& js, jsg::Value reason) {
    // If we are already closed or errored, then we do nothing here.
    // The new error doesn't matter.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      maybeDrainAndSetState(js, kj::mv(reason));
    }
  }

  void push(jsg::Lock& js, kj::Own<Entry> entry) {
    auto& ready = KJ_REQUIRE_NONNULL(state.template tryGet<Ready>(),
        "The consumer is either closed or errored.");
    KJ_REQUIRE(!isClosing(), "The consumer is already closing.");

    // If the size of the entry is zero, do nothing.
    if (entry->getSize() == 0) {
      return;
    }

    UpdateBackpressureScope scope(queue);
    Self::handlePush(js, ready, queue, kj::mv(entry));
  }

  void read(jsg::Lock& js, ReadRequest request) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(c, Closed) {
        return request.resolveAsDone(js);
      }
      KJ_CASE_ONEOF(e, Errored) {
        return request.reject(js, e);
      }
      KJ_CASE_ONEOF(ready, Ready) {
        Self::handleRead(js, ready, *this, queue, kj::mv(request));
        return maybeDrainAndSetState(js);
      }
    }
    KJ_UNREACHABLE;
  }

  void reset() {
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      UpdateBackpressureScope scope(queue);
      ready->buffer.clear();
      ready->queueTotalSize = 0;
    }
  }

  size_t size() const {
    // The current total calculated size of the consumer's internal buffer.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(e, Errored) { return 0; }
      KJ_CASE_ONEOF(c, Closed) { return 0; }
      KJ_CASE_ONEOF(r, Ready) { return r.queueTotalSize; }
    }
    KJ_UNREACHABLE;
  }

  void resolveRead(jsg::Lock& js, ReadRequest& req) {
    auto& ready = KJ_REQUIRE_NONNULL(state.template tryGet<Ready>());
    KJ_REQUIRE(!ready.readRequests.empty());
    KJ_REQUIRE(&req == &ready.readRequests.front());
    req.resolve(js);
    ready.readRequests.pop_front();
  }

  void resolveReadAsDone(jsg::Lock& js, ReadRequest& req) {
    auto& ready = KJ_REQUIRE_NONNULL(state.template tryGet<Ready>());
    KJ_REQUIRE(!ready.readRequests.empty());
    KJ_REQUIRE(&req == &ready.readRequests.front());
    req.resolveAsDone(js);
    ready.readRequests.pop_front();
  }

  void cloneTo(jsg::Lock& js, ConsumerImpl& other) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(c, Closed) {
        other.state.template init<Closed>();
      }
      KJ_CASE_ONEOF(e, Errored) {
        other.state = e.addRef(js);
      }
      KJ_CASE_ONEOF(ready, Ready) {
        // We copy the buffered state but not the readRequests.
        auto& otherReady = KJ_REQUIRE_NONNULL(other.state.template tryGet<Ready>(),
            "The new consumer should not be closed or errored.");
        otherReady.queueTotalSize = ready.queueTotalSize;
        for (auto& item: ready.buffer) {
          KJ_SWITCH_ONEOF(item) {
            KJ_CASE_ONEOF(c, Close) {
              otherReady.buffer.push_back(Close {});
            }
            KJ_CASE_ONEOF(entry, QueueEntry) {
              otherReady.buffer.push_back(entry.clone(js));
            }
          }
        }
      }
    }
  }

  bool hasReadRequests() const {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, Closed) { return false; }
      KJ_CASE_ONEOF(errored, Errored) { return false; }
      KJ_CASE_ONEOF(ready, Ready) {
        return !ready.readRequests.empty();
      }
    }
    KJ_UNREACHABLE;
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(closed, Closed) {}
      KJ_CASE_ONEOF(errored, Errored) {
        visitor.visit(errored);
      }
      KJ_CASE_ONEOF(ready, Ready) {
        for (auto& entry : ready.buffer) {
          KJ_IF_MAYBE(e, entry.template tryGet<QueueEntry>()) {
            visitor.visit(*e);
          }
        }
        for (auto& req : ready.readRequests) {
          visitor.visit(req.resolver);
        }
      }
    }
  }

private:
  struct Close {};
  // A sentinel used in the buffer to signal that close() has been called.

  struct Closed {};
  using Errored = jsg::Value;
  struct Ready {
    std::deque<kj::OneOf<QueueEntry, Close>> buffer;
    std::deque<ReadRequest> readRequests;
    size_t queueTotalSize = 0;
  };

  QueueImpl& queue;
  kj::OneOf<Ready, Closed, Errored> state = Ready();
  kj::Maybe<ConsumerImpl::StateListener&> stateListener;

  bool isClosing() {
    // Closing state is determined by whether there is a Close sentinel that has been
    // pushed into the end of Ready state buffer.
    KJ_SWITCH_ONEOF(state) {
      KJ_CASE_ONEOF(c, Closed) { return false; }
      KJ_CASE_ONEOF(e, Errored) { return false; }
      KJ_CASE_ONEOF(r, Ready) {
        if (r.buffer.empty()) {
          return false;
        }
        return r.buffer.back().template is<Close>();
      }
    }
    KJ_UNREACHABLE;
  }

  void maybeDrainAndSetState(jsg::Lock& js, kj::Maybe<jsg::Value> maybeReason = nullptr) {
    // If the state is already errored or closed then there is nothing to drain.
    KJ_IF_MAYBE(ready, state.template tryGet<Ready>()) {
      UpdateBackpressureScope scope(queue);
      KJ_IF_MAYBE(reason, maybeReason) {
        // If maybeReason != nullptr, then we are draining because of an error.
        // In that case, we want to reset/clear the buffer and reject any remaining
        // pending read requests using the given reason.
        for (auto& request : ready->readRequests) {
          request.reject(js, *reason);
        }
        state = reason->addRef(js);
        KJ_IF_MAYBE(listener, stateListener) {
          listener->onConsumerError(js, kj::mv(*reason));
          // After this point, we should not assume that this consumer can
          // be safely used at all. It's most likely the stateListener has
          // released it.
        }
      } else {
        // Otherwise, if isClosing() is true...
        if (isClosing()) {
          if (!empty() && !Self::handleMaybeClose(js, *ready, *this, queue)) {
            // If the queue is not empty, we'll have the implementation see
            // if it can drain the remaining data into pending reads. If handleMaybeClose
            // returns false, then it could not and we can't yet close. If it returns true,
            // yay! Our queue is empty and we can continue closing down.
            KJ_ASSERT(!empty()); // We're still not empty
            return;
          }

          KJ_ASSERT(empty());
          KJ_REQUIRE(ready->buffer.size() == 1); // The close should be the only item remaining.
          for (auto& request : ready->readRequests) {
            request.resolveAsDone(js);
          }
          state.template init<Closed>();
          KJ_IF_MAYBE(listener, stateListener) {
            listener->onConsumerClose(js);
            // After this point, we should not assume that this consumer can
            // be safely used at all. It's most likely the stateListener has
            // released it.
          }
        }
      }
    }
  }

  friend typename Self::Consumer;
  friend Self;
};

// ============================================================================
// Value queue

class ValueQueue final {
public:
  using ConsumerImpl = ConsumerImpl<ValueQueue>;
  using QueueImpl = QueueImpl<ValueQueue>;

  struct State {};

  struct ReadRequest {
    jsg::Promise<ReadResult>::Resolver resolver;

    void resolveAsDone(jsg::Lock& js);
    void resolve(jsg::Lock& js, jsg::Value value);
    void reject(jsg::Lock& js, jsg::Value& value);
  };

  class Entry {
    // A value queue entry consists of an arbitrary JavaScript value and a size that is
    // calculated by the size algorithm function provided in the stream constructor.
  public:
    explicit Entry(jsg::Value value, size_t size);

    jsg::Value getValue(jsg::Lock& js);

    size_t getSize() const;

    void visitForGc(jsg::GcVisitor& visitor);

    kj::Own<Entry> clone(jsg::Lock& js);

  private:
    jsg::Value value;
    size_t size;
  };

  struct QueueEntry {
    kj::Own<Entry> entry;
    QueueEntry clone(jsg::Lock& js);

    void visitForGc(jsg::GcVisitor& visitor) {
      if (entry) visitor.visit(*entry);
    }
  };

  class Consumer final {
  public:
    Consumer(ValueQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);
    Consumer(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);
    Consumer(Consumer&&) = delete;
    Consumer(Consumer&) = delete;
    Consumer& operator=(Consumer&&) = delete;
    Consumer& operator=(Consumer&) = delete;

    void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason);

    void close(jsg::Lock& js);

    bool empty();

    void error(jsg::Lock& js, jsg::Value reason);

    void read(jsg::Lock& js, ReadRequest request);

    void push(jsg::Lock& js, kj::Own<Entry> entry);

    void reset();

    size_t size();

    kj::Own<Consumer> clone(jsg::Lock& js,
                            kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);

    bool hasReadRequests();

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(impl);
    }

  private:
    ConsumerImpl impl;

    friend class ValueQueue;
  };

  explicit ValueQueue(size_t highWaterMark);

  void close(jsg::Lock& js);

  ssize_t desiredSize() const;

  void error(jsg::Lock& js, jsg::Value reason);

  void maybeUpdateBackpressure();

  void push(jsg::Lock& js, kj::Own<Entry> entry);

  size_t size() const;

  size_t getConsumerCount();

  bool wantsRead() const {
    return impl.wantsRead();
  }

  bool hasPartiallyFulfilledRead() {
    // A ValueQueue can never have a partially fulfilled read.
    return false;
  }

  void visitForGc(jsg::GcVisitor& visitor) {}

private:
  QueueImpl impl;

  static void handlePush(jsg::Lock& js,
                         ConsumerImpl::Ready& state,
                         QueueImpl& queue,
                         kj::Own<Entry> entry);
  static void handleRead(jsg::Lock& js,
                         ConsumerImpl::Ready& state,
                         ConsumerImpl& consumer,
                         QueueImpl& queue,
                         ReadRequest request);
  static bool handleMaybeClose(jsg::Lock& js,
                               ConsumerImpl::Ready& state,
                               ConsumerImpl& consumer,
                               QueueImpl& queue);

  friend ConsumerImpl;
};

// ============================================================================
// Byte queue


class ByteQueue final {
public:
  using ConsumerImpl = ConsumerImpl<ByteQueue>;
  using QueueImpl = QueueImpl<ByteQueue>;

  class ByobRequest;

  struct ReadRequest final {
    enum class Type { DEFAULT, BYOB };
    jsg::Promise<ReadResult>::Resolver resolver;
    kj::Maybe<ByobRequest&> byobReadRequest;
    // The reference here should be cleared when the ByobRequest is invalidated,
    // which happens either when respond(), respondWithNewView(), or invalidate()
    // is called, or when the ByobRequest is destroyed, whichever comes first.

    struct {
      jsg::BackingStore store;
      size_t filled = 0;
      size_t atLeast = 1;
      Type type = Type::DEFAULT;
    } pullInto;

    void resolveAsDone(jsg::Lock& js);
    void resolve(jsg::Lock& js);
    void reject(jsg::Lock& js, jsg::Value& value);

    kj::Own<ByobRequest> makeByobReadRequest(ConsumerImpl& consumer, QueueImpl& queue);
  };

  class ByobRequest final {
    // The ByobRequest is essentially a handle to the ByteQueue::ReadRequest that can be given to a
    // ReadableStreamBYOBRequest object to fulfill the request using the BYOB API pattern.
    //
    // When isInvalidated() is false, respond() or respondWithNewView() can be called to fulfill
    // the BYOB read request. Once either of those are called, or once invalidate() is called,
    // the ByobRequest is no longer usable and should be discarded.
  public:
    ByobRequest(
        ReadRequest& request,
        ConsumerImpl& consumer,
        QueueImpl& queue)
        : request(request),
          consumer(consumer),
          queue(queue) {}

    KJ_DISALLOW_COPY_AND_MOVE(ByobRequest);

    ~ByobRequest() noexcept(false);

    ReadRequest& getRequest() { return KJ_ASSERT_NONNULL(request); }

    bool respond(jsg::Lock& js, size_t amount);

    bool respondWithNewView(jsg::Lock& js, jsg::BufferSource view);

    void invalidate();
    // Disconnects this ByobRequest instance from the associated ByteQueue::ReadRequest.
    // The term "invalidate" is adopted from the streams spec for handling BYOB requests.

    inline bool isInvalidated() const { return request == nullptr; }

    bool isPartiallyFulfilled();

    size_t getAtLeast() const;

    v8::Local<v8::Uint8Array> getView(jsg::Lock& js);

  private:
    kj::Maybe<ReadRequest&> request;
    ConsumerImpl& consumer;
    QueueImpl& queue;
  };

  struct State {
    std::deque<kj::Own<ByobRequest>> pendingByobReadRequests;
  };

  class Entry {
    // A byte queue entry consists of a jsg::BackingStore containing a non-zero-length
    // sequence of bytes. The size is determined by the number of bytes in the entry.
  public:
    explicit Entry(jsg::BackingStore store);

    kj::ArrayPtr<kj::byte> toArrayPtr();

    size_t getSize() const;

    inline void visitForGc(jsg::GcVisitor& visitor) {}

    kj::Own<Entry> clone(jsg::Lock& js);

  private:
    jsg::BackingStore store;
  };

  struct QueueEntry {
    kj::Own<Entry> entry;
    size_t offset;

    QueueEntry clone(jsg::Lock& js);

    void visitForGc(jsg::GcVisitor& visitor) {}
  };

  class Consumer {
  public:
    Consumer(ByteQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);
    Consumer(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);
    Consumer(Consumer&&) = delete;
    Consumer(Consumer&) = delete;
    Consumer& operator=(Consumer&&) = delete;
    Consumer& operator=(Consumer&) = delete;

    void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason);

    void close(jsg::Lock& js);

    bool empty() const;

    void error(jsg::Lock& js, jsg::Value reason);

    void read(jsg::Lock& js, ReadRequest request);

    void push(jsg::Lock& js, kj::Own<Entry> entry);

    void reset();

    size_t size() const;

    kj::Own<Consumer> clone(jsg::Lock& js,
                            kj::Maybe<ConsumerImpl::StateListener&> stateListener = nullptr);

    bool hasReadRequests();

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(impl);
    }

  private:
    ConsumerImpl impl;
  };

  explicit ByteQueue(size_t highWaterMark);

  void close(jsg::Lock& js);

  ssize_t desiredSize() const;

  void error(jsg::Lock& js, jsg::Value reason);

  void maybeUpdateBackpressure();

  void push(jsg::Lock& js, kj::Own<Entry> entry);

  size_t size() const;

  size_t getConsumerCount();

  bool wantsRead() const {
    return impl.wantsRead();
  }

  bool hasPartiallyFulfilledRead();

  kj::Maybe<kj::Own<ByobRequest>> nextPendingByobReadRequest();
  // nextPendingByobReadRequest will be used to support the ReadableStreamBYOBRequest interface
  // that is part of ReadableByteStreamController. When user code calls the `controller.byobRequest`
  // API on a ReadableByteStreamController, they are going to get an instance of a
  // ReadableStreamBYOBRequest object. That object will own the `kj::Own<ByobReadRequest>` that
  // is returned here. User code could end up doing something silly like holding a reference to
  // that byobRequest long after it has been invalidated. We heap-allocate these just to allow
  // their lifespan to be attached to the ReadableStreamBYOBRequest object but internally they
  // will be disconnected as appropriate.

  void visitForGc(jsg::GcVisitor& visitor) {}

private:
  QueueImpl impl;

  static void handlePush(jsg::Lock& js,
                         ConsumerImpl::Ready& state,
                         QueueImpl& queue,
                         kj::Own<Entry> entry);
  static void handleRead(jsg::Lock& js,
                         ConsumerImpl::Ready& state,
                         ConsumerImpl& consumer,
                         QueueImpl& queue,
                         ReadRequest request);
  static bool handleMaybeClose(jsg::Lock& js,
                               ConsumerImpl::Ready& state,
                               ConsumerImpl& consumer,
                               QueueImpl& queue);

  friend ConsumerImpl;
  friend class Consumer;
};

} // workerd::api
