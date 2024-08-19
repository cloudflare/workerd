// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "string-buffer.h"
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("append StringPtr") {
  StringBuffer<100> buffer(100);
  buffer.append(kj::StringPtr("abcdef"));
  KJ_EXPECT("abcdef"_kj == buffer.toString());
}

KJ_TEST("append String") {
  StringBuffer<100> buffer(100);
  auto str = kj::heapString("abc"_kj);
  buffer.append(str);
  KJ_EXPECT("abc"_kj == buffer.toString());
}

KJ_TEST("append char array") {
  StringBuffer<100> buffer(100);
  auto str = kj::heapString("abc");
  buffer.append(str);
  KJ_EXPECT("abc"_kj == buffer.toString());
}

KJ_TEST("overflow") {
  StringBuffer<10> buffer(11);

  for (auto i = 0; i < 100; i++) {
    // 3 character will test all sorts of boundary conditions
    // with 11-bytes heap chunks.
    buffer.append("abc");
  }
  KJ_EXPECT(buffer.toString().size() == 300);
  KJ_EXPECT(
    "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc"
    "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc"
    "abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc"
    "abcabcabcabcabcabcabc"_kj == buffer.toString());
}

} // namespace
} // namespace workerd
