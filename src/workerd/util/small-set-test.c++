// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "small-set.h"
#include "weak-refs.h"

#include <kj/refcount.h>
#include <kj/test.h>

namespace workerd {
namespace {

// A simple refcounted type for testing SmallSet.
// Supports WeakRef creation for testing forEach.
struct TestItem final: public kj::Refcounted {
  int value;
  kj::Rc<WeakRef<TestItem>> selfRef;

  explicit TestItem(int v)
      : value(v),
        selfRef(kj::rc<WeakRef<TestItem>>(kj::Badge<TestItem>{}, *this)) {}

  ~TestItem() noexcept(false) {
    selfRef->invalidate();
  }

  kj::Rc<WeakRef<TestItem>> getWeakRef() {
    return selfRef.addRef();
  }
};

// Helper to check if set contains an item with a given value.
bool containsValue(SmallSet<kj::Rc<TestItem>>& set, int value) {
  return set.containsIf([value](const kj::Rc<TestItem>& item) { return item->value == value; });
}

// Helper to remove an item with a given value.
bool removeValue(SmallSet<kj::Rc<TestItem>>& set, int value) {
  return set.removeIf([value](const kj::Rc<TestItem>& item) { return item->value == value; });
}

// Helper for WeakRef-based tests (used with forEach).
bool removeWeakRefValue(SmallSet<kj::Rc<WeakRef<TestItem>>>& set, int value) {
  return set.removeIf([value](const kj::Rc<WeakRef<TestItem>>& item) {
    KJ_IF_SOME(ref, item->tryGet()) {
      return ref.value == value;
    }
    return false;
  });
}

KJ_TEST("SmallSet: empty set") {
  SmallSet<kj::Rc<TestItem>> set;
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 42));
}

KJ_TEST("SmallSet: add and remove single item") {
  SmallSet<kj::Rc<TestItem>> set;

  set.add(kj::rc<TestItem>(1));
  KJ_EXPECT(!set.empty());
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(containsValue(set, 1));

  KJ_EXPECT(removeValue(set, 1));
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 1));

  // Removing again should return false
  KJ_EXPECT(!removeValue(set, 1));
}

KJ_TEST("SmallSet: add and remove two items") {
  SmallSet<kj::Rc<TestItem>> set;

  set.add(kj::rc<TestItem>(1));
  set.add(kj::rc<TestItem>(2));
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));

  KJ_EXPECT(removeValue(set, 1));
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(!containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));

  KJ_EXPECT(removeValue(set, 2));
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallSet: add and remove multiple items") {
  SmallSet<kj::Rc<TestItem>> set;

  set.add(kj::rc<TestItem>(1));
  set.add(kj::rc<TestItem>(2));
  set.add(kj::rc<TestItem>(3));
  set.add(kj::rc<TestItem>(4));
  KJ_EXPECT(set.size() == 4);

  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));
  KJ_EXPECT(containsValue(set, 3));
  KJ_EXPECT(containsValue(set, 4));

  KJ_EXPECT(removeValue(set, 2));
  KJ_EXPECT(set.size() == 3);
  KJ_EXPECT(!containsValue(set, 2));

  KJ_EXPECT(removeValue(set, 3));
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 4));

  KJ_EXPECT(removeValue(set, 1));
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(containsValue(set, 4));

  KJ_EXPECT(removeValue(set, 4));
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallSet: state transitions") {
  SmallSet<kj::Rc<TestItem>> set;

  // None -> Single
  set.add(kj::rc<TestItem>(1));
  KJ_EXPECT(set.size() == 1);

  // Single -> Double
  set.add(kj::rc<TestItem>(2));
  KJ_EXPECT(set.size() == 2);

  // Double -> Multiple
  set.add(kj::rc<TestItem>(3));
  KJ_EXPECT(set.size() == 3);

  // Multiple stays Multiple
  set.add(kj::rc<TestItem>(4));
  KJ_EXPECT(set.size() == 4);

  // Multiple -> Multiple (one less)
  KJ_EXPECT(removeValue(set, 4));
  KJ_EXPECT(set.size() == 3);

  // Multiple -> Double
  KJ_EXPECT(removeValue(set, 3));
  KJ_EXPECT(set.size() == 2);

  // Double -> Single
  KJ_EXPECT(removeValue(set, 2));
  KJ_EXPECT(set.size() == 1);

  // Single -> None
  KJ_EXPECT(removeValue(set, 1));
  KJ_EXPECT(set.size() == 0);
}

KJ_TEST("SmallSet: iteration") {
  SmallSet<kj::Rc<TestItem>> set;

  // Empty iteration
  int count = 0;
  for (auto& item: set) {
    (void)item;
    count++;
  }
  KJ_EXPECT(count == 0);

  // Single item
  set.add(kj::rc<TestItem>(1));
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item->value == 1);
    count++;
  }
  KJ_EXPECT(count == 1);

  // Two items
  set.add(kj::rc<TestItem>(2));
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item->value == 1 || item->value == 2);
    count++;
  }
  KJ_EXPECT(count == 2);

  // Multiple items
  set.add(kj::rc<TestItem>(3));
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item->value >= 1 && item->value <= 3);
    count++;
  }
  KJ_EXPECT(count == 3);
}

KJ_TEST("SmallSet: clear") {
  SmallSet<kj::Rc<TestItem>> set;

  set.add(kj::rc<TestItem>(1));
  set.add(kj::rc<TestItem>(2));
  set.add(kj::rc<TestItem>(3));
  KJ_EXPECT(set.size() == 3);

  set.clear();
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 1));
  KJ_EXPECT(!containsValue(set, 2));
  KJ_EXPECT(!containsValue(set, 3));
}

KJ_TEST("SmallSet: forEach for safe iteration during modification") {
  // This simulates the queue.h use case where items may be removed during iteration.
  // forEach requires WeakRef items to handle invalidation during iteration.
  SmallSet<kj::Rc<WeakRef<TestItem>>> set;

  auto item1 = kj::rc<TestItem>(1);
  auto item2 = kj::rc<TestItem>(2);
  auto item3 = kj::rc<TestItem>(3);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());
  KJ_EXPECT(set.size() == 3);

  // forEach takes a snapshot internally, so items remain valid even if removed during iteration.
  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& item) {
    foundValues.add(item.value);
    // Remove the item from the set during iteration - this is safe because
    // forEach iterates over an internal snapshot, not the set itself.
    removeWeakRefValue(set, item.value);
  });

  KJ_EXPECT(foundValues.size() == 3);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallSet: forEach from single state") {
  SmallSet<kj::Rc<WeakRef<TestItem>>> set;

  auto item = kj::rc<TestItem>(42);
  set.add(item->getWeakRef());

  size_t count = 0;
  set.forEach([&](TestItem& ref) {
    KJ_EXPECT(ref.value == 42);
    count++;
  });
  KJ_EXPECT(count == 1);
}

KJ_TEST("SmallSet: forEach from double state") {
  SmallSet<kj::Rc<WeakRef<TestItem>>> set;

  auto item1 = kj::rc<TestItem>(1);
  auto item2 = kj::rc<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());

  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& ref) { foundValues.add(ref.value); });
  KJ_EXPECT(foundValues.size() == 2);
  // Order doesn't matter for set semantics, just check both values are present
  bool found1 = false, found2 = false;
  for (auto v: foundValues) {
    if (v == 1) found1 = true;
    if (v == 2) found2 = true;
  }
  KJ_EXPECT(found1);
  KJ_EXPECT(found2);
}

KJ_TEST("SmallSet: forEach from empty state") {
  SmallSet<kj::Rc<WeakRef<TestItem>>> set;
  size_t count = 0;
  set.forEach([&](TestItem& ref) { count++; });
  KJ_EXPECT(count == 0);
}

KJ_TEST("SmallSet: forEach skips invalidated WeakRefs") {
  // Test that forEach properly skips items whose WeakRefs have been invalidated
  SmallSet<kj::Rc<WeakRef<TestItem>>> set;

  auto item1 = kj::rc<TestItem>(1);
  auto item2 = kj::rc<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  KJ_EXPECT(set.size() == 2);

  // Invalidate item1's weak ref by destroying item1
  item1 = nullptr;

  // forEach should only visit item2
  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& ref) { foundValues.add(ref.value); });

  KJ_EXPECT(foundValues.size() == 1);
  KJ_EXPECT(foundValues[0] == 2);
}

KJ_TEST("SmallSet: reference counting works correctly") {
  // Verify that items are properly reference counted
  kj::Rc<TestItem> item = kj::rc<TestItem>(42);
  KJ_EXPECT(!item->isShared());  // Only one reference

  {
    SmallSet<kj::Rc<TestItem>> set;
    set.add(item.addRef());
    KJ_EXPECT(item->isShared());  // Now shared between item and set
  }
  // Set destroyed, only our original reference remains
  KJ_EXPECT(!item->isShared());
}

}  // namespace
}  // namespace workerd
