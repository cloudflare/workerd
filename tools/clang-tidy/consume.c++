// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "consume.h"

#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

namespace workerd::clang_tidy {
namespace {

constexpr llvm::StringRef kConsumeAnnotation = "workerd_consume";

bool hasConsumeAnnotation(const clang::Decl* decl) {
  for (const auto* attr: decl->specific_attrs<clang::AnnotateAttr>()) {
    if (attr->getAnnotation() == kConsumeAnnotation) return true;
  }
  return false;
}

bool methodRequiresConsumeImpl(
    const clang::CXXMethodDecl* method,
    llvm::DenseSet<const clang::CXXMethodDecl*>& visited,
    llvm::DenseMap<const clang::CXXMethodDecl*, bool>& cache) {
  method = method->getCanonicalDecl();
  auto cached = cache.find(method);
  if (cached != cache.end()) return cached->second;

  if (!visited.insert(method).second) return false;

  for (const auto* redecl: method->redecls()) {
    if (hasConsumeAnnotation(redecl)) {
      cache[method] = true;
      return true;
    }
  }

  for (const auto* overridden: method->overridden_methods()) {
    if (methodRequiresConsumeImpl(overridden, visited, cache)) {
      cache[method] = true;
      return true;
    }
  }

  cache[method] = false;
  return false;
}

}  // namespace

void ConsumeCheck::registerMatchers(clang::ast_matchers::MatchFinder* Finder) {
  using namespace clang::ast_matchers;

  auto kjPtrReceiver = expr(anyOf(
      hasType(cxxRecordDecl(hasName("::kj::Ptr"))),
      cxxOperatorCallExpr(
          hasOverloadedOperatorName("->"),
          callee(cxxMethodDecl(ofClass(cxxRecordDecl(hasName("::kj::Ptr"))))))));

  Finder->addMatcher(
      cxxMemberCallExpr(on(kjPtrReceiver), callee(cxxMethodDecl().bind("method"))).bind("call"),
      this);
}

void ConsumeCheck::onStartOfTranslationUnit() {
  methodRequiresConsumeCache.clear();
}

bool ConsumeCheck::methodRequiresConsume(const clang::CXXMethodDecl* method) {
  method = method->getCanonicalDecl();
  auto cached = methodRequiresConsumeCache.find(method);
  if (cached != methodRequiresConsumeCache.end()) return cached->second;

  llvm::DenseSet<const clang::CXXMethodDecl*> visited;
  return methodRequiresConsumeImpl(method, visited, methodRequiresConsumeCache);
}

void ConsumeCheck::check(
    const clang::ast_matchers::MatchFinder::MatchResult& Result) {
  const auto* call = Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("call");
  const auto* method = Result.Nodes.getNodeAs<clang::CXXMethodDecl>("method");
  if (call == nullptr || method == nullptr) return;
  if (!methodRequiresConsume(method)) return;

  diag(call->getExprLoc(),
      "%0 may synchronously destroy its target; call it through "
      "consume(kj::mv(ptr))->%0(...) instead of through kj::Ptr")
      << method->getNameAsString();
}

}  // namespace workerd::clang_tidy
