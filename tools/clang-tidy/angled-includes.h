#pragma once
#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Requires includes of kj, capnp and workerd headers to use angle brackets,
// e.g. `#include <kj/async.h>` rather than `#include "kj/async.h"`.
//
// Headers named with one of those directory prefixes are always found through
// the include path, so the quoted form is misleading: it additionally searches
// relative to the including file first. Quotes are reserved for headers in the
// same directory, which are written without any directory prefix at all.

class AngledIncludesCheck : public clang::tidy::ClangTidyCheck {
public:
  AngledIncludesCheck(clang::StringRef Name,
                      clang::tidy::ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerPPCallbacks(const clang::SourceManager &SM,
                           clang::Preprocessor *PP,
                           clang::Preprocessor *ModuleExpanderPP) override;
};

} // namespace workerd::clang_tidy
