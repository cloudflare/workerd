#pragma once
#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Reports superfluous calls to kj::Promise<T>::ignoreResult().
//
// ignoreResult() discards a promise's result by adding a continuation to the
// chain, which costs an allocation and some code size. That is superfluous in
// two cases:
//
// * The resulting promise is immediately awaited, as in
//   `co_await promise.ignoreResult()`. `co_await promise` discards the result
//   on its own, whatever its type, so the conversion to kj::Promise<void>
//   buys nothing.
//
// * The promise comes straight from capnp::Request::send(), which has
//   sendIgnoringResult() for this purpose. Here the call is reported wherever
//   the resulting promise is used, since sendIgnoringResult() also avoids
//   building the typed response wrapper and pipeline.
//
// The second case has a counterpart with no ignoreResult() call to point at:
// `co_await request.send();` as a statement of its own drops the response it
// just built, and is reported the same way.
//
// A call that isn't immediately awaited and isn't sending a capnp request does
// real work -- it converts the promise to kj::Promise<void> -- and is not
// reported. That includes chains such as
// `promise.ignoreResult().catch_(handler)`, where ignoreResult() is what allows
// the handler to return void.

class PromiseIgnoreResultCheck : public clang::tidy::ClangTidyCheck {
public:
  PromiseIgnoreResultCheck(clang::StringRef Name,
                           clang::tidy::ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override;
  void
  check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  void reportAwaited(const clang::CXXMemberCallExpr &Call);
  void reportSendFold(const clang::CXXMemberCallExpr &Call,
                      const clang::CXXMemberCallExpr &Send);
  void reportDroppedSend(const clang::CXXMemberCallExpr &Send);
};

} // namespace workerd::clang_tidy
