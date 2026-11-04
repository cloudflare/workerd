// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compilation.h"

#include "jsg.h"
#include "setup.h"

#include <workerd/util/sentry.h>

#include <algorithm>
#include <memory>

namespace workerd::jsg {
namespace {

// Cache selection is independent of the V8 operation. Returning none leaves ordinary source
// compilation (or a definition-local fallback) to the caller. An engaged empty handle means
// V8 was called and compilation failed, so callers must propagate its pending exception.
template <typename T>
kj::Maybe<v8::MaybeLocal<T>> tryCompileWithCache(Lock& js,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer,
    kj::FunctionParam<kj::Maybe<CodeCacheKey>(IsolateCodeCache&)> getCacheKey,
    IsolateCodeCache::CompileCallback<T> compile,
    kj::ArrayPtr<const kj::byte> embeddedCache = nullptr) {
  if (embeddedCache.size() > 0) {
    auto data = std::make_unique<v8::ScriptCompiler::CachedData>(
        embeddedCache.begin(), embeddedCache.size());
    v8::ScriptCompiler::Source compileSource(source, origin, data.release());
    auto result = compile(&compileSource, v8::ScriptCompiler::kConsumeCodeCache);
    if (result.IsEmpty()) return result;
    if (compileSource.GetCachedData()->rejected) {
      observer.onCompileCacheRejected(js.v8Isolate);
    } else {
      observer.onCompileCacheFound(js.v8Isolate);
    }
    return result;
  }
  KJ_IF_SOME(cache, IsolateBase::from(js.v8Isolate).tryGetCodeCache()) {
    KJ_IF_SOME(key, getCacheKey(cache)) {
      return cache.compile<T>(js.v8Isolate, key, source, origin, observer, compile);
    }
  }
  return kj::none;
}

// Answers V8's per-function compile hint queries from a sorted list of function positions.
// V8 asks once for each function it would otherwise compile lazily, while compiling the module.
struct CompileHintsLookup {
  kj::ArrayPtr<const int> positions;
  size_t eagerFunctionCount = 0;

  // A v8::CompileHintCallback. Must not throw: V8 calls it from inside its parser.
  static bool shouldCompileEagerly(int position, void* data) {
    auto& lookup = *static_cast<CompileHintsLookup*>(data);
    bool hinted = std::binary_search(lookup.positions.begin(), lookup.positions.end(), position);
    if (hinted) ++lookup.eagerFunctionCount;
    return hinted;
  }
};

v8::MaybeLocal<v8::Module> compileModuleWithHints(jsg::Lock& js,
    v8::Local<v8::String> content,
    const v8::ScriptOrigin& origin,
    kj::ArrayPtr<const int> compileHints,
    const CompilationObserver& observer) {
  kj::Array<int> sortedHints;
  if (!std::is_sorted(compileHints.begin(), compileHints.end())) {
    sortedHints = kj::heapArray(compileHints);
    std::sort(sortedHints.begin(), sortedHints.end());
    compileHints = sortedHints;
  }

  // V8 only calls the callback during CompileModule(), so `lookup` can live on the stack.
  CompileHintsLookup lookup{.positions = compileHints};
  v8::ScriptCompiler::Source source(
      content, origin, &CompileHintsLookup::shouldCompileEagerly, &lookup);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(
          js.v8Isolate, &source, v8::ScriptCompiler::kConsumeCompileHints)
           .ToLocal(&module)) {
    return {};
  }
  observer.onCompileHintsConsumed(js.v8Isolate, compileHints.size(), lookup.eagerFunctionCount);
  return module;
}

}  // namespace

void IsolateCompileHints::record(Lock& js, kj::StringPtr name, v8::Local<v8::Module> module) {
  auto& instances =
      modules.findOrCreate(name, [&]() -> decltype(modules)::Entry { return {kj::str(name), {}}; });
  instances.add(v8::Global<v8::Module>(js.v8Isolate, module));
}

kj::Array<ProducedCompileHints> IsolateCompileHints::collect(Lock& js) const {
  kj::Vector<ProducedCompileHints> result;
  for (auto& entry: modules) {
    kj::Vector<int> positions;
    for (auto& module: entry.value) {
      positions.addAll(getProducedCompileHints(js, module.Get(js.v8Isolate)));
    }
    if (positions.size() > 1) {
      std::sort(positions.begin(), positions.end());
      positions.truncate(std::unique(positions.begin(), positions.end()) - positions.begin());
    }
    result.add(ProducedCompileHints{kj::str(entry.key), positions.releaseAsArray()});
  }
  return result.releaseAsArray();
}

void IsolateCompileHints::discard() {
  modules.clear();
}

kj::Array<ProducedCompileHints> getProducedCompileHints(Lock& js) {
  KJ_IF_SOME(hints, IsolateBase::from(js.v8Isolate).tryGetCompileHints()) {
    return js.withinHandleScope([&]() { return hints.collect(js); });
  }
  return {};
}

kj::Array<int> getProducedCompileHints(Lock& js, v8::Local<v8::Module> module) {
  auto positions =
      module->GetUnboundModuleScript()->GetCompileHintsCollector()->GetCompileHints(js.v8Isolate);
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  return kj::heapArray<int>(positions.data(), positions.size());
}

v8::MaybeLocal<v8::Module> compileModule(Lock& js,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer,
    kj::FunctionParam<kj::Maybe<CodeCacheKey>(IsolateCodeCache&)> getCacheKey,
    const ModuleCompilationOptions& options) {
  KJ_IF_SOME(name, options.bundleModuleName) {
    KJ_IF_SOME(hints, IsolateBase::from(js.v8Isolate).tryGetCompileHints()) {
      v8::ScriptCompiler::Source compileSource(source, origin);
      v8::Local<v8::Module> module;
      if (!v8::ScriptCompiler::CompileModule(
              js.v8Isolate, &compileSource, v8::ScriptCompiler::kProduceCompileHints)
               .ToLocal(&module)) {
        return {};
      }
      hints.record(js, name, module);
      return module;
    }
  }
  auto compile = [&](v8::ScriptCompiler::Source* source,
                     v8::ScriptCompiler::CompileOptions options) {
    return v8::ScriptCompiler::CompileModule(js.v8Isolate, source, options);
  };
  KJ_IF_SOME(result,
      tryCompileWithCache<v8::Module>(
          js, source, origin, observer, getCacheKey, compile, options.embeddedCache)) {
    return result;
  }
  KJ_IF_SOME(cache, options.sharedCache) {
    return cache.compile(js, source, origin, observer);
  }
  if (options.compileHints.size() > 0) {
    return compileModuleWithHints(js, source, origin, options.compileHints, observer);
  }
  v8::ScriptCompiler::Source compileSource(source, origin);
  return v8::ScriptCompiler::CompileModule(js.v8Isolate, &compileSource);
}

v8::MaybeLocal<v8::Module> SharedModuleCompilationCache::compile(Lock& js,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer) const {
  v8::Local<v8::Module> module;
  bool needsCache = true;
  kj::Maybe<uint64_t> rejectedGeneration;
  {
    // Retain the shared lock while V8 reads the slot's borrowed bytes. Other replicas may
    // consume the same entry concurrently, but cannot replace it until compilation finishes.
    auto lock = cachedData.lockShared();
    std::unique_ptr<v8::ScriptCompiler::CachedData> data;
    KJ_IF_SOME(cached, lock->data) {
      data = std::make_unique<v8::ScriptCompiler::CachedData>(cached->data, cached->length);
      if (data->CompatibilityCheck(js.v8Isolate) != v8::ScriptCompiler::CachedData::kSuccess) {
        rejectedGeneration = lock->generation;
        data.reset();
        LOG_WARNING_ONCE("NOSENTRY Cached data for an ESM module failed its compatibility check");
      }
    }
    auto options = data == nullptr ? v8::ScriptCompiler::kNoCompileOptions
                                   : v8::ScriptCompiler::kConsumeCodeCache;
    v8::ScriptCompiler::Source compileSource(source, origin, data.release());
    if (!v8::ScriptCompiler::CompileModule(js.v8Isolate, &compileSource, options)
             .ToLocal(&module)) {
      return {};
    }
    if (options == v8::ScriptCompiler::kConsumeCodeCache) {
      if (compileSource.GetCachedData()->rejected) {
        rejectedGeneration = lock->generation;
        LOG_WARNING_ONCE("NOSENTRY Cached data for an ESM module was rejected");
      } else {
        needsCache = false;
        observer.onCompileCacheFound(js.v8Isolate);
      }
    }
  }
  if (rejectedGeneration != kj::none) observer.onCompileCacheRejected(js.v8Isolate);
  if (needsCache) {
    auto lock = cachedData.lockExclusive();
    // Another replica may already have replaced the rejected entry while we changed locks.
    KJ_IF_SOME(generation, rejectedGeneration) {
      if (lock->generation == generation) lock->data = kj::none;
    }
    if (lock->data == kj::none && !js.isPreparingSnapshot()) {
      if (auto ptr = v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript())) {
        lock->data = kj::Own<v8::ScriptCompiler::CachedData>(
            ptr, kj::_::HeapDisposer<v8::ScriptCompiler::CachedData>::instance);
        ++lock->generation;
        observer.onCompileCacheGenerated(js.v8Isolate);
      } else {
        observer.onCompileCacheGenerationFailed(js.v8Isolate);
      }
    }
  }
  return module;
}

v8::MaybeLocal<v8::Function> compileFunction(Lock& js,
    kj::StringPtr source,
    const v8::ScriptOrigin& origin,
    kj::Maybe<v8::Local<v8::Object>> contextExtension,
    const CompilationObserver& observer) {
  auto content = js.str(source);
  auto compile = [&](v8::ScriptCompiler::Source* source,
                     v8::ScriptCompiler::CompileOptions options) {
    KJ_IF_SOME(extension, contextExtension) {
      return v8::ScriptCompiler::CompileFunction(
          js.v8Context(), source, 0, nullptr, 1, &extension, options);
    }
    return v8::ScriptCompiler::CompileFunction(
        js.v8Context(), source, 0, nullptr, 0, nullptr, options);
  };
  KJ_IF_SOME(result,
      tryCompileWithCache<v8::Function>(
          js, content, origin, observer, [&](IsolateCodeCache& cache) -> kj::Maybe<CodeCacheKey> {
    auto unit = contextExtension == kj::none ? CodeCacheKey::Unit::FUNCTION
                                             : CodeCacheKey::Unit::FUNCTION_WITH_CONTEXT;
    return cache.keyFor(unit, source);
  }, compile)) {
    return result;
  }
  v8::ScriptCompiler::Source compileSource(content, origin);
  return compile(&compileSource, v8::ScriptCompiler::kNoCompileOptions);
}

v8::MaybeLocal<v8::UnboundScript> compileScript(Lock& js,
    kj::StringPtr source,
    const v8::ScriptOrigin& origin,
    ScriptCompilationMode mode,
    const CompilationObserver& observer) {
  auto content = js.str(source);
  auto compile = [&](v8::ScriptCompiler::Source* source,
                     v8::ScriptCompiler::CompileOptions options) {
    return v8::ScriptCompiler::CompileUnboundScript(js.v8Isolate, source, options);
  };
  KJ_IF_SOME(result,
      tryCompileWithCache<v8::UnboundScript>(
          js, content, origin, observer, [&](IsolateCodeCache& cache) -> kj::Maybe<CodeCacheKey> {
    if (mode == ScriptCompilationMode::INTERNAL) return kj::none;
    return cache.keyFor(CodeCacheKey::Unit::CLASSIC_SCRIPT, source);
  }, compile)) {
    return result;
  }
  v8::ScriptCompiler::Source compileSource(content, origin);
  return compile(&compileSource, v8::ScriptCompiler::kNoCompileOptions);
}

}  // namespace workerd::jsg
