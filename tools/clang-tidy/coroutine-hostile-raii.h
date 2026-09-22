//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Derived from LLVM's misc/CoroutineHostileRAIICheck.h and modified for the
// workerd clang-tidy plugin.

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

#include <vector>

namespace workerd::clang_tidy {

// Detects hostile RAII objects that persist across coroutine suspension.
class CoroutineHostileRAIICheck: public clang::tidy::ClangTidyCheck {
 public:
  CoroutineHostileRAIICheck(clang::StringRef Name, clang::tidy::ClangTidyContext *Context);

  bool isLanguageVersionSupported(const clang::LangOptions &LangOpts) const override {
    return LangOpts.CPlusPlus20;
  }

  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override;
  void storeOptions(clang::tidy::ClangTidyOptions::OptionMap &Opts) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  std::optional<clang::TraversalKind> getCheckTraversalKind() const override {
    return clang::TK_AsIs;
  }

 private:
  std::vector<clang::StringRef> RAIITypesList;
  std::vector<clang::StringRef> AllowedAwaitablesList;
  std::vector<clang::StringRef> AllowedCallees;
};

}  // namespace workerd::clang_tidy
