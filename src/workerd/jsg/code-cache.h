// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// V8 code caches for worker-supplied code, kept in a store that the embedder provides.
//
// V8 validates a code cache only against its own version, its flags, its read-only snapshot and
// the source length; it does not check the source content (see the warning on
// v8::ScriptCompiler::CachedData). That a cache belongs to the source it is used for therefore
// rests entirely on the store's key, a SHA-256 of the exact source text (CodeCacheKey). The store
// only holds caches that V8 produced in the same process, and its entries never leave the process.
// See CodeCacheStore.

#include <workerd/jsg/observer.h>
#include <workerd/util/strong-bool.h>

#include <v8-script.h>

#include <kj/array.h>
#include <kj/map.h>
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

// Whether a store lookup that misses may elect the caller to produce the entry.
WD_STRONG_BOOL(MayElect);

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
    // Set on a miss if the caller may be elected and was elected to produce the entry.
    kj::Maybe<kj::Own<Producer>> producer;
  };
  // A hit must be bytes that a Producer for an equal key inserted, under the same V8 build and
  // flags. They are handed to V8, which does not check that they belong to the source.
  virtual LookupResult lookup(const CodeCacheKey& key, MayElect mayElect) const = 0;

  // V8 refused `rejected`, which lookup() returned for `key`. `reason` is the result of
  // CachedData::CompatibilityCheck(), or kj::none if V8 refused the data while deserializing it.
  // The store should drop the entry if it still holds `rejected`, and, if `mayElect` is set, may
  // elect the caller to produce a replacement.
  virtual kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<v8::ScriptCompiler::CachedData::CompatibilityCheckResult> reason,
      MayElect mayElect) const = 0;
};

// One isolate's use of a CodeCacheStore. Owned by IsolateBase; every method must be called with
// the isolate lock held.
//
// Caches are produced only for code compiled from source inside a production window, opened by
// deferProduction(), when the window is closed by produceDeferred(). Serializing after the code
// has run captures the functions V8 compiled lazily while running it, which a cache taken right
// after compilation lacks. An isolate has one window, shared by all code compiled while it is
// open, so an isolate with a store must host one script at a time.
//
// Code compiled outside the window, such as a module first imported during a request, consumes
// the store's cache if it has one, but is otherwise left uncached: a miss does not elect the
// isolate, so that the request does not pay for producing the cache. Modules that should populate
// the store must be imported during startup.
//
// Inside a window, prepare() hashes a source and looks it up ahead of its compile. The embedder
// calls it before entering the startup CPU limit to exclude hashing and store lookups. Consuming
// a hit replaces compilation with V8 deserialization, which remains inside the limit.
class IsolateCodeCache {
 public:
  explicit IsolateCodeCache(kj::Own<const CodeCacheStore> store);
  KJ_DISALLOW_COPY_AND_MOVE(IsolateCodeCache);

  // Computes the key of `utf8Source` compiled as `unit` and looks it up, for a compile of the same
  // bytes, at the same address, later in the window (see keyFor()). Since the address stands in
  // for the content, the bytes must stay alive and unchanged until forgetPreparedSources() is
  // called or the window closes.
  void prepare(CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source);

  // Looks up `key`, for a compile of it later in the window. Until the window closes, the result
  // serves every such compile without another store lookup. Only the first compile takes its
  // producer election, if any. A key that no compile uses is released when the window closes.
  void prepare(const CodeCacheKey& key);

  // Returns the key of `utf8Source` compiled as `unit`. If prepare() was given the same bytes, at
  // the same address, in this window, returns the key it computed rather than hashing again.
  CodeCacheKey keyFor(CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source);

  // Forgets the addresses of the sources given to prepare(), whose bytes the caller can no longer
  // vouch for. Their prepared lookups are kept.
  void forgetPreparedSources();

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

  // Opens a production window: until it is closed, a miss may elect this isolate to produce the
  // cache, which produceDeferred() then does. The window must not be open already.
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

  // A source that prepare() hashed, found by its address.
  struct PreparedSource {
    size_t size;
    CodeCacheKey key;
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

  // The lookup prepared for `key`, or a new one.
  CodeCacheStore::LookupResult takeLookup(const CodeCacheKey& key);

  // Ends the window's state except for `pending`.
  void closeWindow();

  kj::Own<const CodeCacheStore> store;
  bool deferring = false;
  kj::Vector<Pending> pending;
  kj::HashMap<const void*, PreparedSource> preparedSources;
  kj::HashMap<CodeCacheKey, CodeCacheStore::LookupResult> preparedLookups;
};

}  // namespace workerd::jsg
