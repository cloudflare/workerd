//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Derived from LLVM's misc/CoroutineHostileRAIICheck.cpp. The ancestor walk is
// modified to cross declaration nodes without escaping the current function.

#include "coroutine-hostile-raii.h"

#include "clang-tidy/utils/OptionsUtils.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/Basic/AttrKinds.h"
#include "clang/Basic/DiagnosticIDs.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tidy;

namespace workerd::clang_tidy {
namespace {

using clang::ast_matchers::internal::BoundNodesTreeBuilder;

AST_MATCHER_P(Stmt, forEachPrevStmt,
    clang::ast_matchers::internal::Matcher<Stmt>, InnerMatcher) {
  DynTypedNode Child = DynTypedNode::create(Node);
  bool IsHostile = false;
  while (true) {
    auto Parents = Finder->getASTContext().getParents(Child);
    if (Parents.empty()) {
      break;
    }

    DynTypedNode Parent = *Parents.begin();
    if (Parent.get<FunctionDecl>() != nullptr) {
      break;
    }
    if (const auto *PCS = Parent.get<CompoundStmt>()) {
      const auto *ChildStmt = Child.get<Stmt>();
      if (ChildStmt != nullptr) {
        for (const Stmt *Sibling : PCS->children()) {
          if (Sibling == ChildStmt) {
            break;
          }
          BoundNodesTreeBuilder SiblingBuilder;
          if (InnerMatcher.matches(*Sibling, Finder, &SiblingBuilder)) {
            Builder->addMatch(SiblingBuilder);
            IsHostile = true;
          }
        }
      }
    }
    Child = Parent;
  }
  return IsHostile;
}

AST_MATCHER_P(CoawaitExpr, awaitable,
    clang::ast_matchers::internal::Matcher<Expr>, InnerMatcher) {
  if (const Expr *E = Node.getOperand()) {
    return InnerMatcher.matches(*E, Finder, Builder);
  }
  return false;
}

auto typeWithNameIn(const std::vector<StringRef> &Names) {
  return hasType(hasCanonicalType(hasDeclaration(namedDecl(hasAnyName(Names)))));
}

auto functionWithNameIn(const std::vector<StringRef> &Names) {
  auto Call = callExpr(callee(functionDecl(hasAnyName(Names))));
  return anyOf(expr(cxxBindTemporaryExpr(has(Call))), expr(Call));
}

} // namespace

CoroutineHostileRAIICheck::CoroutineHostileRAIICheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      RAIITypesList(utils::options::parseStringList(
          Options.get("RAIITypesList", "std::lock_guard;std::scoped_lock"))),
      AllowedAwaitablesList(utils::options::parseStringList(
          Options.get("AllowedAwaitablesList", ""))),
      AllowedCallees(utils::options::parseStringList(
          Options.get("AllowedCallees", ""))) {}

void CoroutineHostileRAIICheck::registerMatchers(MatchFinder *Finder) {
  auto ScopedLockable =
      varDecl(hasType(hasCanonicalType(
                  hasDeclaration(hasAttr(attr::Kind::ScopedLockable)))))
          .bind("scoped-lockable");
  auto OtherRAII = varDecl(typeWithNameIn(RAIITypesList)).bind("raii");
  auto AllowedSuspend = awaitable(anyOf(typeWithNameIn(AllowedAwaitablesList),
      functionWithNameIn(AllowedCallees)));
  Finder->addMatcher(
      expr(anyOf(coawaitExpr(unless(AllowedSuspend)), coyieldExpr()),
          forEachPrevStmt(
              declStmt(forEach(varDecl(anyOf(ScopedLockable, OtherRAII))))))
          .bind("suspension"),
      this);
}

void CoroutineHostileRAIICheck::check(
    const MatchFinder::MatchResult &Result) {
  if (const auto *VD = Result.Nodes.getNodeAs<VarDecl>("scoped-lockable")) {
    diag(VD->getLocation(),
        "%0 holds a lock across a suspension point of coroutine and could be "
        "unlocked by a different thread")
        << VD;
  }
  if (const auto *VD = Result.Nodes.getNodeAs<VarDecl>("raii")) {
    diag(VD->getLocation(), "%0 persists across a suspension point of coroutine")
        << VD;
  }
  if (const auto *Suspension = Result.Nodes.getNodeAs<Expr>("suspension")) {
    diag(Suspension->getBeginLoc(), "suspension point is here",
        DiagnosticIDs::Note);
  }
}

void CoroutineHostileRAIICheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "RAIITypesList",
      utils::options::serializeStringList(RAIITypesList));
  Options.store(Opts, "AllowedAwaitablesList",
      utils::options::serializeStringList(AllowedAwaitablesList));
  Options.store(Opts, "AllowedCallees",
      utils::options::serializeStringList(AllowedCallees));
}

} // namespace workerd::clang_tidy
