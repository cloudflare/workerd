// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/memory.h>
#include <kj/one-of.h>
#include <kj/vector.h>

namespace workerd {

// A vector-like container optimized for the common case of storing 0-2 items
// of kj::Weak<T> references.
//
// This uses a kj::OneOf to avoid heap allocations for small vectors.
//
// Performance characteristics:
// - 0-1 items: Zero heap allocations, O(1) operations
// - 2 items: Zero heap allocations, O(1) operations
// - 3+ items: Single heap allocation (kj::Vector), O(n) operations
//
// Typical usage patterns:
// - 99% of instances have 1 item
// - 0.9% of instances have 2 items
// - 0.1% of instances have 3+ items
//
// This is NOT a drop-in replacement for std::vector because:
// - No stable ordering guarantees after removal
// - Optimized for small sizes only
//
// Iterator invalidation:
// - Iterators are invalidated when items are removed or storage state changes
// - If iterating over items that may be removed during iteration, use forEach().
// - Iterators must not be held across operations that may destroy targets; use forEach() instead.
//
// SmallWeakVector only stores kj::Weak<T> items. Weak entries do not keep their targets alive;
// forEach() skips entries whose targets have expired.
template <typename T>
class SmallWeakVector {
 public:
  SmallWeakVector() = default;
  KJ_DISALLOW_COPY(SmallWeakVector);
  SmallWeakVector(SmallWeakVector&&) = default;
  SmallWeakVector& operator=(SmallWeakVector&&) = default;

  // Add an item to the vector. The item is moved into the vector.
  // For move-only types, use containsIf() first to check for duplicates if needed.
  void add(kj::Weak<T> item) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        storage = Single(kj::mv(item));
        return;
      }
      KJ_CASE_ONEOF(single, Single) {
        storage = Double(kj::mv(single.item), kj::mv(item));
        return;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        auto vec = kj::Vector<kj::Weak<T>>(4);
        vec.add(kj::mv(dbl.first));
        vec.add(kj::mv(dbl.second));
        vec.add(kj::mv(item));
        storage = kj::mv(vec);
        return;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        vec.add(kj::mv(item));
        return;
      }
    }
    KJ_UNREACHABLE;
  }

  // Remove all live targets matching the predicate.
  // The predicate receives a reference to the target.
  template <typename Predicate>
  void removeAll(Predicate&& predicate) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return;
      }
      KJ_CASE_ONEOF(single, Single) {
        KJ_IF_SOME(ref, single.item.tryGet()) {
          if (!predicate(ref)) return;
          storage = None();
        }
        return;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        bool removeFirst = false;
        KJ_IF_SOME(ref, dbl.first.tryGet()) {
          removeFirst = predicate(ref);
        }

        bool removeSecond = false;
        KJ_IF_SOME(ref, dbl.second.tryGet()) {
          removeSecond = predicate(ref);
        }

        if (removeFirst && removeSecond) {
          storage = None();
        } else if (removeFirst) {
          if (dbl.second == nullptr) {
            storage = None();
          } else {
            storage = Single(kj::mv(dbl.second));
          }
        } else if (removeSecond) {
          if (dbl.first == nullptr) {
            storage = None();
          } else {
            storage = Single(kj::mv(dbl.first));
          }
        }
        return;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        bool removed = false;
        for (size_t i = 0; i < vec.size();) {
          KJ_IF_SOME(ref, vec[i].tryGet()) {
            if (!predicate(ref)) {
              ++i;
              continue;
            }

            if (i < vec.size() - 1) {
              vec[i] = kj::mv(vec.back());
            }
            vec.removeLast();
            removed = true;
            continue;
          }

          ++i;
        }

        if (removed) {
          // Transition back to smaller state if appropriate
          if (vec.size() == 2) {
            storage = Double(kj::mv(vec[0]), kj::mv(vec[1]));
          } else if (vec.size() == 1) {
            storage = Single(kj::mv(vec[0]));
          } else if (vec.size() == 0) {
            storage = None();
          }
          // else: vec.size() >= 3, stay in Vector state
        }
        return;
      }
    }
    KJ_UNREACHABLE;
  }

  // Check if the vector contains a live target matching the predicate.
  // The predicate receives a reference to the target.
  template <typename Predicate>
  bool containsIf(Predicate&& predicate) const {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return false;
      }
      KJ_CASE_ONEOF(single, Single) {
        KJ_IF_SOME(ref, single.item.tryGet()) {
          return predicate(ref);
        }
        return false;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        KJ_IF_SOME(ref, dbl.first.tryGet()) {
          if (predicate(ref)) return true;
        }
        KJ_IF_SOME(ref, dbl.second.tryGet()) {
          if (predicate(ref)) return true;
        }
        return false;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        for (auto& existing: vec) {
          KJ_IF_SOME(ref, existing.tryGet()) {
            if (predicate(ref)) return true;
          }
        }
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  // Get the number of stored weak refs, including expired refs that haven't been removed.
  size_t size() const {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return 0;
      }
      KJ_CASE_ONEOF(single, Single) {
        return 1;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        return 2;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        return vec.size();
      }
    }
    KJ_UNREACHABLE;
  }

  // Check if the vector is empty.
  bool empty() const {
    return size() == 0;
  }

  // Clear all items from the vector.
  void clear() {
    storage = None();
  }

  // Iterate over all valid (non-expired) Weak items, calling func for each.
  // This is safe to use even if func modifies the vector (e.g., removes items).
  //
  // Example:
  //   SmallWeakVector<Consumer> consumers;
  //   consumers.forEach([&](Consumer& c) {
  //     c.close(js);  // Safe even if this removes other consumers
  //   });
  template <typename F>
  void forEach(F&& func) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return;
      }
      KJ_CASE_ONEOF(single, Single) {
        KJ_IF_SOME(ref, single.item.tryGet()) {
          func(ref);
        }
        return;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        // The storage state may change during iteration if func modifies the vector,
        // so we take snapshots of the weak pointers first.
        auto first = kj::Weak<T>(dbl.first);
        auto second = kj::Weak<T>(dbl.second);
        KJ_IF_SOME(ref, first.tryGet()) {
          func(ref);
        }
        // Re-check liveness in case func invalidated the second item.
        KJ_IF_SOME(ref, second.tryGet()) {
          func(ref);
        }
        return;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        // The storage state may change during iteration if func modifies the vector,
        // so we take snapshots of the weak pointers first.
        auto snapshot = KJ_MAP(item, vec) { return kj::Weak<T>(item); };
        for (auto& item: snapshot) {
          // Re-check liveness in case func invalidated prior items.
          KJ_IF_SOME(ref, item.tryGet()) {
            func(ref);
          }
        }
        return;
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  struct None {};

  struct Single {
    kj::Weak<T> item;
    explicit Single(kj::Weak<T> item): item(kj::mv(item)) {}
  };

  struct Double {
    kj::Weak<T> first;
    kj::Weak<T> second;
    Double(kj::Weak<T> first, kj::Weak<T> second): first(kj::mv(first)), second(kj::mv(second)) {}
  };

  using Storage = kj::OneOf<None, Single, Double, kj::Vector<kj::Weak<T>>>;
  Storage storage = None();

  const kj::Weak<T>& getWeakAt(size_t index) const {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        KJ_FAIL_REQUIRE("Dereferencing end iterator");
      }
      KJ_CASE_ONEOF(single, Single) {
        KJ_REQUIRE(index == 0, "Invalid iterator");
        return single.item;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        KJ_REQUIRE(index < 2, "Invalid iterator");
        return index == 0 ? dbl.first : dbl.second;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<kj::Weak<T>>) {
        KJ_REQUIRE(index < vec.size(), "Invalid iterator");
        return vec[index];
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Weak<T>& getWeakAt(size_t index) {
    return const_cast<kj::Weak<T>&>(const_cast<const SmallWeakVector&>(*this).getWeakAt(index));
  }

  size_t firstLiveIndexFrom(size_t index) const {
    auto end = size();
    while (index < end && getWeakAt(index) == nullptr) {
      ++index;
    }
    return index;
  }

  struct EndSentinel {};

 public:
  // Iterator support - skips expired weak refs and returns references to live targets.
  class ConstIterator {
   public:
    ConstIterator() = default;

    const T& operator*() const {
      return owner->getWeakAt(index).assertLive();
    }

    ConstIterator& operator++() {
      index = owner->firstLiveIndexFrom(index + 1);
      return *this;
    }

    ConstIterator operator++(int) {
      ConstIterator tmp = *this;
      index = owner->firstLiveIndexFrom(index + 1);
      return tmp;
    }

    bool operator==(const ConstIterator& other) const {
      return owner == other.owner && index == other.index;
    }

    bool operator!=(const ConstIterator& other) const {
      return !(*this == other);
    }

   private:
    friend class SmallWeakVector;

    ConstIterator(const SmallWeakVector* owner, size_t index)
        : owner(owner),
          index(owner->firstLiveIndexFrom(index)) {}

    ConstIterator(const SmallWeakVector* owner, size_t index, EndSentinel)
        : owner(owner),
          index(index) {}

    const SmallWeakVector* owner = nullptr;
    size_t index = 0;
  };

  // Mutable iterator - skips expired weak refs and returns references to live targets.
  class Iterator {
   public:
    Iterator() = default;

    T& operator*() const {
      return owner->getWeakAt(index).assertLive();
    }

    Iterator& operator++() {
      index = owner->firstLiveIndexFrom(index + 1);
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      index = owner->firstLiveIndexFrom(index + 1);
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return owner == other.owner && index == other.index;
    }

    bool operator!=(const Iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class SmallWeakVector;

    Iterator(SmallWeakVector* owner, size_t index)
        : owner(owner),
          index(owner->firstLiveIndexFrom(index)) {}

    Iterator(SmallWeakVector* owner, size_t index, EndSentinel): owner(owner), index(index) {}

    SmallWeakVector* owner = nullptr;
    size_t index = 0;
  };

  Iterator begin() {
    return Iterator(this, 0);
  }

  Iterator end() {
    return Iterator(this, size(), EndSentinel());
  }

  ConstIterator begin() const {
    return ConstIterator(this, 0);
  }

  ConstIterator end() const {
    return ConstIterator(this, size(), EndSentinel());
  }
};

}  // namespace workerd
