#pragma once
#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Flags lambdas passed to known asynchronous "continuation sinks" (e.g.
// `kj::Promise<T>::then`, `jsg::Promise<T>::then`, `IoContext::run`,
// `IoContext::addTask`, `IoContext::addFunctor`, `kj::evalLater`, etc.)
// that capture bare references, raw pointers, or other non-owning view
// types (`kj::StringPtr`, `kj::ArrayPtr<T>`, `jsg::Lock&`, `IoContext&`,
// ...) by reference or by value.
//
// Such captures are a well-known source of memory-corruption bugs in this
// codebase: by the time the continuation runs, the referent may have been
// destroyed (the surrounding stack frame may have returned, an enclosing
// coroutine may have been canceled, etc.). The safe alternatives are:
//
//   - `kj::Own<T>` / `kj::Rc<T>` / `kj::Arc<T>` -- transfer ownership
//   - `jsg::Ref<T>` (use `JSG_THIS` instead of `[this]` in JSG resource
//     types)
//   - `kj::WeakRef<T>` / `jsg::WeakRef<T>` -- if the continuation should
//     no-op when the referent is gone
//   - `IoOwn<T>` / `IoPtr<T>` (via `IoContext::addObject()`)
//   - Capture a trivially-copyable value
//
// Lambdas invoked immediately (`[&]{ ... }()`) and lambdas passed to
// callees not on the async-sink list are not flagged.
//
// Suppress intentional uses with `// NOLINT(workerd-unsafe-continuation-capture)`
// plus a comment explaining why the capture cannot dangle.

class UnsafeContinuationCaptureCheck : public clang::tidy::ClangTidyCheck {
public:
  UnsafeContinuationCaptureCheck(clang::StringRef Name,
                                 clang::tidy::ClangTidyContext *Context);

  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override;
  void
  check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
  // Surface the AsyncSinks / OwningCaptureTypes options to
  // `clang-tidy --dump-config` and to option serialization.
  void storeOptions(clang::tidy::ClangTidyOptions::OptionMap &Opts) override;
  // Clear the per-function use-map cache when each translation unit
  // finishes so that memory does not accumulate across runs sharing a
  // thread.
  void onEndOfTranslationUnit() override;

private:
  // Raw option strings (preserved verbatim for storeOptions()).
  std::string AsyncSinksRaw;
  std::string OwningCaptureTypesRaw;
  std::string SynchronousSinksRaw;
  // User-configurable extra fully-qualified async-sink function names.
  std::vector<std::string> ExtraSinks;
  // User-configurable extra owning-wrapper template names.
  std::vector<std::string> ExtraOwningTypes;
  // User-configurable extra synchronous-sink function names.
  std::vector<std::string> ExtraSyncSinks;
};

} // namespace workerd::clang_tidy
