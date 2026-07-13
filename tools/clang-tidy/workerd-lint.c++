// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Workerd clang-tidy plugin module registration.
//
// This file registers all workerd-specific clang-tidy checks into the
// "workerd-lint" module. The checks themselves are implemented in separate
// files:
//   - consume.c++: workerd-consume check
//   - visit-for-gc.c++: jsg-visit-for-gc check
//   - unsafe-continuation-capture.c++: workerd-unsafe-continuation-capture check

#include "clang-tidy/ClangTidyModule.h"

#include "consume.h"
#include "unsafe-continuation-capture.h"
#include "visit-for-gc.h"

namespace workerd {
namespace clang_tidy {

class WorkerdLintModule : public clang::tidy::ClangTidyModule {
 public:
  void addCheckFactories(clang::tidy::ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<VisitForGcCheck>("jsg-visit-for-gc");
    CheckFactories.registerCheck<ConsumeCheck>("workerd-consume");
    CheckFactories.registerCheck<UnsafeContinuationCaptureCheck>(
        "workerd-unsafe-continuation-capture");
  }
};

static clang::tidy::ClangTidyModuleRegistry::Add<WorkerdLintModule>
    X("workerd-lint", "Workerd static checks.");

}  // namespace clang_tidy
}  // namespace workerd
