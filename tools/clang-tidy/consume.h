// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "llvm/ADT/DenseMap.h"

namespace clang {
class CXXMethodDecl;
}

namespace workerd::clang_tidy {

// Flags calls to WD_CONSUME methods made directly through kj::Ptr.
// Such calls must use consume(kj::mv(ptr))->method(...) so the active kj::Ptr is
// dropped before the callee can synchronously destroy its target.
class ConsumeCheck : public clang::tidy::ClangTidyCheck {
 public:
  ConsumeCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void onStartOfTranslationUnit() override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;

 private:
  llvm::DenseMap<const clang::CXXMethodDecl*, bool> methodRequiresConsumeCache;

  bool methodRequiresConsume(const clang::CXXMethodDecl* method);
};

}  // namespace workerd::clang_tidy
