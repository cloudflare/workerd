// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Flags a function handed the jsg::GcVisitor (visitForGc() or a helper it calls) that visits
// through a shared owner: kj::Rc<T>, kj::Arc<T>, or a kj::Own<T> whose T is kj::Refcounted or
// kj::AtomicRefcounted, as in `visitor.visit(*rc)`, `visitor.visit(rc->field)` or
// `rc->visitForGc(visitor)`.
//
// The other owners of the object may visit it too. A V8 handle visited from two owners is strong or
// traced depending on which visited last, and a jsg::Ref visited from two owners fails an
// assertion, so a shared object's handles have no single owner whose visits keep them alive.
//
// Any visit through a shared owner is flagged, since the visit is what shows the object holds
// handles. A deliberate visit, such as breaking a cycle that has a single visitor by design,
// carries `// NOLINT(jsg-shared-visit)` with the reason.
class SharedVisitCheck: public clang::tidy::ClangTidyCheck {
 public:
  SharedVisitCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace workerd::clang_tidy
