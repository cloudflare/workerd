// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"

#include <kj/test.h>

namespace workerd {
namespace {

#define ASSERT_VALID_AND_EQUAL(upper, lower, str)                                                  \
  do {                                                                                             \
    auto a = KJ_ASSERT_NONNULL(UUID::fromUpperLower(upper, lower));                                \
    auto b = KJ_ASSERT_NONNULL(UUID::fromString(str));                                             \
    KJ_ASSERT(a.getUpper() == upper);                                                              \
    KJ_ASSERT(a.getLower() == lower);                                                              \
    KJ_ASSERT(b.getUpper() == upper);                                                              \
    KJ_ASSERT(b.getLower() == lower);                                                              \
    KJ_ASSERT(a == b);                                                                             \
  } while (false)

KJ_TEST("Valid UUIDs") {
  ASSERT_VALID_AND_EQUAL(
      72340172838076673ull, 1157442765409226768ull, "01010101-0101-0101-1010-101010101010");
  ASSERT_VALID_AND_EQUAL(
      81985529216486895ull, 81985529216486895ull, "01234567-89ab-cdef-0123-456789abcdef");
  ASSERT_VALID_AND_EQUAL(
      16045690984833335023ull, 16045690984833335023ull, "deadbeef-dead-beef-dead-beefdeadbeef");
}

KJ_TEST("Null UUIDs") {
  KJ_EXPECT(UUID::fromUpperLower(0ull, 0ull) == kj::none);
  KJ_EXPECT(UUID::fromString("00000000-0000-0000-0000-000000000000") == kj::none);
}

KJ_TEST("Invalid UUIDs") {
  KJ_EXPECT(UUID::fromString("") == kj::none);
  KJ_EXPECT(UUID::fromString("foo") == kj::none);
  KJ_EXPECT(UUID::fromString("+_{};'<>?,.`/'!@#$%^&*()") == kj::none);
  KJ_EXPECT(UUID::fromString("101010101-0101-0101-1010-101010101010") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-10101-0101-1010-101010101010") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-10101-1010-101010101010") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-0101-10101-101010101010") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-0101-1010-1010101010101") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-0101-1010-101010101010-") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-0101-1010-10101010101-") == kj::none);
  KJ_EXPECT(UUID::fromString("01010101-0101-0101-1010-10101010101g") == kj::none);
  KJ_EXPECT(UUID::fromString("0123456789abcdef0123456789abcdef") == kj::none);
}

}  // namespace
}  // namespace workerd
