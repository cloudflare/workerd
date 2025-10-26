// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/debug.h>
#include <kj/vector.h>

#include <iterator>

namespace workerd {

// A simple ring buffer with amortized O(1) push/pop at both ends and O(1) random access
// built on top of kj::Vector but with the same interface as std::list. The initial capacity
// can be specified as a template parameter (default 16). The buffer will grow as needed.
// The type T must be Moveable and/or Copyable.
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
  RingBuffer() {
    storage.resize(InitialCapacity);
  }

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
      size_t physicalIndex = (buffer->head + index) % buffer->storage.size();
      return buffer->storage[physicalIndex];
    }

    pointer operator->() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->storage.size();
      return &buffer->storage[physicalIndex];
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
      size_t physicalIndex = (buffer->head + index) % buffer->storage.size();
      return buffer->storage[physicalIndex];
    }

    pointer operator->() const {
      KJ_DREQUIRE(buffer != nullptr);
      size_t physicalIndex = (buffer->head + index) % buffer->storage.size();
      return &buffer->storage[physicalIndex];
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
    if (count == storage.size()) {
      grow();
    }
    storage[tail] = kj::mv(item);
    tail = (tail + 1) % storage.size();
    count++;
  }

  void push_back(const T& item) {
    if (count == storage.size()) {
      grow();
    }
    storage[tail] = item;
    tail = (tail + 1) % storage.size();
    count++;
  }

  template <typename... Args>
  T& emplace_back(Args&&... args) {
    if (count == storage.size()) {
      grow();
    }
    // Construct in place at the tail position
    storage[tail] = T(kj::fwd<Args>(args)...);
    size_t insertedIndex = tail;
    tail = (tail + 1) % storage.size();
    count++;
    return storage[insertedIndex];
  }

  void pop_front() {
    KJ_DREQUIRE(count > 0);
    storage[head].~T();
    head = (head + 1) % storage.size();
    count--;
  }

  T& front() {
    KJ_DREQUIRE(count > 0);
    return storage[head];
  }

  T& back() {
    KJ_DREQUIRE(count > 0);
    size_t backIdx = (tail == 0) ? storage.size() - 1 : tail - 1;
    return storage[backIdx];
  }

  void clear() {
    while (count > 0) {
      storage[head].~T();
      head = (head + 1) % storage.size();
      count--;
    }
    // Reset to initial state
    head = 0;
    tail = 0;
  }

 private:
  kj::Vector<T> storage;
  size_t head = 0;
  size_t tail = 0;
  size_t count = 0;

  void grow() {
    size_t newCapacity = storage.size() * 2;
    kj::Vector<T> newStorage;
    newStorage.resize(newCapacity);

    for (size_t i = 0; i < count; i++) {
      newStorage[i] = kj::mv(storage[(head + i) % storage.size()]);
    }

    storage = kj::mv(newStorage);
    head = 0;
    tail = count;
  }
};

}  // namespace workerd
