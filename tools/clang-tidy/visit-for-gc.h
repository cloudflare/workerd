// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "clang/AST/DeclCXX.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace workerd {
namespace clang_tidy {

// Clang-tidy check that validates JSG resource types correctly visit their
// GC roots. Flags fields of visitable types (jsg::Ref, jsg::V8Ref, jsg::JsRef,
// jsg::Function, jsg::Promise, jsg::BufferSource, jsg::Value, etc., plus
// kj::Maybe/Array/Vector/OneOf and jsg::Optional wrappers thereof) that are
// not visited in the class's visitForGc() method.
//
// This check helps prevent GC-related bugs where JavaScript objects are
// prematurely collected because the C++ side failed to mark them as reachable.
class VisitForGcCheck : public clang::tidy::ClangTidyCheck {
 public:
  VisitForGcCheck(clang::StringRef Name, clang::tidy::ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override;
  void onStartOfTranslationUnit() override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
  void onEndOfTranslationUnit() override;

 private:
  // Records we encountered in this TU and need to evaluate after the field-
  // collection pass completes.
  llvm::SmallVector<const clang::CXXRecordDecl *, 64> records_;

  // CanonicalDecl* of records that appear as a field type somewhere in this
  // TU. We use canonical decls so forward-declared types and their definitions
  // alias; declaration redeclarations point at the same canonical decl.
  llvm::DenseSet<const clang::CXXRecordDecl *> usedAsField_;

  // Records whose holder's visitForGc body is visible in this TU. A struct
  // qualifies for the "used as field" diagnostic only when its holder is
  // analyzable here; otherwise we'd false-positive in every TU that includes
  // the header but not the holder's defining .c++. CanonicalDecl* keys.
  llvm::DenseSet<const clang::CXXRecordDecl *> holderVisitForGcVisible_;

  // (FieldDecl canonical, ...) pairs marking that some outer holder's
  // visitForGc body transitively reaches the given field via a member-access
  // chain (e.g., `visitor.visit(s.func)` where `s` is a struct field).
  // Used to suppress diagnostics on nested structs whose visitable fields are
  // already covered by an enclosing record's visitForGc.
  llvm::DenseSet<const clang::FieldDecl *> transitivelyVisitedFields_;

  const clang::SourceManager *sourceManager_ = nullptr;

  void recordUsedAsField(clang::QualType qt);
  void checkRecord(const clang::CXXRecordDecl *record);
  void collectVisitedFields(const clang::Stmt *stmt,
                            llvm::DenseSet<const clang::FieldDecl *> &visitedFields);

  // True if `record` declares its own visitForGc method or transitively
  // inherits one.
  static bool baseHasVisitForGc(const clang::CXXRecordDecl *record);
  static bool baseHasVisitForGcImpl(
      const clang::CXXRecordDecl *record,
      llvm::DenseSet<const clang::CXXRecordDecl *> &visited);
};

}  // namespace clang_tidy
}  // namespace workerd
