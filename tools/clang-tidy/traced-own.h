// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Flags the two ways an object's handles can be left traced with nothing visiting them:
//
// - A function handed the jsg::GcVisitor (visitForGc() or a helper it calls) that visits through a
//   kj::Own<T>, as in `visitor.visit(*own)` or `visitor.visit(own->field)`. Moving the Own moves
//   the pointer but not the handles behind it, so an object moved out of the owner that visited it
//   keeps traced handles that nothing visits, and the GC can free their targets while the object
//   still uses them. jsg::TracedOwn<T> makes the handles strong again when it moves.
// - Moving a whole container field whose elements hold a jsg::TracedOwn<T>, directly or inside a
//   struct, kj::Maybe or kj::OneOf, as in `auto all = kj::mv(pending)` on a
//   `kj::Vector<jsg::TracedOwn<T>>`. Moving a container moves none of its elements, so their
//   handles stay traced; move the elements out one at a time.
//
// Sites that are known to be safe carry `// NOLINT(jsg-traced-own)` with the reason.
class TracedOwnCheck: public clang::tidy::ClangTidyCheck {
 public:
  TracedOwnCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace workerd::clang_tidy
