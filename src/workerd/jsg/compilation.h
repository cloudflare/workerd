// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/code-cache.h>

#include <kj/function.h>
#include <kj/mutex.h>

namespace workerd::jsg {

class Lock;

// Records the positions of functions compiled while the module runs. Producing hints bypasses
// bytecode caches because V8 cannot record hints while consuming cached code.
WD_STRONG_BOOL(ProduceCompileHints);

// A bytecode slot shared by replicas of one module definition. Used only when the module has
// neither embedded bytecode nor an applicable process-wide store. Keep this with the shared
// definition, never with the isolate-local module instance. Every call must use the same source.
class SharedModuleCompilationCache {
 public:
  // Compiles using the slot, regenerating rejected bytecode. Snapshot preparation consumes
  // existing bytecode but never generates it, because V8 forbids serialization in that phase.
  v8::MaybeLocal<v8::Module> compile(Lock& js,
      v8::Local<v8::String> source,
      const v8::ScriptOrigin& origin,
      const CompilationObserver& observer) const;

 private:
  struct State {
    kj::Maybe<kj::Own<v8::ScriptCompiler::CachedData>> data;
    // Distinguishes replacements even if the allocator reuses a rejected entry's address.
    uint64_t generation = 0;
  };
  kj::MutexGuarded<State> cachedData;

  friend class SharedModuleCompilationCacheTestAccess;
};

struct ModuleCompilationOptions {
  kj::ArrayPtr<const kj::byte> embeddedCache = nullptr;
  kj::ArrayPtr<const int> compileHints = nullptr;
  ProduceCompileHints produceHints = ProduceCompileHints::NO;
  kj::Maybe<const SharedModuleCompilationCache&> sharedCache;
};

// The compilation boundary for both module registries. Chooses hint production, embedded
// bytecode, the process-wide store, a shared definition's slot, then hints or ordinary source
// compilation, in that order. The key callback runs only when a store is installed and needed;
// it may return none for sources that must use their shared slot instead.
v8::MaybeLocal<v8::Module> compileModule(Lock& js,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer,
    kj::FunctionParam<kj::Maybe<CodeCacheKey>(IsolateCodeCache&)> getCacheKey,
    const ModuleCompilationOptions& options = {});

// CommonJS evaluation functions have no parameters and at most one context extension. The
// receiving isolate supplies that extension when consuming bytecode from another isolate.
v8::MaybeLocal<v8::Function> compileFunction(Lock& js,
    kj::StringPtr source,
    const v8::ScriptOrigin& origin,
    kj::Maybe<v8::Local<v8::Object>> contextExtension,
    const CompilationObserver& observer);

enum class ScriptCompilationMode {
  // Runtime helpers and evaluation utilities retain their ordinary, uncached compilation.
  INTERNAL,
  // A service-worker syntax application participates in the process-wide cache.
  WORKER,
};

v8::MaybeLocal<v8::UnboundScript> compileScript(Lock& js,
    kj::StringPtr source,
    const v8::ScriptOrigin& origin,
    ScriptCompilationMode mode,
    const CompilationObserver& observer);

}  // namespace workerd::jsg
