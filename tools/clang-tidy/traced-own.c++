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

// Whether `fn` returns true for any type a `record` holds by value: its fields, its bases, and the
// alternatives of a kj::Maybe or kj::OneOf. What a pointer field points to, as in kj::Own,
// jsg::Ref or jsg::WeakRef, is not followed: moving the pointer does not move it.
template <typename Fn>
bool anyHeldType(const clang::CXXRecordDecl* record, Fn&& fn) {
  if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
    auto name = spec->getQualifiedNameAsString();
    // kj::OneOf keeps its alternatives in raw storage, so they are not among its fields.
    if (name == "kj::Maybe" || name == "kj::OneOf" || name == "std::optional" ||
        name == "std::variant") {
      for (const auto& arg: spec->getTemplateArgs().asArray()) {
        auto args = arg.getKind() == clang::TemplateArgument::Pack ? arg.getPackAsArray()
                                                                   : llvm::ArrayRef(arg);
        for (const auto& alt: args) {
          if (alt.getKind() == clang::TemplateArgument::Type && fn(alt.getAsType())) {
            return true;
          }
        }
      }
    }
  }
  if (!record->hasDefinition()) return false;
  for (const auto* field: record->fields()) {
    if (fn(field->getType())) return true;
  }
  for (const auto& base: record->bases()) {
    if (fn(base.getType())) return true;
  }
  return false;
}

const clang::CXXRecordDecl* unvisitedRecord(
    clang::QualType type, llvm::SmallPtrSetImpl<const clang::Type*>& seen) {
  const auto* canonical = type.getCanonicalType().getTypePtrOrNull();
  if (canonical == nullptr || !seen.insert(canonical).second) return nullptr;
  return canonical->getAsCXXRecordDecl();
}

bool isContainer(const clang::ClassTemplateSpecializationDecl* spec) {
  auto name = spec->getQualifiedNameAsString();
  return name == "kj::Vector" || name == "kj::Array" || name == "kj::Table" ||
      name == "workerd::RingBuffer" || name == "std::vector" || name == "std::deque" ||
      name == "std::list";
}

template <typename Fn>
bool anyTemplateType(const clang::ClassTemplateSpecializationDecl* spec, Fn&& fn) {
  for (const auto& arg: spec->getTemplateArgs().asArray()) {
    if (arg.getKind() == clang::TemplateArgument::Type && fn(arg.getAsType())) return true;
  }
  return false;
}

// Whether a value of `type` holds a jsg::TracedOwn by value, or a container of elements that do.
bool holdsTracedOwn(clang::QualType type, llvm::SmallPtrSetImpl<const clang::Type*>& seen) {
  const auto* record = unvisitedRecord(type, seen);
  if (record == nullptr) return false;
  auto recurse = [&](clang::QualType held) { return holdsTracedOwn(held, seen); };
  if (const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
    if (spec->getQualifiedNameAsString() == "workerd::jsg::TracedOwn") return true;
    if (isContainer(spec) && anyTemplateType(spec, recurse)) return true;
  }
  return anyHeldType(record, recurse);
}

// Containers whose move transfers their storage without moving the elements, when an element
// holds a TracedOwn. Moving a single-value wrapper such as kj::Maybe or kj::OneOf moves the
// TracedOwn itself, which is safe.
bool isTracedOwnContainer(const clang::CXXRecordDecl* record) {
  const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record);
  if (spec == nullptr || !isContainer(spec)) return false;
  return anyTemplateType(spec, [](clang::QualType element) {
    llvm::SmallPtrSet<const clang::Type*, 16> seen;
    return holdsTracedOwn(element, seen);
  });
}

// Whether a value of `type` is a container of TracedOwns or holds one by value, as a struct with
// such a field, or a kj::HashMap whose values hold one, does.
bool holdsTracedOwnContainer(
    clang::QualType type, llvm::SmallPtrSetImpl<const clang::Type*>& seen) {
  const auto* record = unvisitedRecord(type, seen);
  if (record == nullptr) return false;
  if (isTracedOwnContainer(record)) return true;
  return anyHeldType(
      record, [&](clang::QualType held) { return holdsTracedOwnContainer(held, seen); });
}

AST_MATCHER(clang::QualType, tracedOwnContainerType) {
  llvm::SmallPtrSet<const clang::Type*, 16> seen;
  return holdsTracedOwnContainer(Node, seen);
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

  // A container of TracedOwns, or a value that holds one, that some owner may have visited. A
  // non-reference local was filled by the function that moves it, so it is left out.
  auto local =
      declRefExpr(to(varDecl(hasAutomaticStorageDuration(), unless(hasType(referenceType())))));
  auto visitedContainer =
      expr(hasType(qualType(tracedOwnContainerType())), unless(ignoringParenImpCasts(local)));
  // RingBuffer and StateMachine move their own storage, which stays inside the same owner.
  auto withinOwnStorage =
      hasAncestor(cxxRecordDecl(hasAnyName("::workerd::RingBuffer", "::workerd::StateMachine")));

  // `kj::mv(field)`, `kj::mv(state.requireActiveUnsafe())`, `kj::mv(ref)`.
  Finder->addMatcher(
      callExpr(callee(functionDecl(hasAnyName("::kj::mv", "::std::move"))), argumentCountIs(1),
          hasArgument(0, visitedContainer), unless(withinOwnStorage))
          .bind("move"),
      this);
  // `std::swap(a, b)`.
  Finder->addMatcher(callExpr(callee(functionDecl(hasAnyName("::kj::swap", "::std::swap"))),
                         hasAnyArgument(visitedContainer), unless(withinOwnStorage))
                         .bind("swap"),
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
        "moving a container of jsg::TracedOwn, or a value that holds one, moves none of its "
        "elements, so their handles stay traced; move the elements out one at a time")
        << move->getSourceRange();
    return;
  }
  if (const auto* swap = Result.Nodes.getNodeAs<clang::CallExpr>("swap")) {
    diag(swap->getExprLoc(),
        "swapping a container of jsg::TracedOwn, or a value that holds one, moves none of its "
        "elements, so their handles stay traced; move the elements one at a time")
        << swap->getSourceRange();
  }
}

}  // namespace workerd::clang_tidy
