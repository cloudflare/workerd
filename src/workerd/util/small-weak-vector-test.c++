// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "small-weak-vector.h"

#include <kj/memory.h>
#include <kj/test.h>

namespace workerd {
namespace {

// A simple weak-reference target for testing SmallWeakVector.
struct TestItem final: public kj::PtrTarget {
  int value;

  explicit TestItem(int v): value(v) {}

  kj::Weak<TestItem> getWeakRef() {
    return addWeakToThis();
  }
};

// Helper to check if the vector contains an item with a given value.
bool containsValue(SmallWeakVector<TestItem>& vector, int value) {
  return vector.containsIf([value](TestItem& item) { return item.value == value; });
}

// Helper to remove an item with a given value.
void removeValue(SmallWeakVector<TestItem>& vector, int value) {
  vector.removeAll([value](TestItem& item) { return item.value == value; });
}

KJ_TEST("kj::Weak: KJ_IF_SOME on const weak returns mutable pointer") {
  auto item = kj::heap<TestItem>(1);
  const kj::Weak<TestItem> weak = item->getWeakRef();

  bool found = false;
  KJ_IF_SOME(ptr, weak) {
    ptr->value = 2;
    found = true;
  }

  KJ_EXPECT(found);
  KJ_EXPECT(item->value == 2);
}

KJ_TEST("SmallWeakVector: empty vector") {
  SmallWeakVector<TestItem> set;
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 42));
}

KJ_TEST("SmallWeakVector: add and remove single item") {
  SmallWeakVector<TestItem> set;

  auto item = kj::heap<TestItem>(1);
  set.add(item->getWeakRef());
  KJ_EXPECT(!set.empty());
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(containsValue(set, 1));

  removeValue(set, 1);
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 1));

  // Removing again should leave the vector empty.
  removeValue(set, 1);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallWeakVector: add and remove two items") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));

  removeValue(set, 1);
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(!containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));

  removeValue(set, 2);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallWeakVector: add and remove multiple items") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  auto item3 = kj::heap<TestItem>(3);
  auto item4 = kj::heap<TestItem>(4);
  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());
  set.add(item4->getWeakRef());
  KJ_EXPECT(set.size() == 4);

  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 2));
  KJ_EXPECT(containsValue(set, 3));
  KJ_EXPECT(containsValue(set, 4));

  removeValue(set, 2);
  KJ_EXPECT(set.size() == 3);
  KJ_EXPECT(!containsValue(set, 2));

  removeValue(set, 3);
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(containsValue(set, 1));
  KJ_EXPECT(containsValue(set, 4));

  removeValue(set, 1);
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(containsValue(set, 4));

  removeValue(set, 4);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallWeakVector: state transitions") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  auto item3 = kj::heap<TestItem>(3);
  auto item4 = kj::heap<TestItem>(4);

  // None -> Single
  set.add(item1->getWeakRef());
  KJ_EXPECT(set.size() == 1);

  // Single -> Double
  set.add(item2->getWeakRef());
  KJ_EXPECT(set.size() == 2);

  // Double -> Multiple
  set.add(item3->getWeakRef());
  KJ_EXPECT(set.size() == 3);

  // Multiple stays Multiple
  set.add(item4->getWeakRef());
  KJ_EXPECT(set.size() == 4);

  // Multiple -> Multiple (one less)
  removeValue(set, 4);
  KJ_EXPECT(set.size() == 3);

  // Multiple -> Double
  removeValue(set, 3);
  KJ_EXPECT(set.size() == 2);

  // Double -> Single
  removeValue(set, 2);
  KJ_EXPECT(set.size() == 1);

  // Single -> None
  removeValue(set, 1);
  KJ_EXPECT(set.size() == 0);
}

KJ_TEST("SmallWeakVector: iteration") {
  SmallWeakVector<TestItem> set;

  // Empty iteration
  int count = 0;
  for (auto& item: set) {
    (void)item;
    count++;
  }
  KJ_EXPECT(count == 0);

  // Single item
  auto item1 = kj::heap<TestItem>(1);
  set.add(item1->getWeakRef());
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item.value == 1);
    count++;
  }
  KJ_EXPECT(count == 1);

  // Two items
  auto item2 = kj::heap<TestItem>(2);
  set.add(item2->getWeakRef());
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item.value == 1 || item.value == 2);
    count++;
  }
  KJ_EXPECT(count == 2);

  // Multiple items
  auto item3 = kj::heap<TestItem>(3);
  set.add(item3->getWeakRef());
  count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item.value >= 1 && item.value <= 3);
    count++;
  }
  KJ_EXPECT(count == 3);
}

KJ_TEST("SmallWeakVector: clear") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  auto item3 = kj::heap<TestItem>(3);
  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());
  KJ_EXPECT(set.size() == 3);

  set.clear();
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!containsValue(set, 1));
  KJ_EXPECT(!containsValue(set, 2));
  KJ_EXPECT(!containsValue(set, 3));
}

KJ_TEST("SmallWeakVector: forEach for safe iteration during modification") {
  // This simulates the queue.h use case where items may be removed during iteration.
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  auto item3 = kj::heap<TestItem>(3);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());
  KJ_EXPECT(set.size() == 3);

  // forEach takes a snapshot internally, so items remain valid even if removed during iteration.
  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& item) {
    foundValues.add(item.value);
    // Remove the item from the vector during iteration - this is safe because
    // forEach iterates over an internal snapshot, not the vector itself.
    removeValue(set, item.value);
  });

  KJ_EXPECT(foundValues.size() == 3);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallWeakVector: forEach from single state") {
  SmallWeakVector<TestItem> set;

  auto item = kj::heap<TestItem>(42);
  set.add(item->getWeakRef());

  size_t count = 0;
  set.forEach([&](TestItem& ref) {
    KJ_EXPECT(ref.value == 42);
    count++;
  });
  KJ_EXPECT(count == 1);
}

KJ_TEST("SmallWeakVector: forEach from double state") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());

  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& ref) { foundValues.add(ref.value); });
  KJ_EXPECT(foundValues.size() == 2);
  // Order doesn't matter here; just check both values are present.
  bool found1 = false, found2 = false;
  for (auto v: foundValues) {
    if (v == 1) found1 = true;
    if (v == 2) found2 = true;
  }
  KJ_EXPECT(found1);
  KJ_EXPECT(found2);
}

KJ_TEST("SmallWeakVector: forEach from empty state") {
  SmallWeakVector<TestItem> set;
  size_t count = 0;
  set.forEach([&](TestItem& ref) { count++; });
  KJ_EXPECT(count == 0);
}

KJ_TEST("SmallWeakVector: iteration skips expired Weak refs") {
  // Test that iteration properly skips items whose Weak refs have expired.
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  KJ_EXPECT(set.size() == 2);

  // Expire item1's weak ref by destroying item1.
  item1 = nullptr;

  // forEach should only visit item2.
  kj::Vector<int> foundValues;
  set.forEach([&](TestItem& ref) { foundValues.add(ref.value); });

  KJ_EXPECT(foundValues.size() == 1);
  KJ_EXPECT(foundValues[0] == 2);

  // Range iteration should also only visit item2.
  size_t count = 0;
  for (auto& item: set) {
    KJ_EXPECT(item.value == 2);
    count++;
  }
  KJ_EXPECT(count == 1);
}

KJ_TEST("SmallWeakVector: post-increment skips expired Weak refs") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);
  auto item3 = kj::heap<TestItem>(3);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());

  item2 = nullptr;

  auto iter = set.begin();
  KJ_EXPECT((*iter++).value == 1);
  KJ_EXPECT((*iter).value == 3);
}

KJ_TEST("SmallWeakVector: removeAll removes double state to none when remaining ref is expired") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  KJ_EXPECT(set.size() == 2);

  item1 = nullptr;

  removeValue(set, 2);
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallWeakVector: removeAll removes all matching live refs") {
  SmallWeakVector<TestItem> set;

  auto item1 = kj::heap<TestItem>(1);
  auto item2 = kj::heap<TestItem>(1);
  auto item3 = kj::heap<TestItem>(2);

  set.add(item1->getWeakRef());
  set.add(item2->getWeakRef());
  set.add(item3->getWeakRef());

  set.removeAll([](TestItem& item) { return item.value == 1; });
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(containsValue(set, 2));
}

KJ_TEST("SmallWeakVector: weak refs do not keep targets alive") {
  SmallWeakVector<TestItem> set;

  {
    auto item = kj::heap<TestItem>(42);
    set.add(item->getWeakRef());
    KJ_EXPECT(containsValue(set, 42));
  }

  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(!containsValue(set, 42));

  size_t count = 0;
  set.forEach([&](TestItem& item) {
    (void)item;
    count++;
  });
  KJ_EXPECT(count == 0);
}

}  // namespace
}  // namespace workerd
