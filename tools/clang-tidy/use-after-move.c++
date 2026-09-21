// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "use-after-move.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

namespace workerd::clang_tidy {
namespace {

using namespace clang::ast_matchers;

// A kj::mv() in one of these contexts is never evaluated, so it cannot
// invalidate its argument. This mirrors the exclusions in the upstream check.
AST_MATCHER(clang::Expr, hasUnevaluatedContext) {
  if (clang::isa<clang::CXXNoexceptExpr, clang::RequiresExpr>(Node)) return true;
  if (const auto* expression = clang::dyn_cast<clang::UnaryExprOrTypeTraitExpr>(&Node)) {
    return expression->getKind() == clang::UETT_SizeOf ||
        expression->getKind() == clang::UETT_AlignOf;
  }
  if (const auto* expression = clang::dyn_cast<clang::CXXTypeidExpr>(&Node)) {
    return !expression->isPotentiallyEvaluated();
  }
  return false;
}

StatementMatcher inUnevaluatedOrTemplateArgument() {
  return anyOf(hasAncestor(typeLoc()),
      hasAncestor(declRefExpr(to(functionDecl(isTemplateInstantiation())))),
      hasAncestor(expr(hasUnevaluatedContext())));
}

// KJ_CASE_ONEOF expands to a for loop that executes at most once. The generic
// analysis does not infer that the loop increment prevents another iteration.
bool isInsideKjOneOfCase(const clang::Expr& expression, clang::ASTContext& context) {
  clang::DynTypedNode node = clang::DynTypedNode::create(expression);
  while (true) {
    auto parents = context.getParents(node);
    if (parents.size() != 1) return false;
    if (const auto* statement = parents[0].get<clang::Stmt>()) {
      auto location = statement->getBeginLoc();
      if (location.isMacroID()) {
        auto macro = clang::Lexer::getImmediateMacroName(
            location, context.getSourceManager(), context.getLangOpts());
        if (macro == "KJ_CASE_ONEOF") return true;
      }
    }
    node = parents[0];
  }
}

}  // namespace

void UseAfterMoveCheck::registerMatchers(clang::ast_matchers::MatchFinder* finder) {
  // The inherited check() performs Clang's existing CFG and sequencing
  // analysis. It expects the node names bound by its own registerMatchers(), so
  // this matcher preserves that binding contract while matching only kj::mv().
  auto argument = declRefExpr().bind("arg");
  auto isMemberCallee = callee(functionDecl(unless(isStaticStorageClass())));
  auto derivedToBaseCast =
      implicitCastExpr(hasCastKind(clang::CK_DerivedToBase)).bind("optional-cast");
  auto containingConstructor = cxxConstructorDecl(
      hasAnyConstructorInitializer(withInitializer(expr(
          anyOf(equalsBoundNode("call-move"), hasDescendant(expr(equalsBoundNode("call-move")))))
                                                       .bind("containing-ctor-init"))))
                                   .bind("containing-ctor");
  auto containingContext =
      anyOf(hasAncestor(compoundStmt(hasParent(lambdaExpr().bind("containing-lambda")))),
          hasAncestor(
              functionDecl(anyOf(containingConstructor, functionDecl().bind("containing-func")))));
  auto optionalCast = optionally(anyOf(hasParent(derivedToBaseCast),
      hasArgument(0, traverse(clang::TK_AsIs, expr(hasParent(derivedToBaseCast))))));

  // kj::mv() is only an rvalue cast. The enclosing expression is the operation
  // that may actually consume the value, and is therefore where the inherited
  // analysis begins following control flow.
  auto move = callExpr(callee(functionDecl(hasName("::kj::mv")).bind("move-decl")),
      anyOf(cxxMemberCallExpr(isMemberCallee, on(argument)),
          callExpr(unless(cxxMemberCallExpr(isMemberCallee)), hasArgument(0, argument))),
      unless(inUnevaluatedOrTemplateArgument()), expr().bind("call-move"), optionalCast,
      containingContext);

  finder->addMatcher(traverse(clang::TK_AsIs,
                         stmt(forEach(expr(ignoringParenImpCasts(move))), unless(initListExpr()),
                             unless(expr(ignoringParenImpCasts(equalsBoundNode("call-move")))))
                             .bind("moving-call")),
      this);
}

void UseAfterMoveCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result) {
  const auto* move = result.Nodes.getNodeAs<clang::CallExpr>("call-move");
  const auto* argument = result.Nodes.getNodeAs<clang::DeclRefExpr>("arg");
  if (move == nullptr || argument == nullptr || result.Context == nullptr) return;
  if (isInsideKjOneOfCase(*move, *result.Context)) {
    return;
  }

  // All other kj::mv() calls use Clang's normal use-after-move dataflow,
  // including its reinitialization and sequencing rules.
  clang::tidy::bugprone::UseAfterMoveCheck::check(result);
}

}  // namespace workerd::clang_tidy
