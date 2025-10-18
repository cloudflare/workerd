// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/vector.h>

namespace workerd {

// A set-like container optimized for the common case of storing 0-2 items.
// This uses a kj::OneOf to avoid heap allocations for small sets.
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
// This is NOT a drop-in replacement for std::set because:
// - Items are not kept in sorted order
// - No logarithmic lookup guarantees
// - Optimized for small sizes only
//
// Iterator invalidation:
// - Iterators are invalidated when items are removed or storage state changes
// - If iterating over items that may remove themselves, use snapshot():
//     auto snapshot = set.snapshot();
//     for (auto item : snapshot) {
//       item->doSomethingThatMightRemoveItself();
//     }
//
// Template parameter T should be a pointer type or trivially copyable type.
// In case it's not obvious, I just had Claude write this and the test for
// simplicity.
template <typename T>
class SmallSet {
 public:
  SmallSet() = default;
  KJ_DISALLOW_COPY(SmallSet);
  SmallSet(SmallSet&&) = default;
  SmallSet& operator=(SmallSet&&) = default;

  // Add an item to the set. Returns true if the item was added, false if it already existed.
  bool add(T item) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        storage = Single(item);
        return true;
      }
      KJ_CASE_ONEOF(single, Single) {
        if (single.item == item) return false;
        storage = Double(single.item, item);
        return true;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        if (dbl.first == item || dbl.second == item) return false;
        auto vec = kj::Vector<T>(4);
        vec.add(dbl.first);
        vec.add(dbl.second);
        vec.add(item);
        storage = kj::mv(vec);
        return true;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        // Linear search for the item
        for (auto& existing: vec) {
          if (existing == item) return false;
        }
        vec.add(item);
        return true;
      }
    }
    KJ_UNREACHABLE;
  }

  // Remove an item from the set. Returns true if the item was removed, false if not found.
  bool remove(T item) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return false;
      }
      KJ_CASE_ONEOF(single, Single) {
        if (single.item == item) {
          storage = None();
          return true;
        }
        return false;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        if (dbl.first == item) {
          storage = Single(dbl.second);
          return true;
        }
        if (dbl.second == item) {
          storage = Single(dbl.first);
          return true;
        }
        return false;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        // Find and remove the item
        for (size_t i = 0; i < vec.size(); ++i) {
          if (vec[i] == item) {
            // Remove by swapping with last element and truncating
            if (i < vec.size() - 1) {
              vec[i] = vec.back();
            }
            vec.removeLast();

            // Transition back to smaller state if appropriate
            if (vec.size() == 2) {
              storage = Double(vec[0], vec[1]);
            } else if (vec.size() == 1) {
              storage = Single(vec[0]);
            } else if (vec.size() == 0) {
              storage = None();
            }
            // else: vec.size() >= 3, stay in Vector state

            return true;
          }
        }
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  // Check if the set contains an item.
  bool contains(T item) const {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return false;
      }
      KJ_CASE_ONEOF(single, Single) {
        return single.item == item;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        return dbl.first == item || dbl.second == item;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        for (auto& existing: vec) {
          if (existing == item) return true;
        }
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  // Get the number of items in the set.
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
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        return vec.size();
      }
    }
    KJ_UNREACHABLE;
  }

  // Check if the set is empty.
  bool empty() const {
    return size() == 0;
  }

  // Clear all items from the set.
  void clear() {
    storage = None();
  }

  // Create a snapshot of all items as a vector.
  // Use this when iterating over items that may remove themselves from the set during iteration.
  // This is needed because the iterator is invalidated when the storage changes state
  // (e.g., Double -> Single) or when items are removed from the vector.
  //
  // Example usage:
  //   auto snapshot = set.snapshot();
  //   for (auto item : snapshot) {
  //     item->doSomethingThatMightRemoveItself();
  //   }
  kj::Vector<T> snapshot() const {
    auto n = size();
    kj::Vector<T> result(n);  // Pre-allocate exact capacity to avoid reallocation
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        // Empty, return empty vector
      }
      KJ_CASE_ONEOF(single, Single) {
        result.add(single.item);
      }
      KJ_CASE_ONEOF(dbl, Double) {
        result.add(dbl.first);
        result.add(dbl.second);
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        for (auto item: vec) {
          result.add(item);
        }
      }
    }
    return result;
  }

 private:
  struct None {};

  struct Single {
    T item;
    explicit Single(T item): item(item) {}
  };

  struct Double {
    T first;
    T second;
    Double(T first, T second): first(first), second(second) {}
  };

  using Storage = kj::OneOf<None, Single, Double, kj::Vector<T>>;
  Storage storage = None();

 public:
  // Iterator support
  class Iterator {
   public:
    Iterator() = default;

    T operator*() const {
      KJ_SWITCH_ONEOF(*storage) {
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
        KJ_CASE_ONEOF(vec, kj::Vector<T>) {
          KJ_REQUIRE(index < vec.size(), "Invalid iterator");
          return vec[index];
        }
      }
      KJ_UNREACHABLE;
    }

    Iterator& operator++() {
      ++index;
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++index;
      return tmp;
    }

    bool operator==(const Iterator& other) const {
      return storage == other.storage && index == other.index;
    }

    bool operator!=(const Iterator& other) const {
      return !(*this == other);
    }

   private:
    friend class SmallSet;

    Iterator(const Storage* storage, size_t index): storage(storage), index(index) {}

    const Storage* storage = nullptr;
    size_t index = 0;
  };

  Iterator begin() const {
    return Iterator(&storage, 0);
  }

  Iterator end() const {
    return Iterator(&storage, size());
  }
};

}  // namespace workerd
