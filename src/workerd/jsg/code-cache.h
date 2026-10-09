// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// V8 code caches for worker-supplied code, kept in a store that the embedder provides.
//
// V8 validates a code cache only against its own version, its flags, its read-only snapshot and
// the source length; it does not check the source content (see the warning on
// v8::ScriptCompiler::CachedData). The store is therefore keyed by a hash of the exact source
// text, and only holds caches that V8 produced in the same process. See CodeCacheStore.

#include <workerd/jsg/observer.h>

#include <v8-script.h>

#include <kj/array.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/vector.h>

namespace workerd::jsg {

// Identifies one V8 compilation unit by the content of its source. Bytecode is a function of the
// source text, the way it is compiled and process-wide V8 state (version and flags), so the key is
// independent of the name a module or script is compiled under.
struct CodeCacheKey {
  // How the UTF-8 source is turned into a V8 string and compiled. Units are distinct because their
  // UTF-8 decoders may treat invalid input differently.
  enum class Unit : uint8_t {
    // ScriptCompiler::CompileModule of the source as decoded by v8::String::NewFromUtf8 (the
    // legacy module registry).
    LEGACY_REGISTRY_ESM = 1,
    // ScriptCompiler::CompileModule of the source as encoded by the new module registry.
    NEW_REGISTRY_ESM = 2,
    // ScriptCompiler::CompileUnboundScript of the source as decoded by
    // v8::String::NewFromUtf8 (a service-worker syntax main script).
    CLASSIC_SCRIPT = 3,
  };

  kj::FixedArray<kj::byte, 32> sourceSha256;
  Unit unit;

  static CodeCacheKey compute(Unit unit, kj::ArrayPtr<const char> utf8Source);

  bool operator==(const CodeCacheKey& other) const;
  kj::uint hashCode() const;
};

// A process-wide store of V8 code caches, provided by the embedder. Implementations must be
// thread-safe: every method may be called concurrently from any isolate's thread.
//
// The store elects one compiler of each key to produce its cache, so that concurrent isolates do
// not all serialize the same code.
class CodeCacheStore: public kj::AtomicRefcounted {
 public:
  using Bytes = kj::Array<const kj::byte>;

  // The right to produce the cache for one key, held by the isolate that compiled it. Dropping a
  // Producer without calling either method ends the election, so that a later compile of the key
  // can be elected.
  class Producer {
   public:
    virtual ~Producer() noexcept(false) = default;

    // Stores `bytes`, which V8 serialized from code compiled for this producer's key.
    virtual void insert(Bytes bytes) = 0;
    // V8 could not serialize the compiled code.
    virtual void generationFailed() = 0;
  };

  struct LookupResult {
    kj::Maybe<kj::Arc<Bytes>> hit;
    // Set on a miss if the caller was elected to produce the entry.
    kj::Maybe<kj::Own<Producer>> producer;
  };
  // A hit must be bytes that a Producer for an equal key inserted, under the same V8 build and
  // flags. They are handed to V8, which does not check that they belong to the source.
  virtual LookupResult lookup(const CodeCacheKey& key) const = 0;

  // V8 refused `rejected`, which lookup() returned for `key`. `reason` is the result of
  // CachedData::CompatibilityCheck(), or kj::none if V8 refused the data while deserializing it.
  // The store should drop the entry if it still holds `rejected`, and may elect the caller to
  // produce a replacement.
  virtual kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<v8::ScriptCompiler::CachedData::CompatibilityCheckResult> reason) const = 0;
};

// One isolate's use of a CodeCacheStore. Owned by IsolateBase; every method must be called with
// the isolate lock held.
//
// Code compiled from source is serialized either right away or, inside a production window opened
// by deferProduction(), when the window is closed by produceDeferred(). Serializing after the code
// has run captures the functions V8 compiled lazily while running it, which a cache taken right
// after compilation lacks. An isolate has one window, shared by all code compiled while it is
// open.
class IsolateCodeCache {
 public:
  explicit IsolateCodeCache(kj::Own<const CodeCacheStore> store);

  // Compiles a module, consuming the store's cache for `key` if it has one. `origin` must have
  // `is_module` set.
  v8::MaybeLocal<v8::Module> compileModule(v8::Isolate* isolate,
      const CodeCacheKey& key,
      v8::Local<v8::String> source,
      const v8::ScriptOrigin& origin,
      const CompilationObserver& observer);

  // Compiles a classic script, consuming the store's cache for `key` if it has one.
  v8::MaybeLocal<v8::UnboundScript> compileScript(v8::Isolate* isolate,
      const CodeCacheKey& key,
      v8::Local<v8::String> source,
      const v8::ScriptOrigin& origin,
      const CompilationObserver& observer);

  // Opens a production window: until it is closed, caches for code compiled from source are
  // produced by produceDeferred() rather than right after compilation.
  void deferProduction();

  // Closes the production window and produces caches for the code compiled from source in it,
  // except for modules whose evaluation failed.
  void produceDeferred(v8::Isolate* isolate, const CompilationObserver& observer);

  // Closes the production window without producing anything, releasing the elections held for
  // it. Must run before the isolate is disposed.
  void discardDeferred();

 private:
  using Compiled = kj::OneOf<v8::Global<v8::Module>, v8::Global<v8::UnboundScript>>;

  struct Pending {
    kj::Own<CodeCacheStore::Producer> producer;
    Compiled compiled;
  };

  template <typename T, typename Compile>
  v8::MaybeLocal<T> compile(v8::Isolate* isolate,
      const CodeCacheKey& key,
      v8::Local<v8::String> source,
      const v8::ScriptOrigin& origin,
      const CompilationObserver& observer,
      Compile&& compileWithOptions);

  static void produce(v8::Isolate* isolate,
      CodeCacheStore::Producer& producer,
      const Compiled& compiled,
      const CompilationObserver& observer);

  kj::Own<const CodeCacheStore> store;
  bool deferring = false;
  kj::Vector<Pending> pending;
};

}  // namespace workerd::jsg
