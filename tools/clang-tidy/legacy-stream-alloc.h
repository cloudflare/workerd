// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace workerd::clang_tidy {

// Flags direct allocation of the legacy C++ ReadableStream and WritableStream:
// `js.alloc<ReadableStream>(...)`, `js.allocAccounted<WritableStream>(...)` and
// the free-function form `jsg::alloc<ReadableStream>(...)`.
//
// workerd carries two implementations of the web streams: the legacy C++ one in
// src/workerd/api/streams/ and a TypeScript one, selected per worker by the
// typescript_implemented_streams compatibility flag. JsReadableStream::create()
// and JsWritableStream::create() are the dispatch points that pick the
// implementation, so they are the only functions permitted to allocate the
// legacy types directly; anywhere else, a direct allocation hands out a legacy
// stream regardless of the flag.
//
// Allocations lexically inside those two functions (including in lambdas
// defined there) are not reported. The legacy implementation's own internals
// (tee(), detach(), its JS constructors, ...) necessarily allocate legacy
// streams too, as do tests that exercise the legacy implementation directly;
// those sites carry `// NOLINT(workerd-legacy-stream-alloc)` together with a
// comment explaining why the allocation has to be legacy.
class LegacyStreamAllocCheck: public clang::tidy::ClangTidyCheck {
 public:
  LegacyStreamAllocCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(clang::ast_matchers::MatchFinder* Finder) override;
  void check(const clang::ast_matchers::MatchFinder::MatchResult& Result) override;
};

}  // namespace workerd::clang_tidy
