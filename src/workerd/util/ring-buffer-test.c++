// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ring-buffer.h"

#include <kj/test.h>

namespace workerd {
namespace {

using kj::uint;

KJ_TEST("RingBuffer basic operations") {
  RingBuffer<int> buffer;

  KJ_EXPECT(buffer.empty());
  KJ_EXPECT(buffer.size() == 0);

  buffer.push_back(1);
  KJ_EXPECT(!buffer.empty());
  KJ_EXPECT(buffer.size() == 1);
  KJ_EXPECT(buffer.front() == 1);
  KJ_EXPECT(buffer.back() == 1);

  buffer.push_back(2);
  KJ_EXPECT(buffer.size() == 2);
  KJ_EXPECT(buffer.front() == 1);
  KJ_EXPECT(buffer.back() == 2);

  buffer.push_back(3);
  KJ_EXPECT(buffer.size() == 3);
  KJ_EXPECT(buffer.front() == 1);
  KJ_EXPECT(buffer.back() == 3);

  buffer.pop_front();
  KJ_EXPECT(buffer.size() == 2);
  KJ_EXPECT(buffer.front() == 2);
  KJ_EXPECT(buffer.back() == 3);

  buffer.pop_front();
  KJ_EXPECT(buffer.size() == 1);
  KJ_EXPECT(buffer.front() == 3);
  KJ_EXPECT(buffer.back() == 3);

  buffer.pop_front();
  KJ_EXPECT(buffer.empty());
  KJ_EXPECT(buffer.size() == 0);
}

KJ_TEST("RingBuffer push_back with move semantics") {
  RingBuffer<kj::String> buffer;

  auto str1 = kj::heapString("hello");
  auto str2 = kj::heapString("world");

  buffer.push_back(kj::mv(str1));
  buffer.push_back(kj::mv(str2));

  KJ_EXPECT(buffer.size() == 2);
  KJ_EXPECT(buffer.front() == "hello");
  KJ_EXPECT(buffer.back() == "world");
}

KJ_TEST("RingBuffer push_back with copy") {
  RingBuffer<int> buffer;

  int value = 42;
  buffer.push_back(value);
  KJ_EXPECT(buffer.size() == 1);
  KJ_EXPECT(buffer.front() == 42);
  KJ_EXPECT(value == 42);  // Original value unchanged
}

KJ_TEST("RingBuffer emplace_back") {
  struct TestStruct {
    int a = 0;
    TestStruct() = default;
    TestStruct(int a): a(a) {}
    TestStruct(const TestStruct&) = default;
    TestStruct& operator=(const TestStruct&) = default;
  };

  RingBuffer<TestStruct> buffer;

  auto& ref = buffer.emplace_back(10);
  KJ_EXPECT(buffer.size() == 1);
  KJ_EXPECT(ref.a == 10);
  KJ_EXPECT(buffer.front().a == 10);
}

KJ_TEST("RingBuffer clear") {
  RingBuffer<int> buffer;

  for (int i = 0; i < 10; i++) {
    buffer.push_back(i);
  }
  KJ_EXPECT(buffer.size() == 10);

  buffer.clear();
  KJ_EXPECT(buffer.empty());
  KJ_EXPECT(buffer.size() == 0);

  // Should be able to use after clear
  buffer.push_back(1);
  KJ_EXPECT(buffer.size() == 1);
  KJ_EXPECT(buffer.front() == 1);
}

KJ_TEST("RingBuffer iterator basic") {
  RingBuffer<int> buffer;

  for (int i = 0; i < 5; i++) {
    buffer.push_back(i);
  }

  int expected = 0;
  // Using verbose syntax to test iterator
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (auto it = buffer.begin(); it != buffer.end(); ++it) {
    KJ_EXPECT(*it == expected++);
  }
  KJ_EXPECT(expected == 5);
}

KJ_TEST("RingBuffer iterator range-based for loop") {
  RingBuffer<int> buffer;

  for (int i = 1; i <= 5; i++) {
    buffer.push_back(i * 10);
  }

  int expected = 10;
  for (auto& value: buffer) {
    KJ_EXPECT(value == expected);
    expected += 10;
  }
  KJ_EXPECT(expected == 60);
}

KJ_TEST("RingBuffer iterator modification") {
  RingBuffer<int> buffer;

  for (int i = 0; i < 5; i++) {
    buffer.push_back(i);
  }

  // Modify through iterator
  for (auto& value: buffer) {
    value *= 2;
  }

  int expected = 0;
  for (const auto& value: buffer) {
    KJ_EXPECT(value == expected * 2);
    expected++;
  }
}

KJ_TEST("RingBuffer const_iterator") {
  RingBuffer<int> buffer;

  for (int i = 0; i < 5; i++) {
    buffer.push_back(i);
  }

  const auto& constBuffer = buffer;

  int expected = 0;
  // Using verbose syntax to test iterator
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (auto it = constBuffer.begin(); it != constBuffer.end(); ++it) {
    KJ_EXPECT(*it == expected++);
  }
}

KJ_TEST("RingBuffer iterator decrement") {
  RingBuffer<int> buffer;

  for (int i = 0; i < 5; i++) {
    buffer.push_back(i);
  }

  auto it = buffer.end();
  --it;
  KJ_EXPECT(*it == 4);
  --it;
  KJ_EXPECT(*it == 3);

  it--;
  KJ_EXPECT(*it == 2);
}

KJ_TEST("RingBuffer iterator equality") {
  RingBuffer<int> buffer;
  buffer.push_back(1);
  buffer.push_back(2);

  auto it1 = buffer.begin();
  auto it2 = buffer.begin();
  KJ_EXPECT(it1 == it2);

  ++it2;
  KJ_EXPECT(it1 != it2);

  auto end1 = buffer.end();
  auto end2 = buffer.end();
  KJ_EXPECT(end1 == end2);
}

KJ_TEST("RingBuffer iterator arrow operator") {
  struct Point {
    int x;
    int y;
  };

  RingBuffer<Point> buffer;
  buffer.push_back(Point{1, 2});
  buffer.push_back(Point{3, 4});

  auto it = buffer.begin();
  KJ_EXPECT(it->x == 1);
  KJ_EXPECT(it->y == 2);

  ++it;
  KJ_EXPECT(it->x == 3);
  KJ_EXPECT(it->y == 4);
}

KJ_TEST("RingBuffer growth when capacity exceeded") {
  RingBuffer<int, 4> buffer;  // Small initial capacity

  // Fill beyond initial capacity
  for (int i = 0; i < 10; i++) {
    buffer.push_back(i);
  }

  KJ_EXPECT(buffer.size() == 10);

  // Verify all elements are intact
  int expected = 0;
  for (const auto& value: buffer) {
    KJ_EXPECT(value == expected++);
  }
  KJ_EXPECT(expected == 10);
}

KJ_TEST("RingBuffer growth maintains order across wrap-around") {
  RingBuffer<int, 4> buffer;

  // Create wrap-around scenario: fill, then pop, then fill again
  buffer.push_back(1);
  buffer.push_back(2);
  buffer.push_back(3);
  buffer.push_back(4);

  buffer.pop_front();  // Remove 1
  buffer.pop_front();  // Remove 2

  // Now head is at index 2, tail wraps around
  buffer.push_back(5);
  buffer.push_back(6);
  buffer.push_back(7);  // This should trigger growth

  KJ_EXPECT(buffer.size() == 5);

  // Verify order is maintained: 3, 4, 5, 6, 7
  int expected = 3;
  for (const auto& value: buffer) {
    KJ_EXPECT(value == expected++);
  }
  KJ_EXPECT(expected == 8);
}

KJ_TEST("RingBuffer with non-trivial types") {
  struct ComplexType {
    kj::String str;
    kj::Vector<int> vec;

    ComplexType(kj::String s): str(kj::mv(s)) {
      vec.add(1);
      vec.add(2);
    }

    ComplexType(ComplexType&& other) = default;
    ComplexType& operator=(ComplexType&& other) = default;

    KJ_DISALLOW_COPY(ComplexType);
  };

  RingBuffer<kj::Own<ComplexType>> buffer;

  buffer.push_back(kj::heap<ComplexType>(kj::str("first")));
  buffer.push_back(kj::heap<ComplexType>(kj::str("second")));
  buffer.push_back(kj::heap<ComplexType>(kj::str("third")));

  KJ_EXPECT(buffer.size() == 3);
  KJ_EXPECT(buffer.front()->str == "first");
  KJ_EXPECT(buffer.back()->str == "third");

  buffer.pop_front();
  KJ_EXPECT(buffer.front()->str == "second");
}

KJ_TEST("RingBuffer destructor calls element destructors") {
  struct DestructionDetector {
    DestructionDetector(uint& count): count(count) {}
    ~DestructionDetector() noexcept(false) {
      ++count;
    }
    KJ_DISALLOW_COPY_AND_MOVE(DestructionDetector);
    uint& count;
  };

  uint destructionCount = 0;

  {
    RingBuffer<kj::Own<DestructionDetector>> buffer;
    buffer.push_back(kj::heap<DestructionDetector>(destructionCount));
    buffer.push_back(kj::heap<DestructionDetector>(destructionCount));
    buffer.push_back(kj::heap<DestructionDetector>(destructionCount));

    KJ_EXPECT(destructionCount == 0);
  }  // Buffer goes out of scope

  KJ_EXPECT(destructionCount == 3);
}

KJ_TEST("RingBuffer pop_front calls element destructor") {
  struct DestructionDetector {
    DestructionDetector(uint& count): count(count) {}
    ~DestructionDetector() noexcept(false) {
      ++count;
    }
    KJ_DISALLOW_COPY_AND_MOVE(DestructionDetector);
    uint& count;
  };

  uint destructionCount = 0;

  RingBuffer<kj::Own<DestructionDetector>> buffer;
  buffer.push_back(kj::heap<DestructionDetector>(destructionCount));
  buffer.push_back(kj::heap<DestructionDetector>(destructionCount));

  KJ_EXPECT(destructionCount == 0);

  buffer.pop_front();
  KJ_EXPECT(destructionCount == 1);

  buffer.pop_front();
  KJ_EXPECT(destructionCount == 2);
}

KJ_TEST("RingBuffer clear calls all element destructors") {
  struct DestructionDetector {
    DestructionDetector(uint& count): count(count) {}
    ~DestructionDetector() noexcept(false) {
      ++count;
    }
    KJ_DISALLOW_COPY_AND_MOVE(DestructionDetector);
    uint& count;
  };

  uint destructionCount = 0;

  RingBuffer<kj::Own<DestructionDetector>> buffer;
  for (int i = 0; i < 5; i++) {
    buffer.push_back(kj::heap<DestructionDetector>(destructionCount));
  }

  KJ_EXPECT(destructionCount == 0);

  buffer.clear();
  KJ_EXPECT(destructionCount == 5);
  KJ_EXPECT(buffer.empty());
}

KJ_TEST("RingBuffer stress test - many operations") {
  RingBuffer<int, 4> buffer;

  // Perform many mixed operations
  for (int i = 0; i < 100; i++) {
    buffer.push_back(i);
  }

  for (int i = 0; i < 50; i++) {
    buffer.pop_front();
  }

  for (int i = 100; i < 150; i++) {
    buffer.push_back(i);
  }

  KJ_EXPECT(buffer.size() == 100);

  // Verify contents
  int expected = 50;
  for (const auto& value: buffer) {
    KJ_EXPECT(value == expected++);
  }
}

KJ_TEST("RingBuffer empty buffer iterators") {
  RingBuffer<int> buffer;

  KJ_EXPECT(buffer.begin() == buffer.end());
  KJ_EXPECT(buffer.cbegin() == buffer.cend());

  // Range-based for should not execute
  for ([[maybe_unused]] auto& value: buffer) {
    KJ_FAIL_EXPECT("Should not iterate over empty buffer");
  }
}

KJ_TEST("RingBuffer single element") {
  RingBuffer<int> buffer;
  buffer.push_back(42);

  KJ_EXPECT(buffer.front() == 42);
  KJ_EXPECT(buffer.back() == 42);
  KJ_EXPECT(buffer.size() == 1);

  int count = 0;
  for (const auto& value: buffer) {
    KJ_EXPECT(value == 42);
    count++;
  }
  KJ_EXPECT(count == 1);
}

KJ_TEST("RingBuffer alternating push/pop maintains correctness") {
  RingBuffer<int, 4> buffer;

  for (int round = 0; round < 10; round++) {
    buffer.push_back(round * 2);
    buffer.push_back(round * 2 + 1);

    KJ_EXPECT(buffer.front() == round * 2);
    buffer.pop_front();

    KJ_EXPECT(buffer.front() == round * 2 + 1);
    buffer.pop_front();

    KJ_EXPECT(buffer.empty());
  }
}

KJ_TEST("RingBuffer with custom initial capacity") {
  RingBuffer<int, 128> largeBuffer;
  RingBuffer<int, 2> tinyBuffer;

  // Both should work correctly regardless of initial capacity
  for (int i = 0; i < 10; i++) {
    largeBuffer.push_back(i);
    tinyBuffer.push_back(i);
  }

  KJ_EXPECT(largeBuffer.size() == 10);
  KJ_EXPECT(tinyBuffer.size() == 10);

  int expected = 0;
  for (const auto& value: largeBuffer) {
    KJ_EXPECT(value == expected++);
  }

  expected = 0;
  for (const auto& value: tinyBuffer) {
    KJ_EXPECT(value == expected++);
  }
}

KJ_TEST("RingBuffer front and back with wrap-around") {
  RingBuffer<int, 4> buffer;

  buffer.push_back(1);
  buffer.push_back(2);
  buffer.push_back(3);
  buffer.push_back(4);

  // Create wrap-around
  buffer.pop_front();
  buffer.pop_front();
  buffer.push_back(5);
  buffer.push_back(6);

  KJ_EXPECT(buffer.size() == 4);
  KJ_EXPECT(buffer.front() == 3);
  KJ_EXPECT(buffer.back() == 6);
}

KJ_TEST("RingBuffer emplace_back returns reference") {
  RingBuffer<int> buffer;

  auto& ref1 = buffer.emplace_back(10);
  ref1 = 20;

  KJ_EXPECT(buffer.front() == 20);

  buffer.emplace_back(30);
  buffer.emplace_back(40);

  auto& ref2 = buffer.emplace_back(50);
  ref2 = 60;

  KJ_EXPECT(buffer.back() == 60);
}

KJ_TEST("RingBuffer iterator conversion from mutable to const") {
  RingBuffer<int> buffer;
  buffer.push_back(1);
  buffer.push_back(2);

  auto it = buffer.begin();
  RingBuffer<int>::const_iterator cit = it;  // Conversion

  KJ_EXPECT(*cit == 1);
  ++cit;
  KJ_EXPECT(*cit == 2);
}

}  // namespace
}  // namespace workerd
