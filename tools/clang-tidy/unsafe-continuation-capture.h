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
//   - `kj::Weak<T>` / `kj::WeakRc<T>` / `workerd::WeakRef<T>` /
//     `jsg::WeakRef<T>` -- if the continuation should
//     no-op when the referent is gone
//   - `IoOwn<T>` / `IoPtr<T>` (via `IoContext::addObject()`)
//   - Capture a trivially-copyable value
//
// Lambdas invoked immediately (`[&]{ ... }()`) and lambdas passed to
// callees not on the async-sink list are not flagged. Neither are captures
// that provably cannot dangle:
//
//   - The chain is consumed within the frame: `co_await`ed, `.wait()`ed, or
//     owned by a local container or `kj::TaskSet` that never escapes. A
//     chain returned from a lambda passed to a result-owning sink
//     (`kj::Promise::then`, `kj::evalNow`, `KJ_MAP`, ...; extend with the
//     `ResultOwningSinks` option) is owned by that call's result.
//   - The referent has static storage duration, bound through a reference
//     whose initializer provably names a static in this translation unit.
//   - The chain is the promise the enclosing function returns, and the
//     capture is one of that function's reference, pointer or view
//     parameters. The KJ style guide requires callers to keep those alive
//     until the returned promise settles.
//
// Suppress intentional uses with `// NOLINT(workerd-unsafe-continuation-capture)`
// plus a comment explaining why the capture cannot dangle.

class UnsafeContinuationCaptureCheck: public clang::tidy::ClangTidyCheck {
 public:
  UnsafeContinuationCaptureCheck(clang::StringRef Name, clang::tidy::ClangTidyContext *Context);

  void registerMatchers(clang::ast_matchers::MatchFinder *Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;
  // Surface the check's options to
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
  std::string ResultOwningSinksRaw;
  // User-configurable extra fully-qualified async-sink function names.
  std::vector<std::string> ExtraSinks;
  // User-configurable extra owning-wrapper template names.
  std::vector<std::string> ExtraOwningTypes;
  // User-configurable extra synchronous-sink function names.
  std::vector<std::string> ExtraSyncSinks;
  // User-configurable extra result-owning-sink function names.
  std::vector<std::string> ExtraResultOwningSinks;
};

}  // namespace workerd::clang_tidy
