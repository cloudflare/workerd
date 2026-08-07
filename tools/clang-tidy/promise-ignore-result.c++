#include "promise-ignore-result.h"

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tidy;

namespace workerd::clang_tidy {

namespace {

// Source range covering the member access of `Call`, e.g. `.ignoreResult()` in
// `promise.ignoreResult()`, so that removing the range leaves the object
// expression behind. The range is invalid if the call can't be rewritten that
// way: an implicit or `->` member access doesn't leave a promise behind, and
// locations inside a macro can't be edited.
SourceRange memberAccessRange(const CXXMemberCallExpr &Call) {
  const auto *Member = dyn_cast<MemberExpr>(Call.getCallee()->IgnoreImplicit());
  if (Member == nullptr || Member->isArrow()) {
    return SourceRange();
  }
  SourceRange Range(Member->getOperatorLoc(), Call.getEndLoc());
  if (Range.getBegin().isInvalid() || Range.getBegin().isMacroID() ||
      Range.getEnd().isInvalid() || Range.getEnd().isMacroID()) {
    return SourceRange();
  }
  return Range;
}

// Location of the member name token of `Call`, e.g. `send` in
// `request.send()`. Invalid if the token can't be edited.
SourceLocation memberNameLoc(const CXXMemberCallExpr &Call) {
  const auto *Member = dyn_cast<MemberExpr>(Call.getCallee()->IgnoreImplicit());
  if (Member == nullptr || Member->getMemberLoc().isInvalid() ||
      Member->getMemberLoc().isMacroID()) {
    return SourceLocation();
  }
  return Member->getMemberLoc();
}

} // namespace

void PromiseIgnoreResultCheck::registerMatchers(MatchFinder *Finder) {
  // Match on the method's own class rather than on the type of the object
  // expression, so that calls through a class derived from kj::Promise are
  // matched too. capnp::RemotePromise, the type capnp::Request::send() returns,
  // is such a class: it derives from kj::Promise<capnp::Response<Results>>.
  auto promiseClass = classTemplateSpecializationDecl(hasName("::kj::Promise"));

  auto sendOfCapnpRequest = cxxMemberCallExpr(callee(cxxMethodDecl(
      hasName("send"),
      ofClass(classTemplateSpecializationDecl(hasName("::capnp::Request"))))));

  // capnp::Request::sendIgnoringResult() sends the same call without building
  // the typed response wrapper and pipeline that send() has to build only for
  // them to be thrown away, so it is preferred over send().ignoreResult()
  // whatever the resulting promise is used for. This is the more specific
  // diagnosis, hence the co_await matcher below excludes this shape.
  Finder->addMatcher(
      cxxMemberCallExpr(
          callee(cxxMethodDecl(hasName("ignoreResult"), ofClass(promiseClass))),
          on(expr(ignoringImplicit(sendOfCapnpRequest.bind("foldedSend")))))
          .bind("foldedCall"),
      this);

  // A co_await that drops its result doesn't need the typed response either, so
  // sendIgnoringResult() applies just as it does to send().ignoreResult(). The
  // result is dropped when the co_await is a statement of its own, which is an
  // ExprWithCleanups when the response has to be destroyed at the end of that
  // statement. Streaming methods aren't matched: their send() belongs to
  // capnp::StreamingRequest, which has no result to drop in the first place.
  Finder->addMatcher(
      coawaitExpr(
          has(expr(ignoringImplicit(sendOfCapnpRequest.bind("droppedSend")))),
          anyOf(hasParent(compoundStmt()),
                hasParent(exprWithCleanups(hasParent(compoundStmt()))))),
      this);

  // The operand of a co_await is a direct child of the CoawaitExpr, alongside
  // the implicit await_transform()/await_ready()/await_suspend()/await_resume()
  // subexpressions, none of which can call ignoreResult(). Matching the operand
  // rather than any descendant means chains that consume the kj::Promise<void>
  // for something other than the await, such as
  // `co_await promise.ignoreResult().catch_(handler)`, are left alone.
  // ignoringImplicit() sees through the temporary materialization that the
  // operand is wrapped in.
  Finder->addMatcher(
      coawaitExpr(has(expr(ignoringImplicit(
          cxxMemberCallExpr(
              callee(cxxMethodDecl(hasName("ignoreResult"),
                                   ofClass(promiseClass))),
              unless(on(expr(ignoringImplicit(sendOfCapnpRequest)))))
              .bind("awaitedCall"))))),
      this);
}

void PromiseIgnoreResultCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Folded = Result.Nodes.getNodeAs<CXXMemberCallExpr>("foldedCall");
  const auto *FoldedSend =
      Result.Nodes.getNodeAs<CXXMemberCallExpr>("foldedSend");
  if (Folded != nullptr && FoldedSend != nullptr) {
    reportSendFold(*Folded, *FoldedSend);
  }
  if (const auto *Send =
          Result.Nodes.getNodeAs<CXXMemberCallExpr>("droppedSend")) {
    reportDroppedSend(*Send);
  }
  if (const auto *Call =
          Result.Nodes.getNodeAs<CXXMemberCallExpr>("awaitedCall")) {
    reportAwaited(*Call);
  }
}

void PromiseIgnoreResultCheck::reportAwaited(const CXXMemberCallExpr &Call) {
  auto Diag = diag(Call.getCallee()->getExprLoc(),
                   "co_await discards the promise's result already, remove "
                   "ignoreResult()");
  Diag << Call.getSourceRange();

  SourceRange Removal = memberAccessRange(Call);
  if (Removal.isValid()) {
    Diag << FixItHint::CreateRemoval(CharSourceRange::getTokenRange(Removal));
  }
}

void PromiseIgnoreResultCheck::reportSendFold(const CXXMemberCallExpr &Call,
                                              const CXXMemberCallExpr &Send) {
  auto Diag = diag(Call.getCallee()->getExprLoc(),
                   "use sendIgnoringResult() instead of send().ignoreResult()");
  Diag << Call.getSourceRange();

  // Renaming send() without dropping ignoreResult() would leave code that
  // doesn't compile, so only offer the fix when both edits are possible.
  SourceRange Removal = memberAccessRange(Call);
  SourceLocation SendName = memberNameLoc(Send);
  if (Removal.isValid() && SendName.isValid()) {
    Diag << FixItHint::CreateReplacement(
                CharSourceRange::getTokenRange(SendName, SendName),
                "sendIgnoringResult")
         << FixItHint::CreateRemoval(CharSourceRange::getTokenRange(Removal));
  }
}

void PromiseIgnoreResultCheck::reportDroppedSend(
    const CXXMemberCallExpr &Send) {
  auto Diag = diag(Send.getCallee()->getExprLoc(),
                   "the awaited response is dropped, use sendIgnoringResult()");
  Diag << Send.getSourceRange();

  SourceLocation SendName = memberNameLoc(Send);
  if (SendName.isValid()) {
    Diag << FixItHint::CreateReplacement(
        CharSourceRange::getTokenRange(SendName, SendName),
        "sendIgnoringResult");
  }
}

} // namespace workerd::clang_tidy
