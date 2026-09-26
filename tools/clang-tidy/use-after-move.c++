// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "use-after-move.h"

#include "clang-tidy/utils/OptionsUtils.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "use-after-move-analysis.h"

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

bool isOneShotKjCase(const clang::ForStmt& loop, clang::ASTContext& context) {
  auto location = loop.getForLoc();
  if (!location.isMacroID() ||
      clang::Lexer::getImmediateMacroName(
          location, context.getSourceManager(), context.getLangOpts()) != "KJ_CASE_ONEOF") {
    return false;
  }

  // Recognize the generated sentinel test and unconditional sentinel reset. A user-written loop
  // inside the case has a different for-keyword location and must retain its back edge.
  if (loop.getCond() == nullptr || loop.getInc() == nullptr) return false;
  const auto* condition =
      clang::dyn_cast<clang::DeclRefExpr>(loop.getCond()->IgnoreParenImpCasts());
  const auto* increment = clang::dyn_cast<clang::BinaryOperator>(loop.getInc());
  if (condition == nullptr || increment == nullptr || increment->getOpcode() != clang::BO_Assign) {
    return false;
  }
  const auto* assigned =
      clang::dyn_cast<clang::DeclRefExpr>(increment->getLHS()->IgnoreParenImpCasts());
  return assigned != nullptr && assigned->getDecl() == condition->getDecl() &&
      condition->getType()->isPointerType() &&
      increment->getRHS()->isNullPointerConstant(context, clang::Expr::NPC_ValueDependentIsNotNull);
}

void normalizeKjCaseLoops(clang::CFG& cfg, clang::ASTContext& context) {
  for (auto* block: cfg) {
    const auto* loop = clang::dyn_cast_or_null<clang::ForStmt>(block->getLoopTarget());
    if (loop == nullptr || !isOneShotKjCase(*loop, context) || block->succ_size() != 1) continue;
    auto* condition = block->succ_begin()->getReachableBlock();
    if (condition == nullptr || condition->getTerminatorStmt() != loop ||
        condition->succ_size() != 2) {
      continue;
    }

    // CFGBuilder puts the true (body) successor first and the false (exit) successor second.
    // After the increment clears the sentinel, only the exit is reachable. Redirect this edge
    // instead of dropping it: code after the case and enclosing real loops still need analysis.
    auto exit = *(condition->succ_begin() + 1);
    if (exit.getReachableBlock() == nullptr) continue;
    for (auto& predecessor: condition->preds()) {
      if (predecessor.getReachableBlock() == block) {
        predecessor = clang::CFGBlock::AdjacentBlock(nullptr, true);
      }
    }
    *block->succ_begin() = clang::CFGBlock::AdjacentBlock(nullptr, true);
    block->addSuccessor(exit, cfg.getBumpVectorContext());
    block->setLoopTarget(nullptr);
  }
}

// Moving a derived object into its base move constructor leaves fields declared by the derived
// class untouched. Clang 22's analysis does not make this distinction.
bool isDirectDerivedFieldUse(const clang::DeclRefExpr& expression,
    const clang::CXXRecordDecl& derived,
    clang::ASTContext& context) {
  clang::DynTypedNode node = clang::DynTypedNode::create(expression);
  while (true) {
    auto parents = context.getParents(node);
    if (parents.size() != 1) return false;
    if (const auto* member = parents[0].get<clang::MemberExpr>()) {
      const auto* field = clang::dyn_cast<clang::FieldDecl>(member->getMemberDecl());
      return field != nullptr && field->getParent() == &derived;
    }
    const auto* parent = parents[0].get<clang::Expr>();
    if (parent == nullptr || !clang::isa<clang::ImplicitCastExpr, clang::ParenExpr>(parent)) {
      return false;
    }
    node = parents[0];
  }
}

bool hasNonDerivedFieldUse(const clang::Stmt& statement,
    const clang::ValueDecl& moved,
    const clang::CXXRecordDecl& derived,
    clang::ASTContext& context) {
  if (const auto* reference = clang::dyn_cast<clang::DeclRefExpr>(&statement)) {
    return reference->getDecl() == &moved && !isDirectDerivedFieldUse(*reference, derived, context);
  }
  for (const auto* child: statement.children()) {
    if (child != nullptr && hasNonDerivedFieldUse(*child, moved, derived, context)) return true;
  }
  return false;
}

bool onlyUsesDirectDerivedFieldsAfterBaseMove(
    const clang::ast_matchers::MatchFinder::MatchResult& result,
    const clang::DeclRefExpr& argument) {
  const auto* parentCast = result.Nodes.getNodeAs<clang::ImplicitCastExpr>("optional-cast");
  const auto* constructor = result.Nodes.getNodeAs<clang::CXXConstructorDecl>("containing-ctor");
  const auto* movingInitializer = result.Nodes.getNodeAs<clang::Expr>("containing-ctor-init");
  if (parentCast == nullptr || constructor == nullptr || movingInitializer == nullptr ||
      result.Context == nullptr) {
    return false;
  }

  bool afterMove = false;
  for (const auto* initializer: constructor->inits()) {
    if (!afterMove &&
        initializer->getInit()->IgnoreImplicit() == movingInitializer->IgnoreImplicit()) {
      afterMove = true;
      continue;
    }
    if (afterMove &&
        hasNonDerivedFieldUse(*initializer->getInit(), *argument.getDecl(),
            *constructor->getParent(), *result.Context)) {
      return false;
    }
  }
  return !hasNonDerivedFieldUse(
      *constructor->getBody(), *argument.getDecl(), *constructor->getParent(), *result.Context);
}

}  // namespace

void UseAfterMoveCheck::registerMatchers(clang::ast_matchers::MatchFinder* finder) {
  // LLVM's check performs its existing CFG and sequencing
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
  if (onlyUsesDirectDerivedFieldsAfterBaseMove(result, *argument)) return;

  auto invalidationFunctions =
      clang::tidy::utils::options::parseStringList(Options.get("InvalidationFunctions", ""));
  auto reinitializationFunctions =
      clang::tidy::utils::options::parseStringList(Options.get("ReinitializationFunctions", ""));
  detail::checkUseAfterMove(result, *this, invalidationFunctions, reinitializationFunctions,
      [&](clang::CFG& cfg) { normalizeKjCaseLoops(cfg, *result.Context); });
}

}  // namespace workerd::clang_tidy
