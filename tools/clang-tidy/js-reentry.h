// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace clang {
class FunctionDecl;
class Stmt;
}  // namespace clang

namespace workerd::clang_tidy {

// Flags a borrow into JSG-owned state that is still used after a call that can run JavaScript.
//
// User JavaScript run in the middle of a C++ method (a promise's `then` getter, a stream's pull()
// algorithm, an event listener) can drop the last reference to the object a method is working on,
// transition its state, or grow the container it is iterating, freeing what a local reference
// points to. The check tracks local references, pointers, iterators and views (kj::ArrayPtr,
// kj::StringPtr, kj::Ptr) bound into state reached through `this`, a reference or view
// parameter, or a member of an owning pointer, and reports:
//
// - a use of such a borrow after a call that can run JavaScript, and
// - a call that can run JavaScript made on such a borrow, which can free its own receiver.
//
// A call can run JavaScript if its callee takes a `jsg::Lock&` (unless it is a lambda, whose body
// decides), is annotated JSG_MAY_RUN_JS, is one of the V8 entry points that run script (calling a
// function, resolving a promise, reading, writing or enumerating a property, converting to a
// string or number, running microtasks, forcing a GC), or has a body in the translation unit that
// makes such a call outside an unevaluated operand. A callee annotated JSG_NO_JS, or an override of
// one, never runs JavaScript. A strong reference (`addRef()`), a copy, or a local owning pointer
// dereferenced directly is not a borrow; a `jsg::Function` keeps itself alive while it is called,
// and a `kj::Function` is held in C++ state. A loop that ends after its first pass, as
// KJ_CASE_ONEOF's does, does not carry a borrow into a second pass.
//
// Sites that are known to be safe carry `// NOLINT(jsg-js-reentry)` with the reason.
class JsReentryCheck: public clang::tidy::ClangTidyCheck {
 public:
  JsReentryCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void onStartOfTranslationUnit() override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
  std::optional<clang::TraversalKind> getCheckTraversalKind() const override {
    return clang::TK_AsIs;
  }

 private:
  llvm::DenseMap<const clang::FunctionDecl*, bool> mayRunJsCache;
  // Functions mayRunJs() is deciding, by their depth on its recursion stack.
  llvm::DenseMap<const clang::FunctionDecl*, unsigned> activeDepth;
  // The shallowest of those that the current function's answer depended on.
  unsigned lowestActiveDependency = UINT_MAX;
  // Functions decided `false` while a caller they depend on was still undecided, in the order
  // decided, and the depth of the shallowest such caller for each.
  llvm::SmallVector<const clang::FunctionDecl*, 8> provisionalFalse;
  llvm::DenseMap<const clang::FunctionDecl*, unsigned> provisionalDependency;

  bool mayRunJs(const clang::FunctionDecl* function);
  void checkBody(
      const clang::FunctionDecl* function, const clang::Stmt* body, clang::ASTContext& context);
};

}  // namespace workerd::clang_tidy
