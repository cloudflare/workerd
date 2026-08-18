#pragma once

#include <kj/list.h>

namespace kj_rs {

template <typename G, typename O>
class LinkedGroup;
template <typename G, typename O>
class LinkedObject;

template <typename T, typename MaybeConstT, typename InnerIterator>
class StaticCastIterator;

// CRTP mixin for derived class G.

// CRTP mixin for derived class O.

// An iterator which wraps `InnerIterator` and `static_cast`s all mutable dereferences to
// `MaybeConstT&`, and all const dereferences to `const T&`.
//
// With the Ranges TS, all of this nonsense could be boiled down to a one-liner based on
// `std::views::transform()`. I encountered too many puzzles to solve while trying to get that
// working, so here we are.
template <typename T, typename MaybeConstT, typename InnerIterator>
class StaticCastIterator {
 public:
  // Construct an iterator using a default-constructed InnerIterator. In practice, this constructs
  // an end iterator.
  StaticCastIterator() = default;

  // Construct an iterator wrapping `inner`.
  StaticCastIterator(InnerIterator inner): inner(inner) {}

  MaybeConstT& operator*() {
    return static_cast<MaybeConstT&>(*inner);
  }
  const T& operator*() const {
    return static_cast<const T&>(*inner);
  }
  MaybeConstT* operator->() {
    return static_cast<MaybeConstT*>(inner.operator->());
  }
  const T* operator->() const {
    return static_cast<const T*>(inner.operator->());
  }

  inline StaticCastIterator& operator++() {
    ++inner;
    return *this;
  }
  inline StaticCastIterator operator++(int) {
    StaticCastIterator result = *this;
    ++inner;
    return result;
  }

  inline bool operator==(const StaticCastIterator& other) const {
    return inner == other.inner;
  }

 private:
  InnerIterator inner;
};

}  // namespace kj_rs
