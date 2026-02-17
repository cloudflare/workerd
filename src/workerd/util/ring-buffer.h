// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>

#include <iterator>

namespace workerd {

// A simple ring buffer with amortized O(1) push/pop at both ends and O(1) random access.
// The initial capacity can be specified as a template parameter (default 16). The buffer
// will grow as needed. The type T must be Moveable and/or Copyable.
//
// The purpose of this class is to provide a more memory-efficient alternative to std::list
// for use in places where we need a double-ended queue with stable iterators and references
// but avoids the memory overhead of std::list's per-node allocations and has better cache
// locality than std::list. The trade-off is that iterators and references may be
// invalidated when the buffer grows, which is not the case with std::list. However,
// in our use cases, we do not rely on iterator/reference stability across growth
// operations, so this fine.
template <typename T, size_t InitialCapacity = 16>
class RingBuffer final {
 public:
  RingBuffer(): storage(kj::heapArray<kj::byte>(sizeof(T) * InitialCapacity)) {}

  ~RingBuffer() {
    clear();
  }

  RingBuffer(RingBuffer&& other) noexcept
      : storage(kj::mv(other.storage)),
        head(other.head),
        tail(other.tail),
        count(other.count),
        generation(other.generation) {
    other.head = 0;
    other.tail = 0;
    other.count = 0;
    other.generation = 0;
  }

  RingBuffer& operator=(RingBuffer&& other) noexcept {
    if (this != &other) {
      clear();

      storage = kj::mv(other.storage);
      head = other.head;
      tail = other.tail;
      count = other.count;
      generation = other.generation;

      other.head = 0;
      other.tail = 0;
      other.count = 0;
      other.generation = 0;
    }
    return *this;
  }

  KJ_DISALLOW_COPY(RingBuffer);

  bool empty() const {
    return count == 0;
  }
  size_t size() const {
    return count;
  }

  class iterator;
  class const_iterator;

  class iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    iterator(): buffer(nullptr), index(0) {}

    reference operator*() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->capacity();
      return buffer->slot(physicalIndex);
    }

    pointer operator->() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->capacity();
      return &buffer->slot(physicalIndex);
    }

    iterator& operator++() {
      KJ_DREQUIRE(buffer != nullptr);
      ++index;
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    iterator& operator--() {
      KJ_DREQUIRE(buffer != nullptr);
      --index;
      return *this;
    }

    iterator operator--(int) {
      iterator tmp = *this;
      --(*this);
      return tmp;
    }

    bool operator==(const iterator& other) const {
      return buffer == other.buffer && index == other.index;
    }

    bool operator!=(const iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class RingBuffer;
    friend class const_iterator;

    iterator(RingBuffer* buf, size_t idx): buffer(buf), index(idx) {}

    RingBuffer* buffer;
    size_t index;  // Logical index (0 to count-1)
  };

  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    const_iterator(): buffer(nullptr), index(0) {}

    const_iterator(const iterator& it): buffer(it.buffer), index(it.index) {}

    reference operator*() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->capacity();
      return buffer->slot(physicalIndex);
    }

    pointer operator->() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->capacity();
      return &buffer->slot(physicalIndex);
    }

    const_iterator& operator++() {
      KJ_DREQUIRE(buffer != nullptr);
      ++index;
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    const_iterator& operator--() {
      KJ_DREQUIRE(buffer != nullptr);
      --index;
      return *this;
    }

    const_iterator operator--(int) {
      const_iterator tmp = *this;
      --(*this);
      return tmp;
    }

    bool operator==(const const_iterator& other) const {
      return buffer == other.buffer && index == other.index;
    }

    bool operator!=(const const_iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class RingBuffer;

    const_iterator(const RingBuffer* buf, size_t idx): buffer(buf), index(idx) {}

    const RingBuffer* buffer;
    size_t index;
  };

  iterator begin() {
    return iterator(this, 0);
  }

  iterator end() {
    return iterator(this, count);
  }

  const_iterator begin() const {
    return const_iterator(this, 0);
  }

  const_iterator end() const {
    return const_iterator(this, count);
  }

  const_iterator cbegin() const {
    return const_iterator(this, 0);
  }

  const_iterator cend() const {
    return const_iterator(this, count);
  }

  void push_back(T&& item) {
    if (count == capacity()) {
      grow();
    }
    // Use placement new - the slot is uninitialized raw memory
    new (&slot(tail)) T(kj::mv(item));
    tail = (tail + 1) % capacity();
    count++;
  }

  void push_back(const T& item) {
    if (count == capacity()) {
      grow();
    }
    // Use placement new - the slot is uninitialized raw memory
    new (&slot(tail)) T(item);
    tail = (tail + 1) % capacity();
    count++;
  }

  template <typename... Args>
  T& emplace_back(Args&&... args) {
    if (count == capacity()) {
      grow();
    }
    // Use placement new to construct in place at the tail position
    T* ptr = new (&slot(tail)) T(kj::fwd<Args>(args)...);
    tail = (tail + 1) % capacity();
    count++;
    return *ptr;
  }

  void pop_front() {
    KJ_DREQUIRE(count > 0);
    slot(head).~T();
    head = (head + 1) % capacity();
    count--;
    generation++;
  }

  // Returns a generation counter that is incremented each time pop_front() is called.
  // This can be used to detect if the front of the queue has changed during async operations,
  // since RingBuffer may relocate elements when it grows and pointer/reference comparisons
  // are not reliable.
  uint64_t currentGeneration() const {
    return generation;
  }

  T& front() {
    KJ_DREQUIRE(count > 0);
    return slot(head);
  }

  T& back() {
    KJ_DREQUIRE(count > 0);
    size_t backIdx = (tail == 0) ? capacity() - 1 : tail - 1;
    KJ_REQUIRE(backIdx < capacity());
    return slot(backIdx);
  }

  void clear() {
    while (count > 0) {
      slot(head).~T();
      head = (head + 1) % capacity();
      count--;
    }
    // Reset to initial state
    head = 0;
    tail = 0;
  }

 private:
  kj::Array<kj::byte> storage;
  size_t head = 0;
  size_t tail = 0;
  size_t count = 0;
  uint64_t generation = 0;  // Incremented on each pop_front()

  size_t capacity() const {
    return storage.size() / sizeof(T);
  }

  T& slot(size_t index) {
    return reinterpret_cast<T*>(storage.begin())[index];
  }

  const T& slot(size_t index) const {
    return reinterpret_cast<const T*>(storage.begin())[index];
  }

  void grow() {
    size_t oldCapacity = capacity();
    size_t newCapacity = oldCapacity * 2;
    auto newStorage = kj::heapArray<kj::byte>(sizeof(T) * newCapacity);
    T* newSlots = reinterpret_cast<T*>(newStorage.begin());

    // Move-construct elements to new storage using placement new
    for (size_t i = 0; i < count; i++) {
      new (&newSlots[i]) T(kj::mv(slot((head + i) % oldCapacity)));
      slot((head + i) % oldCapacity).~T();
    }

    storage = kj::mv(newStorage);
    head = 0;
    tail = count;
  }
};

}  // namespace workerd
