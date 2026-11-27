// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "html-rewriter-selector.h"

#include <kj/test.h>
#include <kj/vector.h>

namespace workerd::api::htmlrewriter {
namespace {

kj::String makeCombinatorSelector(size_t combinators, kj::StringPtr separator) {
  kj::Vector<char> result;
  result.add('*');
  for (size_t i = 0; i < combinators; ++i) {
    result.addAll(separator);
    result.add('*');
  }
  result.add('\0');
  return kj::String(result.releaseAsArray());
}

kj::String makeNestedSelector(size_t depth, kj::StringPtr function = ":not("_kj) {
  kj::Vector<char> result;
  for (size_t i = 0; i < depth; ++i) {
    result.addAll(function);
  }
  result.add('*');
  for (size_t i = 0; i < depth; ++i) {
    result.add(')');
  }
  result.add('\0');
  return kj::String(result.releaseAsArray());
}

KJ_TEST("HTMLRewriter selector combinator complexity is bounded per branch") {
  KJ_EXPECT(
      !selectorExceedsComplexityLimit(makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY, ">"_kj)));
  KJ_EXPECT(
      selectorExceedsComplexityLimit(makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY + 1, ">"_kj)));
  KJ_EXPECT(!selectorExceedsComplexityLimit(
      makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY, " /**/ \t"_kj)));
  KJ_EXPECT(selectorExceedsComplexityLimit(
      makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY + 1, " /**/ \t"_kj)));

  auto manyBranches = kj::str(makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY, ">"_kj), ",",
      makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY, " "_kj));
  KJ_EXPECT(!selectorExceedsComplexityLimit(manyBranches));
}

KJ_TEST("HTMLRewriter selector functional nesting is bounded") {
  KJ_EXPECT(!selectorExceedsComplexityLimit(makeNestedSelector(MAX_SELECTOR_COMPLEXITY)));
  KJ_EXPECT(selectorExceedsComplexityLimit(makeNestedSelector(MAX_SELECTOR_COMPLEXITY + 1)));

  for (const auto& function: {":not("_kj, ":is("_kj, ":where("_kj, ":has("_kj}) {
    KJ_EXPECT(
        selectorExceedsComplexityLimit(makeNestedSelector(MAX_SELECTOR_COMPLEXITY + 1, function)));
  }
}

KJ_TEST("HTMLRewriter selector syntax cannot inflate or hide complexity") {
  kj::Vector<char> escapedLines;
  escapedLines.addAll(".a"_kj);
  for (size_t i = 0; i < MAX_SELECTOR_COMPLEXITY + 1; ++i) {
    escapedLines.addAll("\\\r\nb"_kj);
  }
  escapedLines.add('\0');

  KJ_EXPECT(!selectorExceedsComplexityLimit(kj::String(escapedLines.releaseAsArray())));
  KJ_EXPECT(
      !selectorExceedsComplexityLimit(R"CSS([data="a > b, \"] c"][other=x\]y] .item\>name)CSS"_kj));
  KJ_EXPECT(
      !selectorExceedsComplexityLimit(R"CSS(:not([data=") > "] /**/ .x, [data='\' >']))CSS"_kj));

  auto hiddenCombinators = kj::str("[data=\"", makeCombinatorSelector(300, ">"_kj), "\"]");
  KJ_EXPECT(!selectorExceedsComplexityLimit(hiddenCombinators));

  auto realCombinators = kj::str(
      "[data='/* > */']", makeCombinatorSelector(MAX_SELECTOR_COMPLEXITY + 1, " /**/ > /**/ "_kj));
  KJ_EXPECT(selectorExceedsComplexityLimit(realCombinators));
}

}  // namespace
}  // namespace workerd::api::htmlrewriter
