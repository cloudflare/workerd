// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "small-set.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("SmallSet: empty set") {
  SmallSet<int*> set;
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);

  int dummy;
  KJ_EXPECT(!set.contains(&dummy));
}

KJ_TEST("SmallSet: add and remove single item") {
  SmallSet<int*> set;
  int a = 1;

  KJ_EXPECT(set.add(&a));
  KJ_EXPECT(!set.empty());
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(set.contains(&a));

  // Adding the same item should return false
  KJ_EXPECT(!set.add(&a));
  KJ_EXPECT(set.size() == 1);

  KJ_EXPECT(set.remove(&a));
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!set.contains(&a));

  // Removing again should return false
  KJ_EXPECT(!set.remove(&a));
}

KJ_TEST("SmallSet: add and remove two items") {
  SmallSet<int*> set;
  int a = 1, b = 2;

  KJ_EXPECT(set.add(&a));
  KJ_EXPECT(set.add(&b));
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(set.contains(&a));
  KJ_EXPECT(set.contains(&b));

  // Adding duplicates should return false
  KJ_EXPECT(!set.add(&a));
  KJ_EXPECT(!set.add(&b));
  KJ_EXPECT(set.size() == 2);

  KJ_EXPECT(set.remove(&a));
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(!set.contains(&a));
  KJ_EXPECT(set.contains(&b));

  KJ_EXPECT(set.remove(&b));
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallSet: add and remove multiple items") {
  SmallSet<int*> set;
  int a = 1, b = 2, c = 3, d = 4;

  KJ_EXPECT(set.add(&a));
  KJ_EXPECT(set.add(&b));
  KJ_EXPECT(set.add(&c));
  KJ_EXPECT(set.add(&d));
  KJ_EXPECT(set.size() == 4);

  KJ_EXPECT(set.contains(&a));
  KJ_EXPECT(set.contains(&b));
  KJ_EXPECT(set.contains(&c));
  KJ_EXPECT(set.contains(&d));

  KJ_EXPECT(set.remove(&b));
  KJ_EXPECT(set.size() == 3);
  KJ_EXPECT(!set.contains(&b));

  KJ_EXPECT(set.remove(&c));
  KJ_EXPECT(set.size() == 2);
  KJ_EXPECT(set.contains(&a));
  KJ_EXPECT(set.contains(&d));

  KJ_EXPECT(set.remove(&a));
  KJ_EXPECT(set.size() == 1);
  KJ_EXPECT(set.contains(&d));

  KJ_EXPECT(set.remove(&d));
  KJ_EXPECT(set.empty());
}

KJ_TEST("SmallSet: state transitions") {
  SmallSet<int*> set;
  int a = 1, b = 2, c = 3, d = 4;

  // None -> Single
  KJ_EXPECT(set.add(&a));
  KJ_EXPECT(set.size() == 1);

  // Single -> Double
  KJ_EXPECT(set.add(&b));
  KJ_EXPECT(set.size() == 2);

  // Double -> Multiple
  KJ_EXPECT(set.add(&c));
  KJ_EXPECT(set.size() == 3);

  // Multiple stays Multiple
  KJ_EXPECT(set.add(&d));
  KJ_EXPECT(set.size() == 4);

  // Multiple -> Multiple (one less)
  KJ_EXPECT(set.remove(&d));
  KJ_EXPECT(set.size() == 3);

  // Multiple -> Double
  KJ_EXPECT(set.remove(&c));
  KJ_EXPECT(set.size() == 2);

  // Double -> Single
  KJ_EXPECT(set.remove(&b));
  KJ_EXPECT(set.size() == 1);

  // Single -> None
  KJ_EXPECT(set.remove(&a));
  KJ_EXPECT(set.size() == 0);
}

KJ_TEST("SmallSet: iteration") {
  SmallSet<int*> set;
  int a = 1, b = 2, c = 3;

  // Empty iteration
  int count = 0;
  for (auto item: set) {
    (void)item;
    count++;
  }
  KJ_EXPECT(count == 0);

  // Single item
  set.add(&a);
  count = 0;
  for (auto item: set) {
    KJ_EXPECT(item == &a);
    count++;
  }
  KJ_EXPECT(count == 1);

  // Two items
  set.add(&b);
  count = 0;
  kj::Vector<int*> found;
  for (auto item: set) {
    found.add(item);
    count++;
  }
  KJ_EXPECT(count == 2);
  KJ_EXPECT(found.size() == 2);

  // Multiple items
  set.add(&c);
  count = 0;
  found.clear();
  for (auto item: set) {
    found.add(item);
    count++;
  }
  KJ_EXPECT(count == 3);
  KJ_EXPECT(found.size() == 3);
}

KJ_TEST("SmallSet: clear") {
  SmallSet<int*> set;
  int a = 1, b = 2, c = 3;

  set.add(&a);
  set.add(&b);
  set.add(&c);
  KJ_EXPECT(set.size() == 3);

  set.clear();
  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
  KJ_EXPECT(!set.contains(&a));
  KJ_EXPECT(!set.contains(&b));
  KJ_EXPECT(!set.contains(&c));
}

KJ_TEST("SmallSet: snapshot for safe iteration during self-removal") {
  // This simulates the queue.h use case where consumers remove themselves
  // during close/error callbacks

  struct RemovableThing {
    SmallSet<RemovableThing*>* owner;
    int value;

    void removeSelf() {
      owner->remove(this);
    }
  };

  SmallSet<RemovableThing*> set;
  RemovableThing a{&set, 1}, b{&set, 2}, c{&set, 3};

  set.add(&a);
  set.add(&b);
  set.add(&c);
  KJ_EXPECT(set.size() == 3);

  // Without snapshot, this would cause iterator invalidation
  // With snapshot, it's safe
  auto snapshot = set.snapshot();
  for (auto* item: snapshot) {
    item->removeSelf();
  }

  KJ_EXPECT(set.empty());
  KJ_EXPECT(set.size() == 0);
}

KJ_TEST("SmallSet: snapshot from single state") {
  SmallSet<int*> set;
  int a = 1;

  set.add(&a);
  auto snapshot = set.snapshot();
  KJ_EXPECT(snapshot.size() == 1);
  KJ_EXPECT(snapshot[0] == &a);
}

KJ_TEST("SmallSet: snapshot from double state") {
  SmallSet<int*> set;
  int a = 1, b = 2;

  set.add(&a);
  set.add(&b);
  auto snapshot = set.snapshot();
  KJ_EXPECT(snapshot.size() == 2);
  // Order doesn't matter for set semantics
  KJ_EXPECT((snapshot[0] == &a && snapshot[1] == &b) || (snapshot[0] == &b && snapshot[1] == &a));
}

KJ_TEST("SmallSet: snapshot from empty state") {
  SmallSet<int*> set;
  auto snapshot = set.snapshot();
  KJ_EXPECT(snapshot.size() == 0);
}

}  // namespace
}  // namespace workerd
