// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "legacy-stream-alloc.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

namespace workerd::clang_tidy {

void LegacyStreamAllocCheck::registerMatchers(clang::ast_matchers::MatchFinder* Finder) {
  using namespace clang::ast_matchers;

  auto legacyStream =
      cxxRecordDecl(hasAnyName("::workerd::api::ReadableStream", "::workerd::api::WritableStream"))
          .bind("stream");

  // jsg::Lock::alloc<T>(), jsg::Lock::allocAccounted<T>() and the free function
  // jsg::alloc<T>() all take the allocated type as their first template argument;
  // the remaining arguments are the deduced constructor-parameter pack.
  auto allocOfLegacyStream =
      functionDecl(anyOf(cxxMethodDecl(hasAnyName("alloc", "allocAccounted"),
                             ofClass(cxxRecordDecl(hasName("::workerd::jsg::Lock")))),
                       hasName("::workerd::jsg::alloc")),
          hasTemplateArgument(0, refersToType(hasDeclaration(legacyStream))));

  // The compatibility-flag dispatch points. hasAncestor() walks through lambdas
  // defined inside these functions as well, so helpers written inline there are
  // covered.
  auto dispatchPoint = functionDecl(hasAnyName(
      "::workerd::api::JsReadableStream::create", "::workerd::api::JsWritableStream::create"));

  Finder->addMatcher(
      callExpr(callee(allocOfLegacyStream), unless(hasAncestor(dispatchPoint))).bind("call"), this);
}

void LegacyStreamAllocCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& Result) {
  const auto* call = Result.Nodes.getNodeAs<clang::CallExpr>("call");
  const auto* stream = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("stream");
  if (call == nullptr || stream == nullptr) return;

  llvm::StringRef streamName = stream->getName();
  llvm::StringRef replacement =
      streamName == "ReadableStream" ? "JsReadableStream::create()" : "JsWritableStream::create()";

  // Point at the `alloc` token so a NOLINT on that line suppresses the report;
  // the range covers the whole call for context.
  diag(call->getCallee()->getExprLoc(),
      "direct allocation of legacy %0; use %1, which selects the stream implementation "
      "by the typescript_implemented_streams compatibility flag")
      << streamName << replacement << call->getSourceRange();
}

}  // namespace workerd::clang_tidy
