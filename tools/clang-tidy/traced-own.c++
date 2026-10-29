// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "traced-own.h"

#include "clang/AST/DeclCXX.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"

namespace workerd::clang_tidy {
namespace {

// Whether a value of `type` holds a jsg::TracedOwn by value: is one, or has one among its fields,
// its bases, or the alternatives of a kj::Maybe or kj::OneOf. What a pointer field points to, as
// in kj::Own, jsg::Ref or jsg::WeakRef, is not followed: moving the pointer does not move it.
bool holdsTracedOwn(clang::QualType type, llvm::SmallPtrSetImpl<const clang::Type*>& seen) {
  const auto* canonical = type.getCanonicalType().getTypePtrOrNull();
  if (canonical == nullptr || !seen.insert(canonical).second) return false;
  const auto* record = canonical->getAsCXXRecordDecl();
  if (record == nullptr) return false;
  if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
    auto name = spec->getQualifiedNameAsString();
    if (name == "workerd::jsg::TracedOwn") return true;
    // kj::OneOf keeps its alternatives in raw storage, so they are not among its fields.
    if (name == "kj::Maybe" || name == "kj::OneOf" || name == "std::optional" ||
        name == "std::variant") {
      for (const auto& arg: spec->getTemplateArgs().asArray()) {
        auto args = arg.getKind() == clang::TemplateArgument::Pack ? arg.getPackAsArray()
                                                                   : llvm::ArrayRef(arg);
        for (const auto& alt: args) {
          if (alt.getKind() == clang::TemplateArgument::Type &&
              holdsTracedOwn(alt.getAsType(), seen)) {
            return true;
          }
        }
      }
    }
  }
  if (!record->hasDefinition()) return false;
  for (const auto* field: record->fields()) {
    if (holdsTracedOwn(field->getType(), seen)) return true;
  }
  for (const auto& base: record->bases()) {
    if (holdsTracedOwn(base.getType(), seen)) return true;
  }
  return false;
}

AST_MATCHER(clang::QualType, holdsTracedOwnElement) {
  llvm::SmallPtrSet<const clang::Type*, 16> seen;
  return holdsTracedOwn(Node, seen);
}

}  // namespace

void TracedOwnCheck::registerMatchers(clang::ast_matchers::MatchFinder* Finder) {
  using namespace clang::ast_matchers;

  auto own = classTemplateSpecializationDecl(hasName("::kj::Own"));
  auto ownExpr = expr(hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(own)))));
  auto derefOwn = cxxOperatorCallExpr(
      hasAnyOverloadedOperatorName("*", "->"), hasArgument(0, ignoringParenImpCasts(ownExpr)));

  auto gcVisitor = cxxRecordDecl(hasName("::workerd::jsg::GcVisitor"));
  // Any function that is handed the visitor: visitForGc(), jsgVisitForGc(), and helpers they call.
  // jsg's own types, TracedOwn among them, are the implementation this check points to.
  auto visitForGc = functionDecl(hasAnyParameter(hasType(references(gcVisitor))),
      unless(hasAncestor(namespaceDecl(hasName("::workerd::jsg")))));

  // `visitor.visit(*own)`, `visitor.visit(own->field)`.
  Finder->addMatcher(
      cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("visit", "visitAll"), ofClass(gcVisitor))),
          hasAnyArgument(expr(anyOf(derefOwn, hasDescendant(derefOwn)))), hasAncestor(visitForGc))
          .bind("visit"),
      this);
  // `own->visitForGc(visitor)`.
  Finder->addMatcher(cxxMemberCallExpr(callee(cxxMethodDecl(hasName("visitForGc"))),
                         on(ignoringParenImpCasts(derefOwn)), hasAncestor(visitForGc))
                         .bind("visit"),
      this);

  // Containers whose move transfers their storage without moving the elements, when an element
  // holds a TracedOwn directly or inside a struct or wrapper. Moving a single-value wrapper such as
  // kj::Maybe or kj::OneOf moves the TracedOwn itself, which is safe.
  auto holdsTracedOwn = classTemplateSpecializationDecl(
      hasAnyName("::kj::Vector", "::kj::Array", "::kj::Table", "::workerd::RingBuffer",
          "::std::vector", "::std::deque", "::std::list"),
      hasAnyTemplateArgument(refersToType(holdsTracedOwnElement())));

  // `kj::mv(field)` / `std::move(field)` of a container of TracedOwns.
  Finder->addMatcher(
      callExpr(callee(functionDecl(hasAnyName("::kj::mv", "::std::move"))), argumentCountIs(1),
          hasArgument(0,
              ignoringParenImpCasts(memberExpr(hasType(
                  hasUnqualifiedDesugaredType(recordType(hasDeclaration(holdsTracedOwn))))))))
          .bind("move"),
      this);
}

void TracedOwnCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& Result) {
  if (const auto* visit = Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("visit")) {
    diag(visit->getExprLoc(),
        "visitForGc visits through a kj::Own; moving the Own leaves the object's handles traced "
        "with nothing visiting them, so hold it in a jsg::TracedOwn")
        << visit->getSourceRange();
    return;
  }
  if (const auto* move = Result.Nodes.getNodeAs<clang::CallExpr>("move")) {
    diag(move->getExprLoc(),
        "moving a container of jsg::TracedOwn moves none of its elements, so their handles stay "
        "traced; move the elements out one at a time")
        << move->getSourceRange();
  }
}

}  // namespace workerd::clang_tidy
