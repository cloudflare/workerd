// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/STLFunctionalExtras.h"

namespace workerd::clang_tidy::detail {

// LLVM's use-after-move analysis with a hook to normalize its private CFG.
// The shared AST is never modified.
void checkUseAfterMove(const clang::ast_matchers::MatchFinder::MatchResult& result,
    clang::tidy::ClangTidyCheck& check,
    llvm::ArrayRef<llvm::StringRef> invalidationFunctions,
    llvm::ArrayRef<llvm::StringRef> reinitializationFunctions,
    llvm::function_ref<void(clang::CFG&)> adjustCFG);

}  // namespace workerd::clang_tidy::detail
