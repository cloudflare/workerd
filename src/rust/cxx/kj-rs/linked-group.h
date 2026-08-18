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
template <typename G, typename O>
class LinkedGroup {
 public:
  LinkedGroup() = default;
  ~LinkedGroup() noexcept(false) {
    for (auto& object: list) {
      object.removeFromGroup(*this);
    }
  }
  KJ_DISALLOW_COPY_AND_MOVE(LinkedGroup);

 private:
  // We'll refer to the `LinkedObject<G, O>` type quite a bit below, so we shadow the class
  // template with our own convenience typedef. But, we need to give LinkedObject friend access to
  // us first.
  friend class LinkedObject<G, O>;
  using LinkedObject = LinkedObject<G, O>;

  using List = kj::List<LinkedObject, &LinkedObject::link>;

  using ListIterator = kj::ListIterator<LinkedObject, LinkedObject, &LinkedObject::link>;
  using ConstListIterator = kj::ListIterator<LinkedObject, const LinkedObject, &LinkedObject::link>;

  using Iterator = StaticCastIterator<O, O, ListIterator>;
  using ConstIterator = StaticCastIterator<O, const O, ConstListIterator>;

 protected:
  // A proxy class representing this LinkedGroup's list of LinkedObjects, if any. Instead of
  // exposing multiple functions on LinkedGroup, we expose one: `linkedObjects()`, and that function
  // returns an object of this proxy class (or the similar ConstLinkedObjectList class below).
  class LinkedObjectList {
   public:
    LinkedObjectList(List& list): list(list) {}
    Iterator begin() {
      return list.begin();
    }
    Iterator end() {
      return list.end();
    }
    decltype(*kj::instance<Iterator>()) front() {
      return *begin();
    }
    bool empty() const {
      return list.empty();
    }

   private:
    List& list;
  };

  class ConstLinkedObjectList {
   public:
    ConstLinkedObjectList(const List& list): list(list) {}
    ConstIterator begin() const {
      return list.begin();
    }
    ConstIterator end() const {
      return list.end();
    }
    decltype(*kj::instance<ConstIterator>()) front() const {
      return *begin();
    }
    bool empty() const {
      return list.empty();
    }

   private:
    const List& list;
  };

  LinkedObjectList linkedObjects() {
    return LinkedObjectList(list);
  }
  ConstLinkedObjectList linkedObjects() const {
    return ConstLinkedObjectList(list);
  }

 private:
  kj::List<LinkedObject, &LinkedObject::link> list;
};

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
  // We'll refer to the `LinkedGroup<G, O>` type quite a bit below, so we shadow the class template
  // with our own convenience typedef. But, we need to give LinkedGroup friend access to us first.
  friend class LinkedGroup<G, O>;
  using LinkedGroup = LinkedGroup<G, O>;

 protected:
  // A proxy class representing this LinkedObject's LinkedGroup, if any. Instead of exposing
  // multiple functions on LinkedObject, we expose one: `linkedGroup()`, and that function returns
  // an object of this proxy class (or the similar ConstLinkedGroupProxy class below).
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

  // Const version of LinkedGroupProxy, exposing only `tryGet()`.
  class ConstLinkedGroupProxy {
   public:
    ConstLinkedGroupProxy(const LinkedObject& self): self(self) {}
    kj::Maybe<const G&> tryGet() const {
      return self.tryGetGroup();
    }

   private:
    const LinkedObject& self;
  };

  // Provide access to this Object's LinkedGroup, if any.
  LinkedGroupProxy linkedGroup() {
    return *this;
  }
  ConstLinkedGroupProxy linkedGroup() const {
    return *this;
  }

 private:
  void setGroup(LinkedGroup& newGroup) {
    // Invalidate our current group membership, if any.
    KJ_IF_SOME(oldGroup, maybeGroup) {
      // If we're already a member of `newGroup`, we're done. Otherwise, we must remove ourselves
      // from the old group.
      if (&newGroup == &oldGroup) {
        return;
      } else {
        removeFromGroup(oldGroup);
      }
    } else {
      KJ_IREQUIRE(!link.isLinked());
    }

    // Add ourselves to the new group.
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

  // Helper for `setGroup()`, `invalidateGroup()`, and `~LinkedGroup()`.
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
