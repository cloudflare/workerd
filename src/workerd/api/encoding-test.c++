// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include <kj/test.h>

namespace workerd::api {
namespace test {

// These tests verify the findBestFit() function used by TextEncoder.encodeInto().
//
// bestFit(input, bufferSize) returns the number of input code units that can be
// fully converted to UTF-8 and fit within the given output buffer size in bytes.
//
// Different characters expand to different UTF-8 byte lengths:
//   - ASCII (U+0000-U+007F): 1 byte per code unit
//   - Latin-1 extended (U+0080-U+00FF): 2 bytes per code unit
//   - BMP characters (U+0100-U+FFFF): 2-3 bytes per code unit
//   - Supplementary characters (U+10000+): 4 bytes, encoded as surrogate pairs in UTF-16
//
// The function must never split a surrogate pair, so if there's only room for part of
// a multi-byte character, it stops before that character.
KJ_TEST("BestFitASCII") {
  // If there's zero input or output space, the answer is zero.
  KJ_ASSERT(bestFit("", 0) == 0);
  KJ_ASSERT(bestFit("a", 0) == 0);
  KJ_ASSERT(bestFit("aa", 0) == 0);
  KJ_ASSERT(bestFit("aaa", 0) == 0);
  KJ_ASSERT(bestFit("aaaa", 0) == 0);
  KJ_ASSERT(bestFit("aaaaa", 0) == 0);
  KJ_ASSERT(bestFit("", 0) == 0);
  KJ_ASSERT(bestFit("", 1) == 0);
  KJ_ASSERT(bestFit("", 2) == 0);
  KJ_ASSERT(bestFit("", 3) == 0);
  KJ_ASSERT(bestFit("", 4) == 0);
  KJ_ASSERT(bestFit("", 5) == 0);
  // Zero cases with two-byte strings.
  KJ_ASSERT(bestFit(u"", 0) == 0);
  KJ_ASSERT(bestFit(u"€", 0) == 0);
  KJ_ASSERT(bestFit(u"€€", 0) == 0);
  KJ_ASSERT(bestFit(u"€€€", 0) == 0);
  KJ_ASSERT(bestFit(u"€€€€", 0) == 0);
  KJ_ASSERT(bestFit(u"€€€€€", 0) == 0);
  KJ_ASSERT(bestFit(u"", 0) == 0);
  KJ_ASSERT(bestFit(u"", 1) == 0);
  KJ_ASSERT(bestFit(u"", 2) == 0);
  KJ_ASSERT(bestFit(u"", 3) == 0);
  KJ_ASSERT(bestFit(u"", 4) == 0);
  KJ_ASSERT(bestFit(u"", 5) == 0);
  // Small buffers that only just fit.
  KJ_ASSERT(bestFit(u"a", 1) == 1);
  KJ_ASSERT(bestFit(u"å", 2) == 1);
  KJ_ASSERT(bestFit(u"€", 3) == 1);
  KJ_ASSERT(bestFit(u"😹", 4) == 2);
  // Small buffers that don't fit.
  KJ_ASSERT(bestFit(u"å", 1) == 0);
  KJ_ASSERT(bestFit(u"€", 2) == 0);
  KJ_ASSERT(bestFit(u"😹", 3) == 0);
  // Don't chop a surrogate pair.
  KJ_ASSERT(bestFit(u"1😹", 4) == 1);
  KJ_ASSERT(bestFit(u"12😹", 5) == 2);
  KJ_ASSERT(bestFit(u"123😹", 6) == 3);
  KJ_ASSERT(bestFit(u"1234😹", 7) == 4);
  KJ_ASSERT(bestFit(u"12345😹", 8) == 5);
  // Some bigger ones just for fun.
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 0) == 0);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 1) == 0);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 2) == 0);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 3) == 0);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 4) == 2);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 5) == 2);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 6) == 2);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 7) == 2);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 8) == 4);
  KJ_ASSERT(bestFit(u"😹😹😹😹😹😹", 9) == 4);
  KJ_ASSERT(bestFit(u"0😹😹😹😹😹😹", 9) == 5);          // 0😹😹 is 5 and takes 9.
  KJ_ASSERT(bestFit(u"01😹😹😹😹😹😹", 9) == 4);         // 01😹 is 4 and takes 6.
  KJ_ASSERT(bestFit(u"012😹😹😹😹😹😹", 9) == 5);        // 012😹 is 5 and takes 7.
  KJ_ASSERT(bestFit(u"0123😹😹😹😹😹😹", 9) == 6);       // 0123😹 is 6 and takes 8.
  KJ_ASSERT(bestFit(u"01234😹😹😹😹😹😹", 9) == 7);      // 01234😹 is 7 and takes 9.
  KJ_ASSERT(bestFit(u"012345😹😹😹😹😹😹", 9) == 6);     // 012345 is 6 and takes 6.
  KJ_ASSERT(bestFit(u"0123456😹😹😹😹😹😹", 9) == 7);    // 0123456 is 7 and takes 7.
  KJ_ASSERT(bestFit(u"01234567😹😹😹😹😹😹", 9) == 8);   // 0123456 is 8 and takes 8.
  KJ_ASSERT(bestFit(u"012345678😹😹😹😹😹😹", 9) == 9);  // 0123456 is 9 and takes 9.
}

}  // namespace test
}  // namespace workerd::api
