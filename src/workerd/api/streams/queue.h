// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "common.h"

#include <workerd/jsg/jsg.h>
#include <workerd/util/ring-buffer.h>
#include <workerd/util/small-set.h>
#include <workerd/util/state-machine.h>
#include <workerd/util/weak-refs.h>

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
//  - All data stored in the queue is in the form of entries. The
//    specific type of entry depends on the queue type. Every entry has a
//    calculated size, which is dependent on the type of entry.
//
//  - Every queue has one or more consumers. Each consumer maintains its own
//    internal buffer of entries that it has yet to consume. Entries are
//    structured such that there is ever only one copy of any given chunk of
//    data in memory, with each entry in each consumer possessing only a reference
//    to it. Whenever data is pushed into the queue, references are pushed into
//    each of the consumers. As data is consumed from the internal buffer, the
//    entries are freed. The underlying data is freed once the last
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
//    consisting of references to the data that has been pushed into
//    the queue.
//
//  - Every consumer maintains a list of pending reads. A read is a request to
//    consume some amount of data from the internal data buffer. If there is
//    enough data in the internal buffer to immediately fulfill the read request
//    when it is received, then we do so. Otherwise, the read is moved into the
//    pending reads list and is fulfilled later once there is enough data provided
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
//  - A single value entry is created.
//  - Clones of that single value entry are distributed to each of
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

// DrainingReadResult is defined in common.h

// Provides the underlying implementation shared by ByteQueue and ValueQueue.
template <typename Self>
class QueueImpl final {
 public:
  using ConsumerImpl = ConsumerImpl<Self>;
  using Entry = Self::Entry;
  using State = Self::State;

  explicit QueueImpl(size_t highWaterMark)
      : highWaterMark(highWaterMark),
        state(QueueState::template create<Ready>()) {}

  QueueImpl(QueueImpl&&) = default;
  QueueImpl& operator=(QueueImpl&&) = default;

  ~QueueImpl() noexcept(false) {
    // Detach all consumers before destruction to prevent UAF.
    // This can happen during isolate teardown when the destruction order
    // of JS wrapper objects doesn't follow the ownership hierarchy.
    allConsumers.forEach([&](ConsumerImpl& consumer) { consumer.detachQueue(); });
  }

  // Closes the queue. The close is forwarded on to all consumers.
  // If we are already closed or errored, do nothing here.
  void close(jsg::Lock& js) {
    if (state.isActive()) {
#ifdef KJ_DEBUG
      isClosingOrErroring = true;
      KJ_DEFER(isClosingOrErroring = false);
#endif
      allConsumers.forEach([&](ConsumerImpl& consumer) { consumer.close(js); });
      state.template transitionTo<Closed>();
    }
  }

  // The amount of data the Queue needs until it is considered full.
  // The value can be zero or negative, in which case backpressure is
  // signaled on the queue.
  // If the queue is already closed or errored, return 0.
  inline ssize_t desiredSize() const {
    return state.isActive() ? highWaterMark - size() : 0;
  }

  // Errors the queue. The error is forwarded on to all consumers,
  // which will, in turn, reset their internal buffers and reject
  // all pending consume promises.
  // If we are already closed or errored, do nothing here.
  void error(jsg::Lock& js, jsg::Value reason) {
    if (state.isActive()) {
#ifdef KJ_DEBUG
      isClosingOrErroring = true;
      KJ_DEFER(isClosingOrErroring = false);
#endif
      allConsumers.forEach([&](ConsumerImpl& consumer) { consumer.error(js, reason.addRef(js)); });
      state.template transitionTo<Errored>(kj::mv(reason));
    }
  }

  // Polls all known consumers to collect their current buffer sizes
  // so that the current queue size can be updated.
  // If we are already closed or errored, set totalQueueSize to zero.
  void maybeUpdateBackpressure() {
    totalQueueSize = 0;
    if (state.isActive()) {
      allConsumers.forEach([&](ConsumerImpl& consumer) {
        totalQueueSize = kj::max(totalQueueSize, consumer.size());
      });
    }
  }

  // Forwards the entry to all consumers (except skipConsumer if given).
  // For each consumer, the entry will be used to fulfill any pending consume operations.
  // If the entry type is byteOriented and has not been fully consumed by pending consume
  // operations, then any left over data will be pushed into the consumer's buffer.
  // Asserts if the queue is closed or errored.
  void push(jsg::Lock& js, kj::Rc<Entry> entry, kj::Maybe<ConsumerImpl&> skipConsumer = kj::none) {
    state.requireActiveUnsafe("The queue is closed or errored.");

    allConsumers.forEach([&](ConsumerImpl& consumer) {
      KJ_IF_SOME(skip, skipConsumer) {
        if (&skip == &consumer) {
          return;
        }
      }
      consumer.push(js, entry->clone(js));
    });
  }

  // The current size of consumer with the most stored data.
  size_t size() const {
    return totalQueueSize;
  }

  size_t getConsumerCount() const {
    return allConsumers.size();
  }

  bool wantsRead() const {
    if (state.isActive()) {
      for (const auto& weakRef: allConsumers) {
        KJ_IF_SOME(consumer, weakRef->tryGet()) {
          if (consumer.hasReadRequests()) return true;
        }
      }
    }
    return false;
  }

  // Specific queue implementations may provide additional state that is attached
  // to the Ready struct.
  kj::Maybe<State&> getState() KJ_LIFETIMEBOUND {
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      return ready;
    }
    return kj::none;
  }

  inline kj::StringPtr jsgGetMemoryName() const;
  inline size_t jsgGetMemorySelfSize() const;
  inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  struct Closed {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "closed"_kj;
  };
  struct Errored {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "errored"_kj;
    jsg::Value reason;
  };

  struct Ready final: public State {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "ready"_kj;
  };

  // State machine for QueueImpl:
  //   Ready -> Closed (close() called)
  //   Ready -> Errored (error() called)
  // Closed is terminal, Errored is implicitly terminal via ErrorState.
  using QueueState = StateMachine<TerminalStates<Closed>,
      ErrorState<Errored>,
      ActiveState<Ready>,
      Ready,
      Closed,
      Errored>;

  size_t highWaterMark;
  size_t totalQueueSize = 0;
  QueueState state;
  // The set of consumers attached to this queue. In the typical case this
  // will be a very small number (often just one or two), so we use SmallSet to
  // optimize for that. This persists across state transitions so we can detach
  // consumers even after close()/error() transitions the queue to a terminal state.
  //
  // We store weak references to consumers to safely handle the case where a consumer
  // is destroyed during iteration (e.g., resolving a read request triggers JS that
  // destroys another consumer in the same queue). When iterating, we check if the WeakRef is still valid.
  SmallSet<kj::Rc<WeakRef<ConsumerImpl>>> allConsumers;

#ifdef KJ_DEBUG
  // Debug flag to detect if addConsumer is called during close/error iteration.
  // This should never happen - it would indicate a bug in the streams implementation.
  bool isClosingOrErroring = false;
#endif

  void addConsumer(kj::Rc<WeakRef<ConsumerImpl>> weakRef) {
    KJ_DASSERT(
        !isClosingOrErroring, "Cannot add a consumer while the queue is being closed or errored");
    allConsumers.add(kj::mv(weakRef));
  }

  void removeConsumer(ConsumerImpl& consumer) {
    allConsumers.removeIf([&consumer](const kj::Rc<WeakRef<ConsumerImpl>>& ref) {
      KJ_IF_SOME(c, ref->tryGet()) {
        return &c == &consumer;
      }
      return false;  // Already invalid, will be cleaned up later
    });
    maybeUpdateBackpressure();
  }

  friend Self;
  friend ConsumerImpl;
};

// Provides the underlying implementation shared by ByteQueue::Consumer and ValueQueue::Consumer
template <typename Self>
class ConsumerImpl final {
 public:
  struct StateListener {
    virtual void onConsumerClose(jsg::Lock& js) = 0;
    virtual void onConsumerError(jsg::Lock& js, jsg::Value reason) = 0;
    // Called when the consumer has a pending read and needs data.
    // Returns true if the pull algorithm completed synchronously (meaning
    // more pumping might yield additional synchronous data), false if the
    // pull is async (promise pending) or no pull was needed.
    virtual bool onConsumerWantsData(jsg::Lock& js) = 0;
  };

  using QueueImpl = QueueImpl<Self>;

  // A simple utility to be allocated on any stack where consumer buffer data maybe consumed
  // or expanded. When the stack is unwound, it ensures the backpressure is appropriately
  // updated. Captures the queue at construction time since the consumer may be
  // destroyed before the scope ends (e.g., via onConsumerError -> owner.doError()).
  struct UpdateBackpressureScope final {
    kj::Maybe<QueueImpl&> queue;
    UpdateBackpressureScope(ConsumerImpl& consumer): queue(consumer.queue) {}
    ~UpdateBackpressureScope() noexcept(false) {
      KJ_IF_SOME(q, queue) {
        q.maybeUpdateBackpressure();
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(UpdateBackpressureScope);
  };

  using ReadRequest = Self::ReadRequest;
  using Entry = Self::Entry;
  using QueueEntry = Self::QueueEntry;

  ConsumerImpl(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none)
      : queue(queue),
        state(ConsumerState::template create<Ready>()),
        stateListener(stateListener) {
    queue.addConsumer(selfRef.addRef());
  }

  explicit ConsumerImpl(kj::Maybe<ConsumerImpl::StateListener&> stateListener)
      : queue(kj::none),
        state(ConsumerState::template create<Ready>()),
        stateListener(stateListener) {}

  KJ_DISALLOW_COPY_AND_MOVE(ConsumerImpl);

  ~ConsumerImpl() noexcept(false) {
    // queue may be none if the queue was destroyed before this consumer
    // (e.g., during isolate teardown) or if cloned from a closed stream.
    // We must remove ourselves before invalidating selfRef, otherwise
    // removeConsumer won't find us (tryGet() would return none).
    KJ_IF_SOME(q, queue) {
      q.removeConsumer(*this);
    }
    // Invalidate after removal so any concurrent iteration will skip us.
    selfRef->invalidate();
  }

  // Called by QueueImpl destructor to detach this consumer from a queue
  // that is about to be destroyed.
  void detachQueue() {
    queue = kj::none;
  }

  void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason) {
    // Already closed or errored - nothing to do.
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      for (auto& request: ready.readRequests) {
        request->resolveAsDone(js);
      }
      state.template transitionTo<Closed>();
    }
  }

  void close(jsg::Lock& js) {
    // If we are already closed or errored, then we do nothing here.
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      // If we are not already closing, enqueue a Close sentinel.
      if (!isClosing()) {
        ready.buffer.push_back(Close{});
      }

      // Then check to see if we need to drain pending reads and
      // update the state to Closed.
      return maybeDrainAndSetState(js);
    }
  }

  inline bool empty() const {
    return size() == 0;
  }

  void error(jsg::Lock& js, jsg::Value reason) {
    // If we are already closed or errored, then we do nothing here.
    // The new error doesn't matter.
    if (state.isActive()) {
      maybeDrainAndSetState(js, kj::mv(reason));
    }
  }

  void push(jsg::Lock& js, kj::Rc<Entry> entry) {
    // If the consumer is already closed or errored, then we do nothing here.
    // This can happen during iteration over consumers in QueueImpl::push() when
    // resolving a read request on one consumer triggers JavaScript code that
    // closes or errors another consumer in the same queue.
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      // If the consumer is already closing or the entry is empty, do nothing.
      // Also skip if queue is none (consumer cloned from closed stream).
      if (isClosing() || entry->getSize() == 0 || queue == kj::none) {
        return;
      }

      UpdateBackpressureScope scope(*this);
      Self::handlePush(js, ready, queue, kj::mv(entry));
    }
  }

  void read(jsg::Lock& js, ReadRequest request) {
    if (state.template is<Closed>()) {
      return request.resolveAsDone(js);
    }
    KJ_IF_SOME(errored, state.tryGetErrorUnsafe()) {
      return request.reject(js, errored.reason);
    }
    auto& ready = state.requireActiveUnsafe();
    // Mutual exclusion with draining reads.
    if (ready.hasPendingDrainingRead) {
      auto error = jsg::Value(
          js.v8Isolate, js.typeError("Cannot call read while there is a pending draining read"_kj));
      return request.reject(js, error);
    }
    Self::handleRead(js, ready, *this, queue, kj::mv(request));
    return maybeDrainAndSetState(js);
  }

  void reset() {
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      UpdateBackpressureScope scope(*this);
      ready.buffer.clear();
      ready.queueTotalSize = 0;
    }
  }

  // The current total calculated size of the consumer's internal buffer.
  size_t size() const {
    return state.whenActiveOr([](const Ready& ready) { return ready.queueTotalSize; }, 0ul);
  }

  void resolveRead(jsg::Lock& js, ReadRequest& req) {
    auto& ready = state.requireActiveUnsafe();
    KJ_REQUIRE(!ready.readRequests.empty());
    KJ_REQUIRE(&req == ready.readRequests.front().get());
    // Pop the request before resolving to ensure the request is fully owned locally.
    auto request = kj::mv(ready.readRequests.front());
    ready.readRequests.pop_front();
    request->resolve(js);
  }

  void resolveReadAsDone(jsg::Lock& js, ReadRequest& req) {
    auto& ready = state.requireActiveUnsafe();
    KJ_REQUIRE(!ready.readRequests.empty());
    KJ_REQUIRE(&req == ready.readRequests.front().get());
    // Pop the request before resolving to ensure the request is fully owned locally.
    auto request = kj::mv(ready.readRequests.front());
    ready.readRequests.pop_front();
    request->resolveAsDone(js);
  }

  void cloneTo(jsg::Lock& js, ConsumerImpl& other) {
    if (state.template is<Closed>()) {
      other.state.template transitionTo<Closed>();
      return;
    }
    KJ_IF_SOME(errored, state.tryGetErrorUnsafe()) {
      other.state.template transitionTo<Errored>(errored.reason.addRef(js));
      return;
    }
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      // We copy the buffered state but not the readRequests.
      auto& otherReady = KJ_REQUIRE_NONNULL(
          other.state.tryGetActiveUnsafe(), "The new consumer should not be closed or errored.");
      otherReady.queueTotalSize = ready.queueTotalSize;
      for (auto& item: ready.buffer) {
        KJ_SWITCH_ONEOF(item) {
          KJ_CASE_ONEOF(c, Close) {
            otherReady.buffer.push_back(Close{});
          }
          KJ_CASE_ONEOF(entry, QueueEntry) {
            otherReady.buffer.push_back(entry.clone(js));
          }
        }
      }
    }
  }

  bool hasReadRequests() const {
    return state.whenActiveOr(
        [](const Ready& ready) { return !ready.readRequests.empty(); }, false);
  }

  void cancelPendingReads(jsg::Lock& js, jsg::JsValue reason) {
    // Already closed or errored - nothing to do.
    state.whenActive([&](Ready& ready) {
      for (auto& request: ready.readRequests) {
        request->resolver.reject(js, reason);
      }
      ready.readRequests.clear();
    });
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    // Technically we shouldn't really have to GC visit the stored error here but there
    // should not be any harm in doing so.
    KJ_IF_SOME(errored, state.tryGetErrorUnsafe()) {
      visitor.visit(errored.reason);
    }
    // There's no reason to GC visit the promise resolver or buffer in Ready state and it is
    // potentially problematic if we do. Since the read requests are queued, if we
    // GC visit it once, remove it from the queue, and GC happens to kick in before
    // we access the resolver, then v8 could determine that the resolver or buffered
    // entries are no longer reachable via tracing and free them before we can
    // actually try to access the held resolver.
  }

  inline kj::StringPtr jsgGetMemoryName() const;
  inline size_t jsgGetMemorySelfSize() const;
  inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // A sentinel used in the buffer to signal that close() has been called.
  struct Close {};

  struct Closed {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "closed"_kj;
  };
  struct Errored {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "errored"_kj;
    jsg::Value reason;
  };
  struct Ready {
    static constexpr kj::StringPtr NAME KJ_UNUSED = "ready"_kj;
    workerd::RingBuffer<kj::OneOf<QueueEntry, Close>, 16> buffer;
    // We use kj::Own<ReadRequest> because ByobRequest holds a reference to its associated
    // ReadRequest. Using RingBuffer directly would invalidate those references when the buffer
    // grows. By heap-allocating each ReadRequest, we ensure reference stability.
    workerd::RingBuffer<kj::Own<ReadRequest>, 8> readRequests;
    size_t queueTotalSize = 0;
    // True if there is a pending draining read operation. Draining reads are mutually
    // exclusive with regular reads - read() will reject if this is true, and drainingRead()
    // will reject if there are pending readRequests.
    bool hasPendingDrainingRead = false;

    inline kj::StringPtr jsgGetMemoryName() const;
    inline size_t jsgGetMemorySelfSize() const;
    inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };

  // State machine for ConsumerImpl:
  //   Ready -> Closed (close() called and drained)
  //   Ready -> Errored (error() called)
  // Closed is terminal, Errored is implicitly terminal via ErrorState.
  using ConsumerState = StateMachine<TerminalStates<Closed>,
      ErrorState<Errored>,
      ActiveState<Ready>,
      Ready,
      Closed,
      Errored>;

  kj::Maybe<QueueImpl&> queue;
  ConsumerState state;
  kj::Maybe<ConsumerImpl::StateListener&> stateListener;
  // WeakRef to this consumer, used for safe registration with QueueImpl.
  // When this consumer is destroyed, we invalidate the WeakRef so that
  // any iteration over allConsumers in QueueImpl will safely skip us.
  kj::Rc<WeakRef<ConsumerImpl>> selfRef =
      kj::rc<WeakRef<ConsumerImpl>>(kj::Badge<ConsumerImpl>{}, *this);

  bool isClosing() {
    // Closing state is determined by whether there is a Close sentinel that has been
    // pushed into the end of Ready state buffer.
    return state.whenActiveOr([](Ready& ready) {
      return !ready.buffer.empty() && ready.buffer.back().template is<Close>();
    }, false);
  }

  void maybeDrainAndSetState(jsg::Lock& js, kj::Maybe<jsg::Value> maybeReason = kj::none) {
    // If the state is already errored or closed then there is nothing to drain.
    KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
      UpdateBackpressureScope scope(*this);
      KJ_IF_SOME(reason, maybeReason) {
        // If maybeReason != nullptr, then we are draining because of an error.
        // In that case, we want to reset/clear the buffer and reject any remaining
        // pending read requests using the given reason.
        for (auto& request: ready.readRequests) {
          request->reject(js, reason);
        }
        state.template transitionTo<Errored>(reason.addRef(js));
        KJ_IF_SOME(listener, stateListener) {
          listener.onConsumerError(js, kj::mv(reason));
          // After this point, we should not assume that this consumer can
          // be safely used at all. It's most likely the stateListener has
          // released it.
        }
      } else {
        // Otherwise, if isClosing() is true...
        if (isClosing()) {
          if (!empty() && !Self::handleMaybeClose(js, ready, *this, queue)) {
            // If the queue is not empty, we'll have the implementation see
            // if it can drain the remaining data into pending reads. If handleMaybeClose
            // returns false, then it could not and we can't yet close. If it returns true,
            // yay! Our queue is empty and we can continue closing down.
            KJ_ASSERT(!empty());  // We're still not empty
            return;
          }

          KJ_ASSERT(empty());
          KJ_REQUIRE(ready.buffer.size() == 1);  // The close should be the only item remaining.
          for (auto& request: ready.readRequests) {
            request->resolveAsDone(js);
          }
          state.template transitionTo<Closed>();
          KJ_IF_SOME(listener, stateListener) {
            listener.onConsumerClose(js);
            // After this point, we should not assume that this consumer can
            // be safely used at all. It's most likely the stateListener has
            // released it.
          }
        }
      }
    }
  }

  friend Self::Consumer;
  friend Self;
};

// ============================================================================
// Value queue

class ValueQueue final {
 public:
  using ConsumerImpl = ConsumerImpl<ValueQueue>;
  using QueueImpl = QueueImpl<ValueQueue>;

  struct State {
    JSG_MEMORY_INFO(ValueQueue::State) {}
  };

  struct ReadRequest {
    jsg::Promise<ReadResult>::Resolver resolver;

    void resolveAsDone(jsg::Lock& js);
    void resolve(jsg::Lock& js, jsg::Value value);
    void reject(jsg::Lock& js, jsg::Value& value);

    JSG_MEMORY_INFO(ValueQueue::ReadRequest) {
      tracker.trackField("resolver", resolver);
    }
  };

  // A value queue entry consists of an arbitrary JavaScript value and a size that is
  // calculated by the size algorithm function provided in the stream constructor.
  class Entry: public kj::Refcounted {
   public:
    explicit Entry(jsg::Value value, size_t size);
    KJ_DISALLOW_COPY_AND_MOVE(Entry);

    jsg::Value getValue(jsg::Lock& js);

    size_t getSize() const;

    void visitForGc(jsg::GcVisitor& visitor);

    kj::Rc<Entry> clone(jsg::Lock& js);

    JSG_MEMORY_INFO(ValueQueue::Entry) {
      tracker.trackField("value", value);
    }

   private:
    jsg::Value value;
    size_t size;
  };

  struct QueueEntry {
    kj::Rc<Entry> entry;
    QueueEntry clone(jsg::Lock& js);

    JSG_MEMORY_INFO(ValueQueue::QueueEntry) {
      tracker.trackFieldWithSize("entry", entry->getSize());
    }
  };

  class Consumer final {
   public:
    Consumer(ValueQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);
    Consumer(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);
    // Used when cloning a consumer whose queue has been destroyed.
    explicit Consumer(kj::Maybe<ConsumerImpl::StateListener&> stateListener);
    Consumer(Consumer&&) = delete;
    Consumer(Consumer&) = delete;
    Consumer& operator=(Consumer&&) = delete;
    Consumer& operator=(Consumer&) = delete;

    void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason);

    void close(jsg::Lock& js);

    bool empty();

    void error(jsg::Lock& js, jsg::Value reason);

    void read(jsg::Lock& js, ReadRequest request);

    // Draining read for optimized pipe-to operations. Drains all currently buffered
    // data, pumps the controller for synchronously available data, and converts
    // all values to bytes. Values must be ArrayBuffer, ArrayBufferView, or string;
    // other types will error the stream.
    // Rejects if there are pending regular reads (mutual exclusion).
    // Regular read() will reject if there is a pending draining read.
    // The maxRead parameter is a soft limit - see ReadableStreamController::drainingRead.
    jsg::Promise<DrainingReadResult> drainingRead(jsg::Lock& js, size_t maxRead = kj::maxValue);

    void push(jsg::Lock& js, kj::Rc<Entry> entry);

    void reset();

    size_t size();

    kj::Own<Consumer> clone(
        jsg::Lock& js, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);

    bool hasReadRequests();
    bool hasPendingDrainingRead();
    void cancelPendingReads(jsg::Lock& js, jsg::JsValue reason);

    void visitForGc(jsg::GcVisitor& visitor);

    inline kj::StringPtr jsgGetMemoryName() const;
    inline size_t jsgGetMemorySelfSize() const;
    inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

   private:
    ConsumerImpl impl;

    friend class ValueQueue;
  };

  explicit ValueQueue(size_t highWaterMark);

  void close(jsg::Lock& js);

  ssize_t desiredSize() const;

  void error(jsg::Lock& js, jsg::Value reason);

  void maybeUpdateBackpressure();

  void push(jsg::Lock& js, kj::Rc<Entry> entry);

  size_t size() const;

  size_t getConsumerCount();

  bool wantsRead() const;

  bool hasPartiallyFulfilledRead();

  void visitForGc(jsg::GcVisitor& visitor);

  inline kj::StringPtr jsgGetMemoryName() const;
  inline size_t jsgGetMemorySelfSize() const;
  inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  QueueImpl impl;

  static void handlePush(
      jsg::Lock& js, ConsumerImpl::Ready& state, kj::Maybe<QueueImpl&> queue, kj::Rc<Entry> entry);
  static void handleRead(jsg::Lock& js,
      ConsumerImpl::Ready& state,
      ConsumerImpl& consumer,
      kj::Maybe<QueueImpl&> queue,
      ReadRequest request);
  static bool handleMaybeClose(jsg::Lock& js,
      ConsumerImpl::Ready& state,
      ConsumerImpl& consumer,
      kj::Maybe<QueueImpl&> queue);

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
    // The reference here should be cleared when the ByobRequest is invalidated,
    // which happens either when respond(), respondWithNewView(), or invalidate()
    // is called, or when the ByobRequest is destroyed, whichever comes first.
    kj::Maybe<ByobRequest&> byobReadRequest;

    struct PullInto {
      jsg::BufferSource store;
      size_t filled = 0;
      size_t atLeast = 1;
      Type type = Type::DEFAULT;

      JSG_MEMORY_INFO(ByteQueue::ReadRequest::PullInto) {
        tracker.trackField("store", store);
      }
    } pullInto;

    ReadRequest(jsg::Promise<ReadResult>::Resolver resolver, PullInto pullInto);
    ReadRequest(ReadRequest&&) = default;
    ReadRequest& operator=(ReadRequest&&) = default;
    ~ReadRequest() noexcept(false);
    void resolveAsDone(jsg::Lock& js);
    void resolve(jsg::Lock& js);
    void reject(jsg::Lock& js, jsg::Value& value);

    kj::Own<ByobRequest> makeByobReadRequest(ConsumerImpl& consumer, QueueImpl& queue);

    JSG_MEMORY_INFO(ByteQueue::ReadRequest) {
      tracker.trackField("resolver", resolver);
      tracker.trackField("pullInto", pullInto);
    }
  };

  // The ByobRequest is essentially a handle to the ByteQueue::ReadRequest that can be given to a
  // ReadableStreamBYOBRequest object to fulfill the request using the BYOB API pattern.
  //
  // When isInvalidated() is false, respond() or respondWithNewView() can be called to fulfill
  // the BYOB read request. Once either of those are called, or once invalidate() is called,
  // the ByobRequest is no longer usable and should be discarded.
  class ByobRequest final {
   public:
    ByobRequest(ReadRequest& request, ConsumerImpl& consumer, QueueImpl& queue)
        : request(request),
          consumer(consumer),
          queue(queue) {}

    KJ_DISALLOW_COPY_AND_MOVE(ByobRequest);

    ~ByobRequest() noexcept(false);

    inline ReadRequest& getRequest() {
      return KJ_ASSERT_NONNULL(request);
    }

    bool respond(jsg::Lock& js, size_t amount);

    bool respondWithNewView(jsg::Lock& js, jsg::BufferSource view);

    // Disconnects this ByobRequest instance from the associated ByteQueue::ReadRequest.
    // The term "invalidate" is adopted from the streams spec for handling BYOB requests.
    void invalidate();

    inline bool isInvalidated() const {
      return request == kj::none;
    }

    bool isPartiallyFulfilled();

    size_t getAtLeast() const;

    v8::Local<v8::Uint8Array> getView(jsg::Lock& js);

    // Returns the byte length of the original underlying ArrayBuffer.
    size_t getOriginalBufferByteLength(jsg::Lock& js) const;

    // Returns the byte offset of the original view plus bytes filled.
    size_t getOriginalByteOffsetPlusBytesFilled() const;

    JSG_MEMORY_INFO(ByteQueue::ByobRequest) {}

   private:
    kj::Maybe<ReadRequest&> request;
    ConsumerImpl& consumer;
    QueueImpl& queue;
  };

  struct State {
    // We use a ring buffer for pending BYOB read requests. Since we store kj::Own<ByobRequest>,
    // the actual ByobRequest objects are heap-allocated and won't be invalidated by buffer growth.
    workerd::RingBuffer<kj::Own<ByobRequest>, 8> pendingByobReadRequests;

    JSG_MEMORY_INFO(ByteQueue::State) {
      for (auto& request: pendingByobReadRequests) {
        tracker.trackField("pendingByobReadRequest", request);
      }
    }
  };

  // A byte queue entry consists of a jsg::BufferSource containing a non-zero-length
  // sequence of bytes. The size is determined by the number of bytes in the entry.
  class Entry: public kj::Refcounted {
   public:
    explicit Entry(jsg::BufferSource store);

    kj::ArrayPtr<kj::byte> toArrayPtr();

    size_t getSize() const;

    void visitForGc(jsg::GcVisitor& visitor);

    kj::Rc<Entry> clone(jsg::Lock& js);

    JSG_MEMORY_INFO(ByteQueue::Entry) {
      tracker.trackField("store", store);
    }

   private:
    jsg::BufferSource store;
  };

  struct QueueEntry {
    kj::Rc<Entry> entry;
    size_t offset;

    QueueEntry clone(jsg::Lock& js);

    JSG_MEMORY_INFO(ByteQueue::QueueEntry) {
      tracker.trackFieldWithSize("entry", entry->getSize());
    }
  };

  class Consumer {
   public:
    Consumer(ByteQueue& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);
    Consumer(QueueImpl& queue, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);
    // Used when cloning a consumer whose queue has been destroyed.
    explicit Consumer(kj::Maybe<ConsumerImpl::StateListener&> stateListener);
    Consumer(Consumer&&) = delete;
    Consumer(Consumer&) = delete;
    Consumer& operator=(Consumer&&) = delete;
    Consumer& operator=(Consumer&) = delete;

    void cancel(jsg::Lock& js, jsg::Optional<v8::Local<v8::Value>> maybeReason);

    void close(jsg::Lock& js);

    bool empty() const;

    void error(jsg::Lock& js, jsg::Value reason);

    void read(jsg::Lock& js, ReadRequest request);

    // Draining read for optimized pipe-to operations. Drains all currently buffered
    // data and pumps the controller for synchronously available data.
    // Returns bytes directly without conversion (data is already bytes).
    // Rejects if there are pending regular reads (mutual exclusion).
    // Regular read() will reject if there is a pending draining read.
    // The maxRead parameter is a soft limit - see ReadableStreamController::drainingRead.
    jsg::Promise<DrainingReadResult> drainingRead(jsg::Lock& js, size_t maxRead = kj::maxValue);

    void push(jsg::Lock& js, kj::Rc<Entry> entry);

    void reset();

    size_t size() const;

    kj::Own<Consumer> clone(
        jsg::Lock& js, kj::Maybe<ConsumerImpl::StateListener&> stateListener = kj::none);
    bool hasReadRequests();
    bool hasPendingDrainingRead();
    void cancelPendingReads(jsg::Lock& js, jsg::JsValue reason);

    void visitForGc(jsg::GcVisitor& visitor);

    inline kj::StringPtr jsgGetMemoryName() const;
    inline size_t jsgGetMemorySelfSize() const;
    inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

   private:
    ConsumerImpl impl;
  };

  explicit ByteQueue(size_t highWaterMark);

  void close(jsg::Lock& js);

  ssize_t desiredSize() const;

  void error(jsg::Lock& js, jsg::Value reason);

  void maybeUpdateBackpressure();

  void push(jsg::Lock& js, kj::Rc<Entry> entry);

  size_t size() const;

  size_t getConsumerCount();

  bool wantsRead() const;

  bool hasPartiallyFulfilledRead();

  // nextPendingByobReadRequest will be used to support the ReadableStreamBYOBRequest interface
  // that is part of ReadableByteStreamController. When user code calls the `controller.byobRequest`
  // API on a ReadableByteStreamController, they are going to get an instance of a
  // ReadableStreamBYOBRequest object. That object will own the `kj::Own<ByobReadRequest>` that
  // is returned here. User code could end up doing something silly like holding a reference to
  // that byobRequest long after it has been invalidated. We heap-allocate these just to allow
  // their lifespan to be attached to the ReadableStreamBYOBRequest object but internally they
  // will be disconnected as appropriate.
  kj::Maybe<kj::Own<ByobRequest>> nextPendingByobReadRequest();

  void visitForGc(jsg::GcVisitor& visitor);

  inline kj::StringPtr jsgGetMemoryName() const;
  inline size_t jsgGetMemorySelfSize() const;
  inline void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  QueueImpl impl;

  static void handlePush(
      jsg::Lock& js, ConsumerImpl::Ready& state, kj::Maybe<QueueImpl&> queue, kj::Rc<Entry> entry);
  static void handleRead(jsg::Lock& js,
      ConsumerImpl::Ready& state,
      ConsumerImpl& consumer,
      kj::Maybe<QueueImpl&> queue,
      ReadRequest request);
  static bool handleMaybeClose(jsg::Lock& js,
      ConsumerImpl::Ready& state,
      ConsumerImpl& consumer,
      kj::Maybe<QueueImpl&> queue);

  friend ConsumerImpl;
  friend class Consumer;
};

template <typename Self>
kj::StringPtr QueueImpl<Self>::jsgGetMemoryName() const {
  return "QueueImpl"_kjc;
}

template <typename Self>
size_t QueueImpl<Self>::jsgGetMemorySelfSize() const {
  return sizeof(QueueImpl<Self>);
}

template <typename Self>
void QueueImpl<Self>::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(errored, state.tryGetErrorUnsafe()) {
    tracker.trackField("error", errored.reason);
  }
}

template <typename Self>
kj::StringPtr ConsumerImpl<Self>::jsgGetMemoryName() const {
  return "ConsumerImpl"_kjc;
}

template <typename Self>
size_t ConsumerImpl<Self>::jsgGetMemorySelfSize() const {
  return sizeof(ConsumerImpl<Self>);
}

template <typename Self>
void ConsumerImpl<Self>::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_IF_SOME(errored, state.tryGetErrorUnsafe()) {
    tracker.trackField("error", errored.reason);
  } else KJ_IF_SOME(ready, state.tryGetActiveUnsafe()) {
    tracker.trackField("inner", ready);
  }
}

template <typename Self>
kj::StringPtr ConsumerImpl<Self>::Ready::jsgGetMemoryName() const {
  return "ConsumerImpl::Ready"_kjc;
}

template <typename Self>
size_t ConsumerImpl<Self>::Ready::jsgGetMemorySelfSize() const {
  return sizeof(Ready);
}

template <typename Self>
void ConsumerImpl<Self>::Ready::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (auto& entry: buffer) {
    KJ_SWITCH_ONEOF(entry) {
      KJ_CASE_ONEOF(c, Close) {
        tracker.trackFieldWithSize("pendingClose", sizeof(Close));
      }
      KJ_CASE_ONEOF(e, QueueEntry) {
        tracker.trackField("entry", e);
      }
    }
  }

  for (auto& request: readRequests) {
    tracker.trackField("pendingRead", *request);
  }
}

kj::StringPtr ValueQueue::Consumer::jsgGetMemoryName() const {
  return "ValueQueue::Consumer"_kjc;
}

size_t ValueQueue::Consumer::jsgGetMemorySelfSize() const {
  return sizeof(ValueQueue::Consumer);
}

void ValueQueue::Consumer::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

kj::StringPtr ValueQueue::jsgGetMemoryName() const {
  return "ValueQueue"_kjc;
}

size_t ValueQueue::jsgGetMemorySelfSize() const {
  return sizeof(ValueQueue);
}

void ValueQueue::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

kj::StringPtr ByteQueue::Consumer::jsgGetMemoryName() const {
  return "ByteQueue::Consumer"_kjc;
}

size_t ByteQueue::Consumer::jsgGetMemorySelfSize() const {
  return sizeof(ByteQueue::Consumer);
}

void ByteQueue::Consumer::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

kj::StringPtr ByteQueue::jsgGetMemoryName() const {
  return "ByteQueue"_kjc;
}

size_t ByteQueue::jsgGetMemorySelfSize() const {
  return sizeof(ByteQueue);
}

void ByteQueue::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("impl", impl);
}

}  // namespace workerd::api
