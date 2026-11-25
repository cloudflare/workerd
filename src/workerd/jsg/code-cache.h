// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// V8 code caches for JavaScript, kept in a store that the embedder provides.
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
#include <kj/function.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/refcount.h>
#include <kj/vector.h>

namespace workerd::jsg {

// Identifies one V8 compilation unit by the content of its source. Bytecode is a function of the
// source text, the way it is compiled and process-wide V8 state (version and flags), so the key is
// independent of the name a module or script is compiled under.
struct CodeCacheKey {
  // How the source is turned into a V8 string and compiled. Distinct encodings and compilation
  // shapes must not share an entry, even when their source bytes are identical.
  enum class Unit : uint8_t {
    // ScriptCompiler::CompileModule of the source as decoded by v8::String::NewFromUtf8 (the
    // legacy module registry).
    LEGACY_REGISTRY_ESM = 1,
    // ScriptCompiler::CompileModule of the source as encoded by the new module registry.
    NEW_REGISTRY_ESM = 2,
    // ScriptCompiler::CompileUnboundScript of the source as decoded by
    // v8::String::NewFromUtf8 (a service-worker syntax main script).
    CLASSIC_SCRIPT = 3,
    // CompileModule of external Latin-1 or UTF-16 source, in either module registry.
    ONE_BYTE_ESM = 4,
    TWO_BYTE_ESM = 5,
    // CompileFunction of UTF-8 source, with no parameters and zero or one context extension.
    FUNCTION = 6,
    FUNCTION_WITH_CONTEXT = 7,
  };

  kj::FixedArray<kj::byte, 32> sourceSha256;
  Unit unit;

  static CodeCacheKey compute(Unit unit, kj::ArrayPtr<const char> source);

  bool operator==(const CodeCacheKey& other) const;
  kj::uint hashCode() const;
};

// Whether a store lookup that misses may elect the caller to produce the entry.
WD_STRONG_BOOL(MayElect);

// A process-wide store of V8 code caches, provided by the embedder. Implementations must be
// thread-safe: every method may be called concurrently from any isolate's thread.
//
// The store elects one compiler of each key to produce its cache, so that concurrent isolates do
// not all serialize the same code. An embedder can limit which caches an isolate shares by giving
// it its own CodeCacheStore, a view of a store that other isolates also use.
class CodeCacheStore: public kj::AtomicRefcounted {
 public:
  using Bytes = kj::Array<const kj::byte>;

  struct LookupResult;

  // A hit whose bytes the store decodes for V8 only when a compile uses it, so that hits for code
  // that is never compiled cost no decoding. May be destroyed on any thread, without the isolate
  // lock.
  class LazyHit {
   public:
    virtual ~LazyHit() noexcept(false) = default;

    // Called under the isolate lock, at most once, when a compile first uses the hit. Returns the
    // bytes as `hit`. If it cannot or will not decode them, the store counts why and returns no
    // hit and, if `mayElect` is set, may elect the caller to produce a replacement; the caller
    // then compiles as after a miss, without reporting a reject.
    virtual LookupResult decode(MayElect mayElect) = 0;
  };

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
    // Set, instead of `hit`, on a hit that the caller decodes only if a compile uses it. Such a
    // hit serves one compile, which frees the decoded bytes once it is done.
    kj::Maybe<kj::Own<LazyHit>> lazyHit;
  };
  // A hit must be bytes that V8 serialized for a Producer of an equal key, under the same V8
  // build and flags. They are handed to V8, which does not check that they belong to the source.
  virtual LookupResult lookup(const CodeCacheKey& key, MayElect mayElect) const = 0;

  // V8 refused `rejected`, which lookup() returned for `key`. `reason` is the result of
  // CachedData::CompatibilityCheck(), or kj::none if V8 refused the data while deserializing it.
  // The store should drop the entry if it still holds `rejected`, and, if `mayElect` is set, may
  // elect the caller to produce a replacement.
  virtual kj::Maybe<kj::Own<Producer>> reject(const CodeCacheKey& key,
      const Bytes& rejected,
      kj::Maybe<v8::ScriptCompiler::CachedData::CompatibilityCheckResult> reason,
      MayElect mayElect) const = 0;

  // When an isolate produces the caches it was elected for.
  enum class ProductionTime {
    // When its startup ends (IsolateCodeCache::finishStartup()).
    END_OF_STARTUP,
    // When the embedder calls IsolateCodeCache::produceDeferred(). The production window stays
    // open after startup, so that code compiled later joins it.
    ON_EMBEDDER_REQUEST,
  };
  virtual ProductionTime getProductionTime() const {
    return ProductionTime::END_OF_STARTUP;
  }

  // Whether the isolate may serialize another of the caches it was elected for. produceDeferred()
  // asks before each one. A store that bounds what one isolate produces returns false once the
  // isolate has produced its share; the elections left then end without producing.
  virtual bool mayProduce() const {
    return true;
  }

  // Whether builtin modules use the store. Only builtins that ship without a compile cache ask
  // it. A store whose caches are scoped to one script returns false, so that each script does not
  // keep its own copy of the same builtin's cache.
  virtual bool cachesBuiltins() const {
    return true;
  }
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
// The store decides when the window closes (CodeCacheStore::getProductionTime()). At the end of
// startup, code compiled later, such as a module first imported during a request, consumes the
// store's cache if it has one, but is otherwise left uncached: a miss does not elect the
// isolate, so that the request does not pay for producing the cache. Modules that should populate
// the store must then be imported during startup. If the embedder closes the window instead, code
// compiled until then, including during requests, is produced with the code compiled at startup,
// and serializing it also captures the functions those requests ran. The embedder must then close
// the window under a lock that no request is charged for.
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
  // called or the window closes. Does nothing after endProduction().
  void prepare(CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source);

  // Looks up `key` for the compiles of it later in the window. Only the first takes the producer
  // election. A hit that the store shares serves every such compile without another lookup; a
  // lazy one serves only the first, and later ones look the key up again. Does nothing after
  // endProduction(): compiles then look their key up as they would outside a window.
  void prepare(const CodeCacheKey& key);

  // Returns the key of `utf8Source` compiled as `unit`. If prepare() was given the same bytes, at
  // the same address, in this window, returns the key it computed rather than hashing again.
  CodeCacheKey keyFor(CodeCacheKey::Unit unit, kj::ArrayPtr<const char> utf8Source);

  // Forgets the addresses of the sources given to prepare(), whose bytes the caller can no longer
  // vouch for. Their prepared lookups are kept.
  void forgetPreparedSources();

  // Whether builtin modules use the store (see CodeCacheStore::cachesBuiltins()).
  bool cachesBuiltins() const {
    return store->cachesBuiltins();
  }

  template <typename T>
  using CompileCallback = kj::FunctionParam<v8::MaybeLocal<T>(
      v8::ScriptCompiler::Source*, v8::ScriptCompiler::CompileOptions)>;

  // Selects the store's bytecode and invokes the V8 operation on both hits and misses. The
  // callback consumes cached data through V8's compile API; a miss may retain its result for
  // deferred serialization. T is Module, UnboundScript or Function. The key must identify the
  // exact source and compilation shape used by the callback.
  template <typename T>
  v8::MaybeLocal<T> compile(v8::Isolate* isolate,
      const CodeCacheKey& key,
      v8::Local<v8::String> source,
      const v8::ScriptOrigin& origin,
      const CompilationObserver& observer,
      CompileCallback<T> compileWithOptions);

  // Opens a production window: until it is closed, a miss may elect this isolate to produce the
  // cache, which produceDeferred() then does. The window must not be open already, unless the
  // embedder closes it, in which case the code compiled next joins the open window. Does nothing
  // after endProduction().
  void deferProduction();

  // Ends the startup that deferProduction() began: produces the window's caches if the store
  // produces at the end of startup, and otherwise leaves the window open. An open window then
  // keeps the elections and compiled code that production needs.
  void finishStartup(v8::Isolate* isolate, const CompilationObserver& observer);

  // Ends a startup that failed: discards the window, unless an earlier startup finished in it and
  // left it open for the embedder to close. Code compiled by the startups that succeeded, and by
  // their requests, then stays in the window.
  void failStartup();

  // Closes the production window and produces caches for the code compiled from source in it,
  // except for modules whose evaluation failed. Does nothing if the window is closed.
  void produceDeferred(v8::Isolate* isolate, const CompilationObserver& observer);

  bool isWindowOpen() const {
    return deferring;
  }

  // Closes the production window without producing anything, releasing the elections held for
  // it. Must run before the isolate is disposed.
  void discardDeferred();

  // Discards the production window, if one is open, and keeps any other from opening, so that code
  // compiled later neither elects the isolate nor is kept alive for production; the isolate still
  // loads scripts and consumes caches as outside a window. For an embedder that closes the window,
  // once it will close no other.
  void endProduction();

 private:
  using Compiled =
      kj::OneOf<v8::Global<v8::Module>, v8::Global<v8::UnboundScript>, v8::Global<v8::Function>>;

  struct Pending {
    kj::Own<CodeCacheStore::Producer> producer;
    Compiled compiled;
  };

  // A source that prepare() hashed, found by its address.
  struct PreparedSource {
    size_t size;
    CodeCacheKey key;
  };

  static void produce(v8::Isolate* isolate,
      CodeCacheStore::Producer& producer,
      const Compiled& compiled,
      const CompilationObserver& observer);

  // The lookup prepared for `key`, or a new one, with its hit decoded.
  CodeCacheStore::LookupResult takeLookup(const CodeCacheKey& key);

  // Decodes the lazy hit of `lookup`, if any. If the store does not decode it, turns `lookup`
  // into a miss, keeping any producer it elected.
  void decodeHit(CodeCacheStore::LookupResult& lookup);

  // Ends the window's state except for `pending`.
  void closeWindow();

  kj::Own<const CodeCacheStore> store;
  bool deferring = false;
  // Set by endProduction().
  bool ended = false;
  // Whether a startup finished in the open window and left it open.
  bool startupFinished = false;
  kj::Vector<Pending> pending;
  kj::HashMap<const void*, PreparedSource> preparedSources;
  // The lookups that prepare() made, for the compiles of their keys in the window.
  kj::HashMap<CodeCacheKey, CodeCacheStore::LookupResult> preparedLookups;
};

}  // namespace workerd::jsg
