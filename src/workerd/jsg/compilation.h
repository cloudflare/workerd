// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/code-cache.h>

#include <kj/function.h>
#include <kj/mutex.h>

namespace workerd::jsg {

class Lock;

struct ProducedCompileHints {
  // The original name in the Worker bundle, before registry normalization.
  kj::String name;
  // UTF-16 function start positions, sorted and without duplicates.
  kj::Array<int> functionPositions;
};

// One isolate's compile hint production session. Enabled before compiling a Worker's modules
// through IsolateBase::enableCompileHints(). Only bundle ES modules participate. Their V8
// handles remain alive until discard(), which must run under the isolate lock before disposal.
// An enabled isolate hosts one Worker bundle, whose original names identify module definitions.
class IsolateCompileHints {
 public:
  void record(Lock& js, kj::StringPtr name, v8::Local<v8::Module> module);
  kj::Array<ProducedCompileHints> collect(Lock& js) const;
  void discard();

 private:
  kj::HashMap<kj::String, kj::Vector<v8::Global<v8::Module>>> modules;
};

// Returns a snapshot of the enabled session, or an empty array when collection is disabled.
// Hints accumulate during evaluation and subsequent calls, including warm-up requests.
kj::Array<ProducedCompileHints> getProducedCompileHints(Lock& js);

// Reads one source-text module's V8 collector, sorting and deduplicating its function positions.
kj::Array<int> getProducedCompileHints(Lock& js, v8::Local<v8::Module> module);

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
  // Present only for bundle ES modules. The session groups query/fragment instances by this
  // original name, independently of the registry's resolution URL or path.
  kj::Maybe<kj::StringPtr> bundleModuleName;
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
