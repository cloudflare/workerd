// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "uuid.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("UUIDToString") {
  KJ_EXPECT(UUIDToString(0ull, 0ull) ==
      "00000000-0000-0000-0000-000000000000");
  KJ_EXPECT(UUIDToString(72340172838076673ull, 1157442765409226768ull) ==
      "01010101-0101-0101-1010-101010101010");
  KJ_EXPECT(UUIDToString(81985529216486895ull, 81985529216486895ull) ==
      "01234567-89ab-cdef-0123-456789abcdef");
  KJ_EXPECT(UUIDToString(16045690984833335023ull, 16045690984833335023ull) ==
      "deadbeef-dead-beef-dead-beefdeadbeef");
}

}  // namespace
}  // namespace workerd
