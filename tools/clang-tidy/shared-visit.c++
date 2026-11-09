// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "shared-visit.h"

#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

namespace workerd::clang_tidy {

void SharedVisitCheck::registerMatchers(clang::ast_matchers::MatchFinder* Finder) {
  using namespace clang::ast_matchers;

  auto refcounted =
      cxxRecordDecl(isSameOrDerivedFrom(hasAnyName("::kj::Refcounted", "::kj::AtomicRefcounted")));
  auto sharedOwner = classTemplateSpecializationDecl(anyOf(hasAnyName("::kj::Rc", "::kj::Arc"),
      allOf(hasName("::kj::Own"),
          hasTemplateArgument(0,
              refersToType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(refcounted))))))));
  auto sharedExpr =
      expr(hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(sharedOwner)))));
  auto derefShared = cxxOperatorCallExpr(
      hasAnyOverloadedOperatorName("*", "->"), hasArgument(0, ignoringParenImpCasts(sharedExpr)));

  auto gcVisitor = cxxRecordDecl(hasName("::workerd::jsg::GcVisitor"));
  // Any function that is handed the visitor: visitForGc(), jsgVisitForGc(), and helpers they call.
  // jsg's own implementation is left out.
  auto visitForGc = functionDecl(hasAnyParameter(hasType(references(gcVisitor))),
      unless(hasAncestor(namespaceDecl(hasName("::workerd::jsg")))));

  // `visitor.visit(*rc)`, `visitor.visit(rc->field)`.
  Finder->addMatcher(
      cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("visit", "visitAll"), ofClass(gcVisitor))),
          hasAnyArgument(expr(anyOf(derefShared, hasDescendant(derefShared)))),
          hasAncestor(visitForGc))
          .bind("visit"),
      this);
  // `rc->visitForGc(visitor)`.
  Finder->addMatcher(cxxMemberCallExpr(callee(cxxMethodDecl(hasName("visitForGc"))),
                         on(ignoringParenImpCasts(derefShared)), hasAncestor(visitForGc))
                         .bind("visit"),
      this);
}

void SharedVisitCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& Result) {
  if (const auto* visit = Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("visit")) {
    diag(visit->getExprLoc(),
        "visitForGc visits through a shared owner; another owner may visit the same handles, which "
        "are then strong or traced depending on which visited last")
        << visit->getSourceRange();
  }
}

}  // namespace workerd::clang_tidy
