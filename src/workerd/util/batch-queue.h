// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/vector.h>
#include <utility>

namespace workerd {

using kj::uint;

// A double-buffered batch queue which enforces an upper bound on buffer growth.
//
// Objects of this type have two buffers -- the push buffer and the pop buffer -- and support
// `push()` and `pop()` operations. `push()` adds elements to the push buffer. `pop()` swaps the
// push and the pop buffers and returns a RAII object which provides a view onto the pop buffer.
// When the RAII object is destroyed, it resets the size and capacity of the pop buffer.
//
// This class is useful when the cost of context switching between producers and consumers is
// high and/or when you must be able to gracefully handle bursts of pushes, such as when
// transferring objects between threads. Note that this class implements no cross-thread
// synchronization itself, but it can become an effective multiple-producer, single-consumer queue
// when wrapped as a `kj::MutexGuarded<BatchQueue<T>>`.
template <typename T>
class BatchQueue {
public:
  // `initialCapacity` is the number of elements of type T for which we should allocate space in the
  // initial buffers, and any reconstructed buffers. Buffers will be reconstructed if they are
  // observed to grow beyond `maxCapacity` after a completed pop operation.
  explicit BatchQueue(uint initialCapacity, uint maxCapacity)
      : pushBuffer(initialCapacity),
        popBuffer(initialCapacity),
        initialCapacity(initialCapacity),
        maxCapacity(maxCapacity) {}

  // This is the return type of `pop()` (in fact, `pop()` is the only way to construct a non-empty
  // Batch). Default-constructible, moveable, and non-copyable.
  //
  // A Batch can be converted to an ArrayPtr<T>. When a Batch is destroyed, it clears the pop
  // buffer and resets the pop buffer capacity to `initialCapacity` if necessary.
  class Batch {
  public:
    Batch() = default;
    Batch(Batch&&) = default;
    Batch& operator=(Batch&&) = default;
    ~Batch() noexcept(false);
    KJ_DISALLOW_COPY(Batch);

    operator kj::ArrayPtr<T>() {
      return batchQueue
          .map([](auto& bq) -> kj::ArrayPtr<T> { return bq.popBuffer; })
          .orDefault(nullptr);
    }

    kj::ArrayPtr<T> asArrayPtr() { return *this; }

  private:
    explicit Batch(BatchQueue& batchQueue): batchQueue(batchQueue) {}
    friend BatchQueue;

    kj::Maybe<BatchQueue<T>&> batchQueue;
    // It's a Maybe so we can support move operations.
  };

  // If a batch is available, swap the buffers and return a Batch object backed by the pop buffer.
  // The caller should destroy the Batch object as soon as they are done with it. Destruction will
  // clear the pop buffer and, if necessary, reconstruct it to stay under `maxCapacity`.
  //
  // Throws if `pop()` is called again before the previous Batch object was destroyed. Note
  // that this exception is only reliable if the previous `pop()` returned a non-empty Batch.
  //
  // `pop()` accesses both buffers, so it must be synchronized with `push()` operations across
  // threads. Batch objects and `push()` access different buffers, so they require no explicit
  // cross-thread synchronization with each other.
  Batch pop() {
    KJ_REQUIRE(popBuffer.empty(), "pop()'s previous result not yet destroyed.");

    Batch batch;

    if (!pushBuffer.empty()) {
      std::swap(pushBuffer, popBuffer);
      batch = Batch(*this);
    }

    return batch;
  }

  // Add an item to the current batch.
  template <typename U>
  void push(U&& value) {
    pushBuffer.add(kj::fwd<U>(value));
  }

  auto empty() const { return pushBuffer.empty(); }
  auto size() const { return pushBuffer.size(); }

private:
  kj::Vector<T> pushBuffer;
  kj::Vector<T> popBuffer;
  uint initialCapacity;
  uint maxCapacity;
};

// =======================================================================================
// Inline implementation details

template <typename T>
BatchQueue<T>::Batch::~Batch() noexcept(false) {
  KJ_IF_MAYBE(bq, batchQueue) {
    bq->popBuffer.clear();
    if (auto capacity = bq->popBuffer.capacity(); capacity > bq->maxCapacity) {
      // Reset the queue to avoid letting it grow unbounded.
      bq->popBuffer = kj::Vector<T>(bq->initialCapacity);
    }
  }
}

}  // namespace workerd
