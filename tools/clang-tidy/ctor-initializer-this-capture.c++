// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ctor-initializer-this-capture.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

using namespace clang;
using namespace clang::ast_matchers;

namespace workerd::clang_tidy {

namespace {

// The lambda called by `S`, if `S` calls a lambda expression directly, as in `[&]() { ... }()`.
const LambdaExpr* immediatelyInvokedLambda(const Stmt& S) {
  if (const auto* Call = dyn_cast<CXXOperatorCallExpr>(&S)) {
    if (Call->getOperator() == OO_Call && Call->getNumArgs() > 0) {
      return dyn_cast<LambdaExpr>(Call->getArg(0)->IgnoreUnlessSpelledInSource());
    }
    return nullptr;
  }
  // Inside a template, a call to a lambda can stay an unresolved CallExpr whose callee is the
  // lambda itself.
  if (const auto* Call = dyn_cast<CallExpr>(&S)) {
    if (const Expr* Callee = Call->getCallee()) {
      return dyn_cast<LambdaExpr>(Callee->IgnoreUnlessSpelledInSource());
    }
  }
  return nullptr;
}

// Whether `Init` evaluates to the object under construction itself: `this` or `*this`.
bool isThisObject(const Expr* Init) {
  if (Init == nullptr) return false;
  const Expr* E = Init->IgnoreUnlessSpelledInSource();
  if (const auto* Deref = dyn_cast<UnaryOperator>(E)) {
    if (Deref->getOpcode() != UO_Deref) return false;
    E = Deref->getSubExpr()->IgnoreUnlessSpelledInSource();
  }
  return isa<CXXThisExpr>(E);
}

// Describes what `Init` initializes, for the diagnostic: a member name or a base class type.
std::string initializedName(const CXXCtorInitializer& Init, const ASTContext& Context) {
  if (const FieldDecl* Member = Init.getAnyMember()) {
    return Member->getNameAsString();
  }
  if (const TypeSourceInfo* Type = Init.getTypeSourceInfo()) {
    return Type->getType().getAsString(Context.getPrintingPolicy());
  }
  return "<unknown>";
}

}  // namespace

void CtorInitializerThisCaptureCheck::registerMatchers(MatchFinder* Finder) {
  // Implicit initializers cannot contain a lambda written in the constructor, so only written ones
  // are inspected. Template patterns and their instantiations are both matched, since a capture
  // default may only resolve to a `this` capture once instantiated; clang-tidy merges the
  // resulting duplicate diagnostics.
  Finder->addMatcher(cxxConstructorDecl(forEachConstructorInitializer(
                         cxxCtorInitializer(isWritten()).bind("init"))),
      this);
}

void CtorInitializerThisCaptureCheck::check(const MatchFinder::MatchResult& Result) {
  const auto* Init = Result.Nodes.getNodeAs<CXXCtorInitializer>("init");
  if (Init == nullptr || Init->getInit() == nullptr) return;
  checkExpr(Init->getInit(), initializedName(*Init, *Result.Context));
}

void CtorInitializerThisCaptureCheck::checkExpr(const Stmt* S, llvm::StringRef InitializedName) {
  if (S == nullptr) return;

  if (const LambdaExpr* Invoked = immediatelyInvokedLambda(*S)) {
    // The lambda does not outlive the call, so its own captures are harmless, but a lambda it
    // creates may still escape. Its capture initializers and the call's arguments are evaluated in
    // the initializer, too.
    for (const Stmt* Child: S->children()) {
      const auto* ChildExpr = dyn_cast_or_null<Expr>(Child);
      if (ChildExpr != nullptr && ChildExpr->IgnoreUnlessSpelledInSource() == Invoked) {
        for (const Stmt* LambdaChild: Invoked->children()) {
          checkExpr(LambdaChild, InitializedName);
        }
      } else {
        checkExpr(Child, InitializedName);
      }
    }
    return;
  }

  if (const auto* Lambda = dyn_cast<LambdaExpr>(S)) {
    checkLambda(*Lambda, InitializedName);
    return;
  }

  for (const Stmt* Child: S->children()) {
    checkExpr(Child, InitializedName);
  }
}

void CtorInitializerThisCaptureCheck::checkLambda(
    const LambdaExpr& Lambda, llvm::StringRef InitializedName) {
  for (const LambdaCapture& Capture: Lambda.captures()) {
    bool CapturesObject = Capture.capturesThis();
    if (!CapturesObject && Capture.capturesVariable()) {
      const auto* Var = dyn_cast<VarDecl>(Capture.getCapturedVar());
      CapturesObject = Var != nullptr && Var->isInitCapture() && isThisObject(Var->getInit());
    }
    if (!CapturesObject) continue;

    if (Capture.isImplicit()) {
      diag(Lambda.getCaptureDefaultLoc(),
          "lambda in the initializer of '%0' implicitly captures 'this' through its capture "
          "default before the object is fully constructed; create the lambda in the constructor "
          "body instead")
          << InitializedName << Lambda.getIntroducerRange();
    } else {
      diag(Capture.getLocation(),
          "lambda in the initializer of '%0' captures 'this' before the object is fully "
          "constructed; create the lambda in the constructor body instead")
          << InitializedName << Lambda.getIntroducerRange();
    }
    // One diagnostic per lambda. Any lambda nested in this one that captures `this` needs this
    // one to capture it too, so it adds nothing.
    return;
  }

  // A lambda that does not capture `this` cannot contain one that does, but its capture
  // initializers run in the constructor initializer and may create one.
  for (const LambdaCapture& Capture: Lambda.captures()) {
    if (!Capture.capturesVariable()) continue;
    if (const auto* Var = dyn_cast<VarDecl>(Capture.getCapturedVar());
        Var != nullptr && Var->isInitCapture()) {
      checkExpr(Var->getInit(), InitializedName);
    }
  }
}

}  // namespace workerd::clang_tidy
