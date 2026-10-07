// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "code-cache.h"

#include <openssl/sha.h>

#include <kj/debug.h>
#include <kj/hash.h>

#include <climits>
#include <cstring>
#include <memory>

namespace workerd::jsg {

CodeCacheKey CodeCacheKey::compute(Unit unit, kj::ArrayPtr<const char> utf8Source) {
  CodeCacheKey key{.unit = unit};
  static_assert(sizeof(key.sourceSha256) == SHA256_DIGEST_LENGTH);
  SHA256(utf8Source.asBytes().begin(), utf8Source.size(), key.sourceSha256.begin());
  return key;
}

bool CodeCacheKey::operator==(const CodeCacheKey& other) const {
  return unit == other.unit && sourceSha256.asPtr() == other.sourceSha256.asPtr();
}

kj::uint CodeCacheKey::hashCode() const {
  // The digest is uniformly distributed, so any part of it is a good hash.
  kj::uint prefix;
  memcpy(&prefix, sourceSha256.begin(), sizeof(prefix));
  return kj::hashCode(prefix, static_cast<uint8_t>(unit));
}

IsolateCodeCache::IsolateCodeCache(kj::Own<const CodeCacheStore> store): store(kj::mv(store)) {}

v8::MaybeLocal<v8::Module> IsolateCodeCache::compileModule(v8::Isolate* isolate,
    const CodeCacheKey& key,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer) {
  return compile<v8::Module>(isolate, key, source, origin, observer,
      [&](v8::ScriptCompiler::Source* compileSource, v8::ScriptCompiler::CompileOptions options) {
    return v8::ScriptCompiler::CompileModule(isolate, compileSource, options);
  });
}

v8::MaybeLocal<v8::UnboundScript> IsolateCodeCache::compileScript(v8::Isolate* isolate,
    const CodeCacheKey& key,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer) {
  return compile<v8::UnboundScript>(isolate, key, source, origin, observer,
      [&](v8::ScriptCompiler::Source* compileSource, v8::ScriptCompiler::CompileOptions options) {
    return v8::ScriptCompiler::CompileUnboundScript(isolate, compileSource, options);
  });
}

template <typename T, typename Compile>
v8::MaybeLocal<T> IsolateCodeCache::compile(v8::Isolate* isolate,
    const CodeCacheKey& key,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer,
    Compile&& compileWithOptions) {
  // `lookup.hit` owns the cached bytes, which V8 reads without taking ownership, so it must
  // outlive `compileSource`.
  auto lookup = store->lookup(key);
  auto producer = kj::mv(lookup.producer);

  // Ownership of the CachedData object (but not of its buffer) passes to `compileSource`.
  v8::ScriptCompiler::CachedData* cachedData = nullptr;
  KJ_IF_SOME(bytes, lookup.hit) {
    KJ_ASSERT(bytes->size() <= INT_MAX);
    auto candidate = std::make_unique<v8::ScriptCompiler::CachedData>(bytes->begin(),
        static_cast<int>(bytes->size()), v8::ScriptCompiler::CachedData::BufferNotOwned);
    auto check = candidate->CompatibilityCheck(isolate);
    if (check == v8::ScriptCompiler::CachedData::kSuccess) {
      cachedData = candidate.release();
      observer.onCompileCacheFound(isolate);
    } else {
      producer = store->reject(key, *bytes, check);
      observer.onCompileCacheRejected(isolate);
    }
  }

  v8::ScriptCompiler::Source compileSource(source, origin, cachedData);
  auto options = cachedData == nullptr ? v8::ScriptCompiler::kNoCompileOptions
                                       : v8::ScriptCompiler::kConsumeCodeCache;
  KJ_ASSERT(v8::ScriptCompiler::CompileOptionsIsValid(options));
  v8::Local<T> result;
  if (!compileWithOptions(&compileSource, options).ToLocal(&result)) {
    return {};
  }

  if (cachedData != nullptr && compileSource.GetCachedData()->rejected) {
    // V8 refused the data while deserializing it and compiled from source instead.
    producer = store->reject(key, *KJ_ASSERT_NONNULL(lookup.hit), kj::none);
    observer.onCompileCacheRejected(isolate);
  }

  KJ_IF_SOME(p, producer) {
    Compiled compiled = v8::Global<T>(isolate, result);
    if (deferring) {
      pending.add(Pending{.producer = kj::mv(p), .compiled = kj::mv(compiled)});
    } else {
      produce(isolate, *p, compiled, observer);
    }
  }
  return result;
}

void IsolateCodeCache::deferProduction() {
  deferring = true;
}

void IsolateCodeCache::produceDeferred(v8::Isolate* isolate, const CompilationObserver& observer) {
  deferring = false;
  auto entries = kj::mv(pending);
  for (auto& entry: entries) {
    KJ_IF_SOME(module, entry.compiled.tryGet<v8::Global<v8::Module>>()) {
      v8::HandleScope scope(isolate);
      // An errored module's code is complete, but it is not worth keeping: the code that failed
      // will fail again.
      if (module.Get(isolate)->GetStatus() == v8::Module::kErrored) continue;
    }
    produce(isolate, *entry.producer, entry.compiled, observer);
  }
}

void IsolateCodeCache::discardDeferred() {
  deferring = false;
  pending.clear();
}

void IsolateCodeCache::produce(v8::Isolate* isolate,
    CodeCacheStore::Producer& producer,
    const Compiled& compiled,
    const CompilationObserver& observer) {
  v8::HandleScope scope(isolate);
  std::unique_ptr<v8::ScriptCompiler::CachedData> data;
  KJ_SWITCH_ONEOF(compiled) {
    KJ_CASE_ONEOF(module, v8::Global<v8::Module>) {
      data.reset(
          v8::ScriptCompiler::CreateCodeCache(module.Get(isolate)->GetUnboundModuleScript()));
    }
    KJ_CASE_ONEOF(script, v8::Global<v8::UnboundScript>) {
      data.reset(v8::ScriptCompiler::CreateCodeCache(script.Get(isolate)));
    }
  }
  if (data == nullptr || data->length <= 0) {
    producer.generationFailed();
    observer.onCompileCacheGenerationFailed(isolate);
    return;
  }
  // Copy the bytes so that the store holds memory it allocated itself rather than V8's.
  producer.insert(kj::heapArray<kj::byte>(data->data, data->length));
  observer.onCompileCacheGenerated(isolate);
}

}  // namespace workerd::jsg
