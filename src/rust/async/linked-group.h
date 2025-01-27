#pragma once

#include <kj/list.h>

namespace workerd::rust::async {

// `LinkedGroup<G, O>` and `LinkedObject<G, O>` are CRTP mixins which allow derived classes G and O
// to weakly refer to each other in a one-to-many relationship.
//
// For example, say you have two classes, Group and Object. There exists a natural one-to-many
// relationship between the two. Given a Group, you would like to be able to dererefence its
// Objects, and, given an Object, you would like be able to dereference its Group. Further suppose
// the objects have independent lifetimes: Objects may be destroyed before their Groups, and Groups
// may be destroyed before their Objects.
//
// If you are operating in a single-threaded context (or can provide sufficient synchronization),
// and if Group and Object are both immobile (non-copyable, non-moveable) classes, then
// `LinkedGroup<Group, Object>` and `LinkedObject<Group, Object>` can be used to implement the above
// scenario safely. To do so, first:
//
//  - Your Group class must publicly inherit from `LinkedGroup<Group, Object>`.
//  - Your Object class must publicly inherit from `LinkedObject<Group, Object>`.
//
// This will add one protected member function to each of your derived classes:
// `Object::linkedGroup()`, and `Group::linkedObjects()`. They are protected so that they are not
// part of your type's public API unless you explicitly want them to be, e.g., with a public `using`
// statement like `using LinkedGroup::linkedObjects`.
//
// You can use `Object::linkedGroup()` to manage Group membership and dereference Groups from
// Objects:
//
//  - `object.linkedGroup().set(group)` adds an Object to a Group.
//    This function implicitly removes the Object from its current Group, if any.
//  - `object.linkedGroup().set(kj::none)` removes an Object from its current Group, if any.
//  - `object.linkedGroup().tryGet()` dereferences the Object's current Group, if any.
//
// You can use `Group::linkedObjects()` to iterate over the list of currently linked Objects.
//
//  - `group.linkedObjects().begin()` obtains an iterator to the beginning of the list of Objects.
//  - `group.linkedObjects().end()` obtains an iterator to the end of the list of Objets.
//  - `group.linkedObjects().front()` dereferences the front of the list of Objects.
//    Calling `front()` on an empty list (`begin() == end()`) is undefined behavior.
//  - `group.linkedObjects().empty()` is true if there are no Objects in the list.
//
// Finally, destroying either the Group or its Object safely severs their relationship(s).
//
//  - Destroying an Object implicitly calls `object.linkedGroup().set(kj::none)` on itself.
//  - Destroying a Group implicitly calls `object.linkedGroup().set(kj::none)` on all its objects.
//
// Considerations:
//
//   - Your Group object's destructor will contain a _O(n)_ algorithm inside it, with _n_ being the
//     number of linked objects at destruction time. If Groups frequently outlive large sets of
//     Objects, this may be an issue to consider.
//   - It is valid to remove the front Object in a `Group::linkedObjects()` list while iterating
//     over the list. Removing an Object in any other position in the list will invalidate all
//     existing iterators.
//
// TODO(now): Tests. Multiple inheritance if an object must join multiple groups, or a group must
//   have multiple linked object types? Can we write something like `linkedGroup<G>()` in the
//   LinkedObject derived class, and `linkedObjects<O>()` in the LinkedGro8up derived class?
//   - Test: Order in which LinkedObjects are added.
//   - Test: Redundant set() does not change position of LinkedObject in list.
//   - Test: Lifetimes, of course.
//   - Test: Iteration and removal.
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
    Iterator begin() { return list.begin(); }
    Iterator end() { return list.end(); }
    decltype(*kj::instance<Iterator>()) front() { return *begin(); }
    bool empty() const { return list.empty(); }
  private:
    List& list;
  };

  class ConstLinkedObjectList {
  public:
    ConstLinkedObjectList(const List& list): list(list) {}
    ConstIterator begin() const { return list.begin(); }
    ConstIterator end() const { return list.end(); }
    decltype(*kj::instance<ConstIterator>()) front() const { return *begin(); }
    bool empty() const { return list.empty(); }
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
    void set(LinkedGroup& newGroup) { self.setGroup(newGroup); }
    void set(kj::None) { self.invalidateGroup(); }
    kj::Maybe<G&> tryGet() { return self.tryGetGroup(); }
  private:
    LinkedObject& self;
  };

  // Const version of LinkedGroupProxy, exposing only `tryGet()`.
  class ConstLinkedGroupProxy {
  public:
    ConstLinkedGroupProxy(const LinkedObject& self): self(self) {}
    kj::Maybe<const G&> tryGet() const { return self.tryGetGroup(); }
  private:
    const LinkedObject& self;
  };

  // Provide access to this Object's LinkedGroup, if any.
  LinkedGroupProxy linkedGroup() { return *this; }
  ConstLinkedGroupProxy linkedGroup() const { return *this; }

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

}  // namespace workerd::rust::async
