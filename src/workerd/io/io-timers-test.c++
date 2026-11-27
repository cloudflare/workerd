// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-timers.h"

#include <kj/test.h>

namespace workerd {
namespace {

constexpr TimeoutId::ValueType MAX_SAFE_INTEGER = (1ull << 53) - 1;

KJ_TEST("TimeoutId::Generator starts at 1 and counts up") {
  TimeoutId::Generator generator;
  KJ_EXPECT(generator.getNext().toNumber() == 1);
  KJ_EXPECT(generator.getNext().toNumber() == 2);
  KJ_EXPECT(generator.getNext().toNumber() == 3);
}

KJ_TEST("TimeoutId::Generator never hands out Number.MAX_SAFE_INTEGER") {
  TimeoutId::Generator generator(MAX_SAFE_INTEGER - 2);
  KJ_EXPECT(generator.getNext().toNumber() == MAX_SAFE_INTEGER - 2);
  KJ_EXPECT(generator.getNext().toNumber() == MAX_SAFE_INTEGER - 1);

  // The call that would return MAX_SAFE_INTEGER throws, and so does every later call: the
  // counter keeps advancing, so the generator never wraps around to reuse an id.
  for (auto i = 0; i < 2; ++i) {
    KJ_EXPECT_LOG(WARNING, "Unable to set timeout because there are no more unique ids");
    KJ_EXPECT_THROW_MESSAGE(
        "jsg.Error: Unable to set timeout because there are no more unique ids less than "
        "Number.MAX_SAFE_INTEGER.",
        generator.getNext());
  }
}

KJ_TEST("TimeoutId converts to and from a double without changing the value") {
  KJ_EXPECT(TimeoutId::fromNumber(42).toNumber() == 42);
  KJ_EXPECT(TimeoutId::fromNumber(static_cast<double>(MAX_SAFE_INTEGER - 1)).toNumber() ==
      static_cast<double>(MAX_SAFE_INTEGER - 1));
  KJ_EXPECT(TimeoutId::fromNumber(7.9).toNumber() == 7);
  KJ_EXPECT(TimeoutId::fromNumber(1) < TimeoutId::fromNumber(2));
}

}  // namespace
}  // namespace workerd
