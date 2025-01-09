#pragma once

#include <kj/list.h>

namespace workerd::rust::async {

// `LinkedGroup<G, O>` and `LinkedObject<G, O>` are CRTP mixins which allow derived classes G and E
// to weakly refer to each other in a one-to-many relationship.
//
// For example, say you have two classes, Group and Object. There exists a natural one-to-many
// relationship between the two. Given a Group, you would like to be able to dererefence its
// Objects, and, given an Object, you would like be able to dereference its Group. Further suppose
// the objects have independent lifetimes: Objects may be destroyed before their Groups, and Groups
// may be destroyed before their Objects.
//
// If you are operating in a single-threaded context, and if Group and Object are both immobile
// (non-copyable, non-moveable) classes, then `LinkedGroup<Group, Object>` and
// `LinkedObject<Group, Object>` can be used to implement the above scenario safely:
//
//  - Group publicly inherits `LinkedGroup<Group, Object>`.
//  - Object publicly inherits `LinkedObject<Group, Object>`.
//
//  - `object.linkedGroup.set(group)` adds an Object to a Group.
//    This function implicitly removes the Object from its current Group, if any.
//  - `object.linkedGroup.tryGet()` dereferences the Object's current Group, if any.
//  - `object.linkedGroup.invalidate()` removes an Object from its current Group, if any.
//
//  - `group.linkedObjects()` obtains an iterable range of the Group's current Objects.
//
//  - Destroying an Object implicitly calls `object.invalidate()` on itself.
//  - Destroying a Group implicitly calls `object.invalidate()` on all its objects.
//
// TODO(now): Tests.
template <typename G, typename O>
class LinkedGroup;
template <typename G, typename O>
class LinkedObject;

template <typename T, typename MaybeConstT, typename InnerIterator>
class StaticCastIterator;

// CRTP mixin to allow `LinkedObject<G, O>` objects to join your group.
template <typename G, typename O>
class LinkedGroup {
public:
  LinkedGroup() = default;
  ~LinkedGroup() noexcept(false) {
    for (auto& object: list) {
      object.linkedGroup().invalidate();
    }
  }
  KJ_DISALLOW_COPY_AND_MOVE(LinkedGroup);

private:
  friend class LinkedObject<G, O>;
  using LinkedObject = LinkedObject<G, O>;

  using List = kj::List<LinkedObject, &LinkedObject::link>;

  using ListIterator = kj::ListIterator<LinkedObject, LinkedObject, &LinkedObject::link>;
  using ConstListIterator = kj::ListIterator<LinkedObject, const LinkedObject, &LinkedObject::link>;

  using Iterator = StaticCastIterator<O, O, ListIterator>;
  using ConstIterator = StaticCastIterator<O, const O, ConstListIterator>;

protected:
  class LinkedObjectList {
  public:
    LinkedObjectList(List& list): list(list) {}
    Iterator begin() { return list.begin(); }
    Iterator end() { return list.end(); }
    decltype(*kj::instance<Iterator>()) front() { return *begin(); }
  private:
    List& list;
  };

  class ConstLinkedObjectList {
  public:
    ConstLinkedObjectList(const List& list): list(list) {}
    ConstIterator begin() const { return list.begin(); }
    ConstIterator end() const { return list.end(); }
    decltype(*kj::instance<ConstIterator>()) front() const { return *begin(); }
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

// A CRTP mixin to allow for your derived class to join a `LinkedGroup<G, O>`.
template <typename G, typename O>
class LinkedObject {
public:
  LinkedObject() = default;
  ~LinkedObject() noexcept(false) {
    invalidateGroup();
  }
  KJ_DISALLOW_COPY_AND_MOVE(LinkedObject);

protected:
  friend class LinkedGroup<G, O>;
  using LinkedGroup = LinkedGroup<G, O>;

  class Group {
  public:
    Group(LinkedObject& self): self(self) {}
    void set(LinkedGroup& newGroup) { self.setGroup(newGroup); }
    kj::Maybe<G&> tryGet() { return self.tryGetGroup(); }
    void invalidate() { self.invalidateGroup(); }
  private:
    LinkedObject& self;
  };

  class ConstGroup {
  public:
    ConstGroup(const LinkedObject& self): self(self) {}
    kj::Maybe<const G&> tryGet() const { return self.tryGetGroup(); }
  private:
    const LinkedObject& self;
  };

  Group linkedGroup() { return *this; }
  ConstGroup linkedGroup() const { return *this; }

private:
  void setGroup(LinkedGroup& newGroup) {
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

  void invalidateGroup() {
    KJ_IF_SOME(group, maybeGroup) {
      removeFromGroup(group);
    } else {
      KJ_IREQUIRE(!link.isLinked());
    }
  }

  // Helper for `setGroup()` and `invalidateGroup()`.
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
template <typename T, typename MaybeConstT, typename InnerIterator>
class StaticCastIterator {
public:
  StaticCastIterator() = default;
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

}  // namespace workerd::rust::async
