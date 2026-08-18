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
template <typename G, typename O>
class LinkedObject {
 public:
  LinkedObject() = default;
  ~LinkedObject() noexcept(false) {
    invalidateGroup();
  }
  KJ_DISALLOW_COPY_AND_MOVE(LinkedObject);

 private:
  friend class LinkedGroup<G, O>;
  using LinkedGroup = LinkedGroup<G, O>;

 protected:
  class LinkedGroupProxy {
   public:
    LinkedGroupProxy(LinkedObject& self): self(self) {}
    void set(LinkedGroup& newGroup) {
      self.setGroup(newGroup);
    }
    void set(kj::None) {
      self.invalidateGroup();
    }
    kj::Maybe<G&> tryGet() {
      return self.tryGetGroup();
    }

   private:
    LinkedObject& self;
  };

  class ConstLinkedGroupProxy {
   public:
    ConstLinkedGroupProxy(const LinkedObject& self): self(self) {}
    kj::Maybe<const G&> tryGet() const {
      return self.tryGetGroup();
    }

   private:
    const LinkedObject& self;
  };

  LinkedGroupProxy linkedGroup() {
    return *this;
  }
  ConstLinkedGroupProxy linkedGroup() const {
    return *this;
  }

 private:
  void setGroup(LinkedGroup& newGroup) {
    KJ_IF_SOME(oldGroup, maybeGroup) {
      if (&newGroup == &oldGroup) {
        return;
      } else {
        removeFromGroup(oldGroup);
      }
    } else {
      KJ_IREQUIRE(!link.isLinked());
    }

    newGroup.list.add(*this);
    maybeGroup = newGroup;
  }

  kj::Maybe<G&> tryGetGroup() {
    KJ_IF_SOME(group, maybeGroup) {
      KJ_IREQUIRE(link.isLinked());
      return static_cast<G&>(group);
    } else {
      KJ_IREQUIRE(!link.isLinked());
      return kj::none;
    }
  }

  kj::Maybe<const G&> tryGetGroup() const {
    KJ_IF_SOME(group, maybeGroup) {
      KJ_IREQUIRE(link.isLinked());
      return static_cast<const G&>(group);
    } else {
      KJ_IREQUIRE(!link.isLinked());
      return kj::none;
    }
  }

  void invalidateGroup() {
    KJ_IF_SOME(group, maybeGroup) {
      removeFromGroup(group);
    } else {
      KJ_IREQUIRE(!link.isLinked());
    }
  }

  void removeFromGroup(LinkedGroup& group) {
    KJ_IREQUIRE(link.isLinked());
    group.list.remove(*this);
    maybeGroup = kj::none;
  }

  kj::ListLink<LinkedObject> link;
  kj::Maybe<LinkedGroup&> maybeGroup;
};

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
