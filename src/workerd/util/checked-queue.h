#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/exception.h>

#include <concepts>
#include <list>

namespace workerd::util {

template <typename T>
concept Movable = std::is_move_constructible_v<T> && std::is_move_assignable_v<T>;

// A simple wrapper around std::list that provides a checked queue interface,
// ensuring that items can only be moved out of the queue if they exist.
// Members are not copyable, only movable. The intention here is to provide
// a safe-to-use queue that avoids the pitfalls of using std::list directly
// (such as accidentally dangling references when the list is empty but someone
// calls front(), etc).
template <Movable T>
class Queue final {
 public:
  Queue() = default;
  Queue(Queue<T>&&) = default;
  Queue<T>& operator=(Queue<T>&&) = default;
  KJ_DISALLOW_COPY(Queue);

  inline void push(T&& value) {
    inner.push_back(kj::mv(value));
  }

  template <typename... Args>
  T& emplace(Args&&... args) KJ_LIFETIMEBOUND {
    return inner.emplace_back(std::forward<Args>(args)...);
  }

  // Pops the front element from the queue, moving it out.
  // Returns kj::none if the queue is empty.
  inline kj::Maybe<T> pop() {
    if (inner.empty()) {
      return kj::none;
    }
    T value = kj::mv(inner.front());
    inner.pop_front();
    return kj::mv(value);
    inner.pop_front();
    return kj::mv(value);
  }

  // Returns a reference to the front element without removing it.
  // Returns kj::none if the queue is empty.
  inline kj::Maybe<T&> peek() KJ_LIFETIMEBOUND {
    if (inner.empty()) {
      return kj::none;
    }
    return inner.front();
  }

  // Returns a reference to the front element without removing it.
  // Returns kj::none if the queue is empty.
  inline kj::Maybe<const T&> peek() const KJ_LIFETIMEBOUND {
    if (inner.empty()) {
      return kj::none;
    }
    return inner.front();
  }

  // Returns a reference to the last element without removing it.
  // Returns kj::none if the queue is empty.
  inline kj::Maybe<T&> peekBack() KJ_LIFETIMEBOUND {
    if (inner.empty()) {
      return kj::none;
    }
    return inner.back();
  }

  // Returns a reference to the last element without removing it.
  // Returns kj::none if the queue is empty.
  inline kj::Maybe<const T&> peekBack() const KJ_LIFETIMEBOUND {
    if (inner.empty()) {
      return kj::none;
    }
    return inner.back();
  }

  // Drains the queue, moving each element to the callback one at a time.
  // Returns the number of elements moved.
  inline size_t drainTo(auto callback) {
    size_t count = 0;
    while (!inner.empty()) {
      callback(KJ_ASSERT_NONNULL(pop()));
      count++;
    }
    return count;
  }

  // Removes elements from the queue that satisfy the given condition.
  // Returns the number of elements removed.
  inline size_t deleteIf(auto callback) {
    size_t count = 0;
    auto it = inner.begin();
    while (it != inner.end()) {
      if (callback(*it)) {
        it = inner.erase(it);
        count++;
      } else {
        ++it;
      }
    }
    return count;
  }

  // Takes the first element in the queue that satisfies the given condition, if any.
  inline kj::Maybe<T> takeIf(auto callback) {
    for (auto it = inner.begin(); it != inner.end(); ++it) {
      if (callback(*it)) {
        T value = kj::mv(*it);
        inner.erase(it);
        return kj::mv(value);
      }
    }
    return kj::none;
  }

  // Applies the callback to each element in the queue.
  // Returns the number of elements processed.
  // If the callback returns false, the iteration stops.
  inline size_t forEach(auto callback) const {
    size_t count = 0;
    for (const auto& item: inner) {
      count++;
      if constexpr (std::is_void_v<decltype(callback(item))>) {
        callback(item);
      } else {
        if (!callback(item)) break;
      }
    }
    return count;
  }

  // Checks if the queue is empty.
  inline bool empty() const {
    return inner.empty();
  }

  // Returns the number of elements in the queue.
  inline size_t size() const {
    return inner.size();
  }

  // Clears the queue, removing all elements.
  inline void clear() {
    inner.clear();
  }

  // Swap the contents of this queue with another.
  inline void swap(Queue<T>& other) {
    inner.swap(other.inner);
  }

 private:
  std::list<T> inner;

  // Delete the new and delete operators to prevent heap allocation.
  void* operator new(size_t) = delete;
  void operator delete(void*) = delete;
};
}  // namespace workerd::util
