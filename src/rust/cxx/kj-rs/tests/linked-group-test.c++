#include "kj-rs/linked-group.h"

#include <kj/test.h>

namespace kj_rs {
namespace {

// Minimal concrete types for testing.
class TestGroup;
class TestObject;

class TestGroup: public LinkedGroup<TestGroup, TestObject> {
 public:
  explicit TestGroup(int id): id(id) {}

  // Expose the protected member for testing.
  using LinkedGroup::linkedObjects;

  int id;
};

class TestObject: public LinkedObject<TestGroup, TestObject> {
 public:
  explicit TestObject(int id): id(id) {}

  // Expose the protected member for testing.
  using LinkedObject::linkedGroup;

  int id;
};

// ---------------------------------------------------------------------------
// Basic membership
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: object can join a group") {
  TestGroup group(1);
  TestObject object(10);

  KJ_EXPECT(group.linkedObjects().empty());
  KJ_EXPECT(object.linkedGroup().tryGet() == kj::none);

  object.linkedGroup().set(group);

  KJ_EXPECT(!group.linkedObjects().empty());
  KJ_EXPECT(group.linkedObjects().front().id == 10);
  KJ_IF_SOME(g, object.linkedGroup().tryGet()) {
    KJ_EXPECT(g.id == 1);
  } else {
    KJ_FAIL_EXPECT("expected group to be set");
  }
}

KJ_TEST("LinkedGroup: object can leave a group") {
  TestGroup group(1);
  TestObject object(10);

  object.linkedGroup().set(group);
  KJ_EXPECT(!group.linkedObjects().empty());

  object.linkedGroup().set(kj::none);
  KJ_EXPECT(group.linkedObjects().empty());
  KJ_EXPECT(object.linkedGroup().tryGet() == kj::none);
}

KJ_TEST("LinkedGroup: object can switch groups") {
  TestGroup group1(1);
  TestGroup group2(2);
  TestObject object(10);

  object.linkedGroup().set(group1);
  KJ_EXPECT(!group1.linkedObjects().empty());
  KJ_EXPECT(group2.linkedObjects().empty());

  object.linkedGroup().set(group2);
  KJ_EXPECT(group1.linkedObjects().empty());
  KJ_EXPECT(!group2.linkedObjects().empty());
  KJ_IF_SOME(g, object.linkedGroup().tryGet()) {
    KJ_EXPECT(g.id == 2);
  } else {
    KJ_FAIL_EXPECT("expected group to be set");
  }
}

// ---------------------------------------------------------------------------
// Insertion order
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: objects are iterable in insertion order") {
  TestGroup group(1);
  TestObject a(1), b(2), c(3);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);
  c.linkedGroup().set(group);

  // Verify iteration order matches insertion order.
  auto it = group.linkedObjects().begin();
  KJ_EXPECT(it->id == 1);
  ++it;
  KJ_EXPECT(it->id == 2);
  ++it;
  KJ_EXPECT(it->id == 3);
  ++it;
  KJ_EXPECT(it == group.linkedObjects().end());
}

// ---------------------------------------------------------------------------
// Redundant set() is a no-op
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: redundant set() does not change position") {
  TestGroup group(1);
  TestObject a(1), b(2), c(3);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);
  c.linkedGroup().set(group);

  // Re-set b to the same group — order should be unchanged.
  b.linkedGroup().set(group);

  auto it = group.linkedObjects().begin();
  KJ_EXPECT(it->id == 1);
  ++it;
  KJ_EXPECT(it->id == 2);
  ++it;
  KJ_EXPECT(it->id == 3);
  ++it;
  KJ_EXPECT(it == group.linkedObjects().end());
}

// ---------------------------------------------------------------------------
// Lifetimes: object destroyed before group
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: destroying an object removes it from the group") {
  TestGroup group(1);
  TestObject a(1);
  {
    TestObject b(2);
    b.linkedGroup().set(group);
    a.linkedGroup().set(group);

    // Both present.
    auto it = group.linkedObjects().begin();
    KJ_EXPECT(it->id == 2);
    ++it;
    KJ_EXPECT(it->id == 1);
    ++it;
    KJ_EXPECT(it == group.linkedObjects().end());
  }
  // b is destroyed; only a remains.
  auto it = group.linkedObjects().begin();
  KJ_EXPECT(it->id == 1);
  ++it;
  KJ_EXPECT(it == group.linkedObjects().end());
}

// ---------------------------------------------------------------------------
// Lifetimes: group destroyed before objects
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: destroying a group unlinks all objects") {
  TestObject a(1), b(2);
  {
    TestGroup group(1);
    a.linkedGroup().set(group);
    b.linkedGroup().set(group);
    KJ_EXPECT(a.linkedGroup().tryGet() != kj::none);
    KJ_EXPECT(b.linkedGroup().tryGet() != kj::none);
  }
  // Group destroyed — objects should no longer reference it.
  KJ_EXPECT(a.linkedGroup().tryGet() == kj::none);
  KJ_EXPECT(b.linkedGroup().tryGet() == kj::none);
}

// ---------------------------------------------------------------------------
// Iteration and removal of the front element
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: removing the front element during iteration is safe") {
  TestGroup group(1);
  TestObject a(1), b(2), c(3);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);
  c.linkedGroup().set(group);

  // The header documents that removing the *front* element during iteration is valid.
  kj::Vector<int> collected;
  for (auto it = group.linkedObjects().begin(); it != group.linkedObjects().end();) {
    auto& obj = *it;
    ++it;  // advance before removing
    collected.add(obj.id);
    obj.linkedGroup().set(kj::none);
  }

  KJ_EXPECT(collected.size() == 3);
  KJ_EXPECT(collected[0] == 1);
  KJ_EXPECT(collected[1] == 2);
  KJ_EXPECT(collected[2] == 3);
  KJ_EXPECT(group.linkedObjects().empty());
}

// ---------------------------------------------------------------------------
// Const access
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: const access to group's objects") {
  TestGroup group(1);
  TestObject a(1), b(2);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);

  const TestGroup& cgroup = group;
  KJ_EXPECT(!cgroup.linkedObjects().empty());
  KJ_EXPECT(cgroup.linkedObjects().front().id == 1);

  auto it = cgroup.linkedObjects().begin();
  KJ_EXPECT(it->id == 1);
  ++it;
  KJ_EXPECT(it->id == 2);
  ++it;
  KJ_EXPECT(it == cgroup.linkedObjects().end());
}

KJ_TEST("LinkedGroup: const access to object's group") {
  TestGroup group(1);
  TestObject object(10);

  object.linkedGroup().set(group);

  const TestObject& cobject = object;
  KJ_IF_SOME(g, cobject.linkedGroup().tryGet()) {
    KJ_EXPECT(g.id == 1);
  } else {
    KJ_FAIL_EXPECT("expected group to be set");
  }
}

// ---------------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: default-constructed objects have no group") {
  TestObject object(1);
  KJ_EXPECT(object.linkedGroup().tryGet() == kj::none);
}

KJ_TEST("LinkedGroup: default-constructed groups have no objects") {
  TestGroup group(1);
  KJ_EXPECT(group.linkedObjects().empty());
  KJ_EXPECT(group.linkedObjects().begin() == group.linkedObjects().end());
}

// ---------------------------------------------------------------------------
// Multiple objects, various removal patterns
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: removing a middle object leaves others intact") {
  TestGroup group(1);
  TestObject a(1), b(2), c(3);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);
  c.linkedGroup().set(group);

  b.linkedGroup().set(kj::none);

  auto it = group.linkedObjects().begin();
  KJ_EXPECT(it->id == 1);
  ++it;
  KJ_EXPECT(it->id == 3);
  ++it;
  KJ_EXPECT(it == group.linkedObjects().end());
}

KJ_TEST("LinkedGroup: removing all objects one by one") {
  TestGroup group(1);
  TestObject a(1), b(2), c(3);

  a.linkedGroup().set(group);
  b.linkedGroup().set(group);
  c.linkedGroup().set(group);

  a.linkedGroup().set(kj::none);
  KJ_EXPECT(!group.linkedObjects().empty());

  b.linkedGroup().set(kj::none);
  KJ_EXPECT(!group.linkedObjects().empty());

  c.linkedGroup().set(kj::none);
  KJ_EXPECT(group.linkedObjects().empty());
}

// ---------------------------------------------------------------------------
// set(kj::none) on an unlinked object is a no-op
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: set(none) on an unlinked object is safe") {
  TestObject object(1);
  // Should not crash or assert.
  object.linkedGroup().set(kj::none);
  KJ_EXPECT(object.linkedGroup().tryGet() == kj::none);
}

// ---------------------------------------------------------------------------
// Multiple groups are independent
// ---------------------------------------------------------------------------

KJ_TEST("LinkedGroup: multiple groups are independent") {
  TestGroup g1(1), g2(2);
  TestObject a(1), b(2), c(3), d(4);

  a.linkedGroup().set(g1);
  b.linkedGroup().set(g1);
  c.linkedGroup().set(g2);
  d.linkedGroup().set(g2);

  // Verify g1 has {a, b}.
  {
    auto it = g1.linkedObjects().begin();
    KJ_EXPECT(it->id == 1);
    ++it;
    KJ_EXPECT(it->id == 2);
    ++it;
    KJ_EXPECT(it == g1.linkedObjects().end());
  }

  // Verify g2 has {c, d}.
  {
    auto it = g2.linkedObjects().begin();
    KJ_EXPECT(it->id == 3);
    ++it;
    KJ_EXPECT(it->id == 4);
    ++it;
    KJ_EXPECT(it == g2.linkedObjects().end());
  }

  // Move b from g1 to g2.
  b.linkedGroup().set(g2);

  {
    auto it = g1.linkedObjects().begin();
    KJ_EXPECT(it->id == 1);
    ++it;
    KJ_EXPECT(it == g1.linkedObjects().end());
  }
  {
    auto it = g2.linkedObjects().begin();
    KJ_EXPECT(it->id == 3);
    ++it;
    KJ_EXPECT(it->id == 4);
    ++it;
    KJ_EXPECT(it->id == 2);
    ++it;
    KJ_EXPECT(it == g2.linkedObjects().end());
  }
}

}  // namespace
}  // namespace kj_rs
