// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wasm-shutdown-signal.h"

#include <kj/test.h>

#include <cstring>

namespace workerd {
namespace {

// ---------------------------------------------------------------------------
// AtomicList tests
// ---------------------------------------------------------------------------

KJ_TEST("AtomicList pushFront and iterate") {
  AtomicList<int> list;

  KJ_EXPECT(list.isEmpty());

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  KJ_EXPECT(!list.isEmpty());

  // Iterate should visit 1, 2, 3 (pushFront prepends).
  int expected = 1;
  list.iterate([&](int value) {
    KJ_EXPECT(value == expected, value, expected);
    ++expected;
  });
  KJ_EXPECT(expected == 4);
}

KJ_TEST("AtomicList filter removes matching nodes") {
  AtomicList<int> list;

  list.pushFront(5);
  list.pushFront(4);
  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Keep only odd numbers.
  list.filter([](int value) { return value % 2 != 0; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 3);
  KJ_EXPECT(remaining[0] == 1);
  KJ_EXPECT(remaining[1] == 3);
  KJ_EXPECT(remaining[2] == 5);
}

KJ_TEST("AtomicList filter removes all nodes") {
  AtomicList<int> list;

  list.pushFront(2);
  list.pushFront(4);
  list.pushFront(6);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("AtomicList filter keeps all nodes") {
  AtomicList<int> list;

  list.pushFront(1);
  list.pushFront(2);
  list.pushFront(3);

  list.filter([](int) { return true; });

  int count = 0;
  list.iterate([&](int) { ++count; });
  KJ_EXPECT(count == 3);
}

KJ_TEST("AtomicList single element filter remove") {
  AtomicList<int> list;

  list.pushFront(42);

  list.filter([](int) { return false; });

  KJ_EXPECT(list.isEmpty());
}

KJ_TEST("AtomicList filter removes head only") {
  AtomicList<int> list;

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Remove head (value 1).
  list.filter([](int value) { return value != 1; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 2);
  KJ_EXPECT(remaining[0] == 2);
  KJ_EXPECT(remaining[1] == 3);
}

KJ_TEST("AtomicList filter removes tail only") {
  AtomicList<int> list;

  list.pushFront(3);
  list.pushFront(2);
  list.pushFront(1);

  // Remove tail (value 3).
  list.filter([](int value) { return value != 3; });

  kj::Vector<int> remaining;
  list.iterate([&](int value) { remaining.add(value); });

  KJ_EXPECT(remaining.size() == 2);
  KJ_EXPECT(remaining[0] == 1);
  KJ_EXPECT(remaining[1] == 2);
}

// ---------------------------------------------------------------------------
// writeWasmShutdownSignals and WasmShutdownSignal tests are covered by the
// JS-level wasm-shutdown-signal-js-test.wd-test, which runs inside a real
// workerd instance with V8 initialized. V8's BackingStore API requires the
// V8 sandbox to be set up, so we cannot test it in a plain kj_test.
// ---------------------------------------------------------------------------

}  // namespace
}  // namespace workerd
