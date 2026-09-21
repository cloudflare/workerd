// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/bugprone/UseAfterMoveCheck.h"

namespace workerd::clang_tidy {

// Extends bugprone-use-after-move to recognize kj::mv() while excluding
// KJ_CASE_ONEOF's generated one-iteration loop.
class UseAfterMoveCheck final: public clang::tidy::bugprone::UseAfterMoveCheck {
 public:
  UseAfterMoveCheck(clang::StringRef name, clang::tidy::ClangTidyContext* context)
      : clang::tidy::bugprone::UseAfterMoveCheck(name, context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
};

}  // namespace workerd::clang_tidy
