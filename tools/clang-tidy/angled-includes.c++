#include "angled-includes.h"

#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

using namespace clang;
using namespace clang::tidy;

namespace workerd::clang_tidy {

namespace {

// Directory prefixes whose headers must be included with angle brackets.
constexpr llvm::StringRef AngledPrefixes[] = {"kj/", "capnp/", "workerd/"};

class AngledIncludesPPCallbacks : public PPCallbacks {
public:
  explicit AngledIncludesPPCallbacks(AngledIncludesCheck &Check)
      : Check(Check) {}

  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File, StringRef SearchPath,
                          StringRef RelativePath, const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override {
    if (IsAngled)
      return;

    for (llvm::StringRef Prefix : AngledPrefixes) {
      if (!FileName.starts_with(Prefix))
        continue;

      // FilenameRange covers the quotes as well as the file name, so replacing
      // it wholesale is enough to swap the quotes for angle brackets.
      Check.diag(FilenameRange.getBegin(),
                 "include of '%0' must use angle brackets")
          << FileName
          << FixItHint::CreateReplacement(FilenameRange,
                                          ("<" + FileName + ">").str());
      return;
    }
  }

private:
  AngledIncludesCheck &Check;
};

} // namespace

void AngledIncludesCheck::registerPPCallbacks(const SourceManager &,
                                              Preprocessor *PP,
                                              Preprocessor *) {
  PP->addPPCallbacks(std::make_unique<AngledIncludesPPCallbacks>(*this));
}

} // namespace workerd::clang_tidy
