// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>
#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/vector.h>

#include <concepts>

namespace workerd {

// Concept for types that support reference counting via addRef().
// This matches kj::Rc<T> which has an addRef() method returning kj::Rc<T>.
template <typename T>
concept RefCountedSmartPtr = requires(T& t) {
  { t.addRef() } -> std::convertible_to<T>;
};

// Concept for smart pointers to WeakRef-like types that have a tryGet() method.
// This is used to constrain forEach() which needs to check if the ref is still valid.
template <typename T>
concept WeakRefSmartPtr = RefCountedSmartPtr<T> && requires(T& t) {
  { t->tryGet() };
};

// A set-like container optimized for the common case of storing 0-2 items
// of reference-counted smart pointer types (like kj::Rc<T>).
//
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
// - If iterating over items that may be removed during iteration, use releaseSnapshot()
//   to get owned copies that remain valid even if the original is removed from the set.
//
// Template parameter T must be a reference-counted smart pointer type like kj::Rc<X>
// that has an addRef() method returning the same type.
template <RefCountedSmartPtr T>
class SmallSet {
 public:
  SmallSet() = default;
  KJ_DISALLOW_COPY(SmallSet);
  SmallSet(SmallSet&&) = default;
  SmallSet& operator=(SmallSet&&) = default;

  // Add an item to the set. The item is moved into the set.
  // For move-only types, use containsIf() first to check for duplicates if needed.
  void add(T item) {
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
        auto vec = kj::Vector<T>(4);
        vec.add(kj::mv(dbl.first));
        vec.add(kj::mv(dbl.second));
        vec.add(kj::mv(item));
        storage = kj::mv(vec);
        return;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        vec.add(kj::mv(item));
        return;
      }
    }
    KJ_UNREACHABLE;
  }

  // Remove an item matching the predicate. Returns true if an item was removed.
  // The predicate receives a const reference to each item.
  template <typename Predicate>
  bool removeIf(Predicate&& predicate) {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return false;
      }
      KJ_CASE_ONEOF(single, Single) {
        if (predicate(single.item)) {
          storage = None();
          return true;
        }
        return false;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        if (predicate(dbl.first)) {
          storage = Single(kj::mv(dbl.second));
          return true;
        }
        if (predicate(dbl.second)) {
          storage = Single(kj::mv(dbl.first));
          return true;
        }
        return false;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        // Find and remove the first matching item
        for (size_t i = 0; i < vec.size(); ++i) {
          if (predicate(vec[i])) {
            // Remove by overwriting with last element and truncating
            if (i < vec.size() - 1) {
              vec[i] = kj::mv(vec.back());
            }
            vec.removeLast();

            // Transition back to smaller state if appropriate
            if (vec.size() == 2) {
              storage = Double(kj::mv(vec[0]), kj::mv(vec[1]));
            } else if (vec.size() == 1) {
              storage = Single(kj::mv(vec[0]));
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

  // Check if the set contains an item matching the predicate.
  template <typename Predicate>
  bool containsIf(Predicate&& predicate) const {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return false;
      }
      KJ_CASE_ONEOF(single, Single) {
        return predicate(single.item);
      }
      KJ_CASE_ONEOF(dbl, Double) {
        return predicate(dbl.first) || predicate(dbl.second);
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        for (auto& existing: vec) {
          if (predicate(existing)) return true;
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

  // Iterate over all valid (non-invalidated) WeakRef items, calling func for each.
  // This is safe to use even if func modifies the set (e.g., removes items).
  //
  // Only available when T is a smart pointer to a WeakRef-like type with tryGet() method
  // (e.g., kj::Rc<WeakRef<X>>). The callback receives a reference to the
  // underlying type (X&).
  //
  // Example:
  //   SmallSet<kj::Rc<WeakRef<Consumer>>> consumers;
  //   consumers.forEach([&](Consumer& c) {
  //     c.close(js);  // Safe even if this removes other consumers
  //   });
  template <typename F>
  void forEach(F&& func)
    requires WeakRefSmartPtr<T>
  {
    KJ_SWITCH_ONEOF(storage) {
      KJ_CASE_ONEOF(none, None) {
        return;
      }
      KJ_CASE_ONEOF(single, Single) {
        KJ_IF_SOME(ref, single.item->tryGet()) {
          func(ref);
        }
        return;
      }
      KJ_CASE_ONEOF(dbl, Double) {
        // The storage state may change during iteration if func modifies the set,
        // so we take snapshots of the items first. Snapshotting just requires calling
        // addRef to increment the ref counts.
        kj::Array<T> refs = kj::arr(dbl.first.addRef(), dbl.second.addRef());
        for (auto& item: refs) {
          // We check tryGet on each item in case func invalidated some of them
          // in prior iterations.
          KJ_IF_SOME(ref, item->tryGet()) {
            func(ref);
          }
        }
        return;
      }
      KJ_CASE_ONEOF(vec, kj::Vector<T>) {
        // The storage state may change during iteration if func modifies the set,
        // so we take snapshots of the items first. Snapshotting just requires calling
        // addRef to increment the ref counts.
        auto snapshot = KJ_MAP(item, vec) { return item.addRef(); };
        for (auto& item: snapshot) {
          // We check tryGet on each item in case func invalidated some of them
          // in prior iterations.
          KJ_IF_SOME(ref, item->tryGet()) {
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
    T item;
    explicit Single(T item): item(kj::mv(item)) {}
  };

  struct Double {
    T first;
    T second;
    Double(T first, T second): first(kj::mv(first)), second(kj::mv(second)) {}
  };

  using Storage = kj::OneOf<None, Single, Double, kj::Vector<T>>;
  Storage storage = None();

 public:
  // Iterator support - returns const references to items
  class ConstIterator {
   public:
    ConstIterator() = default;

    const T& operator*() const {
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

    ConstIterator& operator++() {
      ++index;
      return *this;
    }

    ConstIterator operator++(int) {
      ConstIterator tmp = *this;
      ++index;
      return tmp;
    }

    bool operator==(const ConstIterator& other) const {
      return storage == other.storage && index == other.index;
    }

    bool operator!=(const ConstIterator& other) const {
      return !(*this == other);
    }

   private:
    friend class SmallSet;

    ConstIterator(const Storage* storage, size_t index): storage(storage), index(index) {}

    const Storage* storage = nullptr;
    size_t index = 0;
  };

  // Mutable iterator - returns mutable references to items
  class Iterator {
   public:
    Iterator() = default;

    T& operator*() const {
      if (storage->template is<None>()) {
        KJ_FAIL_REQUIRE("Dereferencing end iterator");
      } else if (storage->template is<Single>()) {
        KJ_REQUIRE(index == 0, "Invalid iterator");
        return storage->template get<Single>().item;
      } else if (storage->template is<Double>()) {
        KJ_REQUIRE(index < 2, "Invalid iterator");
        auto& dbl = storage->template get<Double>();
        return index == 0 ? dbl.first : dbl.second;
      } else {
        auto& vec = storage->template get<kj::Vector<T>>();
        KJ_REQUIRE(index < vec.size(), "Invalid iterator");
        return vec[index];
      }
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

    Iterator(Storage* storage, size_t index): storage(storage), index(index) {}

    Storage* storage = nullptr;
    size_t index = 0;
  };

  Iterator begin() {
    return Iterator(&storage, 0);
  }

  Iterator end() {
    return Iterator(&storage, size());
  }

  ConstIterator begin() const {
    return ConstIterator(&storage, 0);
  }

  ConstIterator end() const {
    return ConstIterator(&storage, size());
  }
};

}  // namespace workerd
