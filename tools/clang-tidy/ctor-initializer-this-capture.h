// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Flags lambdas that capture `this` inside a constructor's mem-initializer list:
//
//   Foo::Foo(Timer& timer)
//       : task(timer.afterDelay(1 * kj::SECONDS).then([this]() { onTimeout(); })) {}
//
// While the initializer list runs, the object is only partially constructed: members declared
// after the one being initialized do not exist yet, the constructor body has not run, and a derived
// class's members are not constructed either. A lambda that holds onto `this` lets that
// partially constructed object escape, and nothing stops the lambda from running before
// construction finishes, or from running against an object whose constructor later throws.
// Creating the lambda in the constructor body instead keeps the capture after every member has
// been initialized.
//
// The check reports:
//   - explicit `this` and `*this` captures,
//   - implicit captures of `this` through a `[&]` or `[=]` capture default,
//   - init-captures of the object itself, such as `[self = this]` or `[&self = *this]`.
//
// Base-class, member, and delegating initializers are all checked; default member initializers
// are not, since they are part of the class definition rather than of a constructor.
//
// A lambda that is called immediately, `[&]() { ... }()`, is not reported: the capture does not
// outlive the initializer expression, so it is no more dangerous than naming the members
// directly. Lambdas nested inside such a call are still checked.
class CtorInitializerThisCaptureCheck: public clang::tidy::ClangTidyCheck {
 public:
  CtorInitializerThisCaptureCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  bool isLanguageVersionSupported(const clang::LangOptions& LangOpts) const override {
    return LangOpts.CPlusPlus11;
  }
  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;

 private:
  void checkExpr(const clang::Stmt* S, llvm::StringRef InitializedName);
  void checkLambda(const clang::LambdaExpr& Lambda, llvm::StringRef InitializedName);
};

}  // namespace workerd::clang_tidy
