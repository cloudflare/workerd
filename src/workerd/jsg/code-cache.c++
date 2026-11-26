// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "code-cache.h"

#include <openssl/sha.h>
#include <v8-function.h>
#include <v8-isolate.h>

#include <kj/debug.h>
#include <kj/hash.h>

#include <climits>
#include <cstring>
#include <memory>

namespace workerd::jsg {

CodeCacheKey CodeCacheKey::compute(Unit unit, kj::ArrayPtr<const char> source) {
  CodeCacheKey key{.unit = unit};
  static_assert(sizeof(key.sourceSha256) == SHA256_DIGEST_LENGTH);
  SHA256(source.asBytes().begin(), source.size(), key.sourceSha256.begin());
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

CodeCacheStore::LookupResult CodeCacheStore::Held::decode(MayElect) {
  KJ_UNIMPLEMENTED("this store hands over no hit undecoded");
}

IsolateCodeCache::IsolateCodeCache(kj::Own<const CodeCacheStore> store): store(kj::mv(store)) {}

IsolateCodeCache::~IsolateCodeCache() noexcept(false) {
  if (retained.size() == 0) return;
  auto released = KJ_MAP(entry, retained) { return kj::mv(entry.value); };
  // The isolate is being torn down; failing to give the caches back only loses them.
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() { store->release(kj::mv(released)); })) {
    KJ_LOG(ERROR, "failed to release code caches of a destroyed isolate", exception);
  }
}

void IsolateCodeCache::prepare(CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source) {
  if (ended) return;
  auto key = CodeCacheKey::compute(unit, utf8Source);
  prepare(key);
  preparedSources.upsert(
      static_cast<const void*>(utf8Source.begin()), {.size = utf8Source.size(), .key = key});
}

void IsolateCodeCache::prepare(const CodeCacheKey& key) {
  // The embedder prepares as it opens a window, which deferProduction() keeps closed once
  // production ended.
  if (ended) return;
  KJ_REQUIRE(deferring, "code cache lookups are prepared only inside a production window");
  // Identical sources share a lookup and at most one producer election.
  preparedLookups.findOrCreate(key, [&]() -> decltype(preparedLookups)::Entry {
    return {key, store->lookup(key, MayElect::YES)};
  });
}

CodeCacheKey IsolateCodeCache::keyFor(
    CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source) {
  KJ_IF_SOME(prepared, preparedSources.find(static_cast<const void*>(utf8Source.begin()))) {
    if (prepared.size == utf8Source.size() && prepared.key.unit == unit) return prepared.key;
  }
  return CodeCacheKey::compute(unit, utf8Source);
}

void IsolateCodeCache::forgetPreparedSources() {
  preparedSources.clear();
}

CodeCacheStore::LookupResult IsolateCodeCache::takeLookup(const CodeCacheKey& key) {
  KJ_IF_SOME(prepared, preparedLookups.find(key)) {
    decodeHit(prepared);
    if (prepared.held != kj::none) {
      // The first compile takes a hit that the store handed over, so that its bytes are freed
      // once that compile is done. Identical sources compiled later, which are rare, compile from
      // source.
      auto taken = kj::mv(prepared);
      prepared = {};
      return taken;
    }
    // A hit that the store shares serves other modules with identical source too. Only the first
    // compile takes the producer.
    return {
      .hit = prepared.hit.map([](auto& bytes) { return bytes.addRef(); }),
      .producer = kj::mv(prepared.producer),
    };
  }
  auto result = store->lookup(key, MayElect(deferring));
  decodeHit(result);
  return result;
}

void IsolateCodeCache::decodeHit(CodeCacheStore::LookupResult& lookup) {
  if (lookup.hit != kj::none) return;
  auto& held = KJ_UNWRAP_OR_RETURN(lookup.held);
  auto decoded = held->decode(MayElect(deferring));
  KJ_IF_SOME(bytes, decoded.hit) {
    lookup.hit = kj::mv(bytes);
    return;
  }
  // The store could not decode its own cache, or refused to, and counted why. V8 rejected nothing,
  // so the compile goes on as after a miss.
  lookup.held = kj::none;
  if (lookup.producer == kj::none) lookup.producer = kj::mv(decoded.producer);
}

void IsolateCodeCache::retain(const CodeCacheKey& key, kj::Own<CodeCacheStore::Held> held) {
  retained.upsert(key, kj::mv(held),
      [](auto& existing, auto&& replacement) { existing = kj::mv(replacement); });
}

template <typename T>
v8::MaybeLocal<T> IsolateCodeCache::compile(v8::Isolate* isolate,
    const CodeCacheKey& key,
    v8::Local<v8::String> source,
    const v8::ScriptOrigin& origin,
    const CompilationObserver& observer,
    CompileCallback<T> compileWithOptions) {
  // `lookup.hit` owns the cached bytes, which V8 reads without taking ownership, so it must
  // outlive `compileSource`.
  auto lookup = takeLookup(key);
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
    } else {
      producer = store->reject(key, *bytes, check, MayElect(deferring));
      observer.onCompileCacheRejected(isolate);
    }
  }

  v8::ScriptCompiler::Source compileSource(source, origin, cachedData);
  auto options = cachedData == nullptr ? v8::ScriptCompiler::kNoCompileOptions
                                       : v8::ScriptCompiler::kConsumeCodeCache;
  KJ_ASSERT(v8::ScriptCompiler::CompileOptionsIsValid(options));
  v8::Local<T> result;
  bool compiled = compileWithOptions(&compileSource, options).ToLocal(&result);
  bool consumed = cachedData != nullptr && !compileSource.GetCachedData()->rejected;
  if (consumed) {
    // V8 accepted the cache, which stays good even if compilation failed for another reason.
    KJ_IF_SOME(held, lookup.held) {
      retain(key, kj::mv(held));
    }
  }
  if (!compiled) {
    return {};
  }

  if (cachedData != nullptr) {
    if (!consumed) {
      // V8 refused the data while deserializing it and compiled from source instead.
      producer = store->reject(key, *KJ_ASSERT_NONNULL(lookup.hit), kj::none, MayElect(deferring));
      observer.onCompileCacheRejected(isolate);
    } else {
      observer.onCompileCacheFound(isolate);
    }
  }
  // A refused cache is discarded with `lookup`.

  KJ_IF_SOME(p, producer) {
    // Only a compile inside the production window is elected.
    KJ_ASSERT(deferring);
    pending.add(
        Pending{.key = key, .producer = kj::mv(p), .compiled = v8::Global<T>(isolate, result)});
  }
  return result;
}

template v8::MaybeLocal<v8::Module> IsolateCodeCache::compile<v8::Module>(v8::Isolate*,
    const CodeCacheKey&,
    v8::Local<v8::String>,
    const v8::ScriptOrigin&,
    const CompilationObserver&,
    CompileCallback<v8::Module>);

template v8::MaybeLocal<v8::UnboundScript> IsolateCodeCache::compile<v8::UnboundScript>(
    v8::Isolate*,
    const CodeCacheKey&,
    v8::Local<v8::String>,
    const v8::ScriptOrigin&,
    const CompilationObserver&,
    CompileCallback<v8::UnboundScript>);

template v8::MaybeLocal<v8::Function> IsolateCodeCache::compile<v8::Function>(v8::Isolate*,
    const CodeCacheKey&,
    v8::Local<v8::String>,
    const v8::ScriptOrigin&,
    const CompilationObserver&,
    CompileCallback<v8::Function>);

void IsolateCodeCache::deferProduction() {
  if (ended) return;
  if (deferring) {
    // The window is per isolate, but each script opens its own, so a second window would mean a
    // second script, whose code would be produced or discarded with the first one's. That is
    // only acceptable when the embedder closes the window, and so decides when both are done.
    KJ_ASSERT(store->getProductionTime() == CodeCacheStore::ProductionTime::ON_EMBEDDER_REQUEST,
        "code cache production window is already open");
    return;
  }
  deferring = true;
}

void IsolateCodeCache::finishStartup(v8::Isolate* isolate, const CompilationObserver& observer) {
  if (!deferring) return;
  if (store->getProductionTime() == CodeCacheStore::ProductionTime::END_OF_STARTUP) {
    produceDeferred(isolate, observer);
    return;
  }
  startupFinished = true;
}

void IsolateCodeCache::failStartup() {
  if (!startupFinished) discardDeferred();
}

void IsolateCodeCache::produceDeferred(v8::Isolate* isolate, const CompilationObserver& observer) {
  if (!deferring) return;
  closeWindow();
  auto entries = kj::mv(pending);
  for (auto& entry: entries) {
    // Dropping the rest ends their elections.
    if (!store->mayProduce()) break;
    KJ_IF_SOME(module, entry.compiled.tryGet<v8::Global<v8::Module>>()) {
      v8::HandleScope scope(isolate);
      // An errored module's code is complete, but the cache policy does not retain it.
      if (module.Get(isolate)->GetStatus() == v8::Module::kErrored) continue;
    }
    KJ_IF_SOME(held, produce(isolate, *entry.producer, entry.compiled, observer)) {
      retain(entry.key, kj::mv(held));
    }
  }
}

void IsolateCodeCache::discardDeferred() {
  closeWindow();
  pending.clear();
}

void IsolateCodeCache::endProduction() {
  ended = true;
  discardDeferred();
}

void IsolateCodeCache::closeWindow() {
  deferring = false;
  startupFinished = false;
  forgetPreparedSources();
  // Give back the caches handed over for keys that no compile used.
  kj::Vector<kj::Own<CodeCacheStore::Held>> unused;
  for (auto& entry: preparedLookups) {
    KJ_IF_SOME(held, entry.value.held) {
      unused.add(kj::mv(held));
    }
  }
  preparedLookups.clear();
  if (!unused.empty()) store->release(unused.releaseAsArray());
}

IsolateCodeCache::MaybeHeld IsolateCodeCache::produce(v8::Isolate* isolate,
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
    KJ_CASE_ONEOF(function, v8::Global<v8::Function>) {
      data.reset(v8::ScriptCompiler::CreateCodeCacheForFunction(function.Get(isolate)));
    }
  }
  if (data == nullptr || data->length <= 0) {
    producer.generationFailed();
    observer.onCompileCacheGenerationFailed(isolate);
    return kj::none;
  }
  // Copy the bytes so that the store holds memory it allocated itself rather than V8's.
  auto held = producer.insert(kj::heapArray<kj::byte>(data->data, data->length));
  observer.onCompileCacheGenerated(isolate);
  return held;
}

}  // namespace workerd::jsg
