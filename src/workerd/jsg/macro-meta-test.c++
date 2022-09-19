// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "macro-meta.h"
#include <kj/test.h>

namespace workerd::jsg {
namespace {

static_assert(JSG_IF_NONEMPTY(abcd, 123) == 123);
static_assert(JSG_IF_NONEMPTY(, abcd) true);

template <typename... T>
constexpr int sum(T... args) { return (args + ...); }

#define JSG_TEST_FOR_EACH_OP(op, x) (2 op x)
static_assert(JSG_FOR_EACH(JSG_TEST_FOR_EACH_OP, *) true);
static_assert(sum(JSG_FOR_EACH(JSG_TEST_FOR_EACH_OP, *, 1, 2, 3, 4)) == 20);

static_assert(sum(JSG_FOR_EACH(JSG_TEST_FOR_EACH_OP, *,
    1, 2, 3, 4, 5, 6, 7, 8,
    1, 2, 3, 4, 5, 6, 7, 8,
    1, 2, 3, 4, 5, 6, 7, 8,
    1, 2, 3, 4, 5, 6, 7, 8)) == 36*2*4);
#undef JSG_TEST_FOR_EACH_OP

KJ_TEST("macro meta") {
  // Nothing to actually do here; tests are compile-time
}

}  // namespace
}  // namespace workerd::jsg
