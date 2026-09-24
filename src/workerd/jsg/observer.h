// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/util/strong-bool.h>

#include <v8-local-handle.h>

#include <kj/common.h>
#include <kj/exception.h>
#include <kj/string.h>

// Forward declare v8::Isolate here, this allows us to avoid including the V8 header and compile
// some targets without depending on V8.
namespace v8 {
class Isolate;
}

namespace workerd::jsg {

class Url;

struct ResolveObserver {
  virtual ~ResolveObserver() noexcept(false) {}

  // Identifies the context in which a module resolution is being performed.
  enum class Context {
    // The resolve is being performed by a worker bundle module
    // (that is, a worker script is calling import or require).
    BUNDLE,
    // The resolve is being performed by a builtin module
    // (that is, one of the modules built into the worker runtime).
    BUILTIN,
    // Like builtin, but it's a module that is *only* resolvable from a builtin
    // (like the `node-internal:...` modules)
    BUILTIN_ONLY,
    // Resolves only user-importable built-in modules (the kBuiltin bundle),
    // excluding both worker bundle modules and internal-only modules. Used
    // by user-facing APIs like process.getBuiltinModule() that must not
    // expose internal modules or return user bundle overrides.
    PUBLIC_BUILTIN,
  };

  enum class Source {
    // The resolve originated from a static import statement.
    STATIC_IMPORT,
    // The resolve originated from a dynamic import statement.
    DYNAMIC_IMPORT,
    // The resolve originated from a CommonJS require() call.
    REQUIRE,
    // The resolve originated from an internal direct call to
    // the ModuleRegistry.
    INTERNAL,
  };

  // Used to report the status of a module resolution.
  class ResolveStatus {
   public:
    ResolveStatus() = default;
    KJ_DISALLOW_COPY_AND_MOVE(ResolveStatus);
    virtual ~ResolveStatus() noexcept(false) {}

    // Indicates that the module resolution was successful and a
    // matching module was found in the registry.
    virtual void found() {}

    // Indicates that the module resolution failed because no matching
    // module was found in the registry.
    virtual void notFound() {}

    // Indicates that the module resolution failed because an error
    // occurred.
    virtual void exception(kj::Exception&& exception) {}
  };

  // Called when a module is being resolved. The returned ResolveStatus
  // object will be used to report the result of the resolution.
  // It is guaranteed that isolate lock is not held during invocation.
  virtual kj::Own<ResolveStatus> onResolveModule(
      const Url& specifier, Context context, Source source) const {
    static ResolveStatus nonopStatus;
    return {&nonopStatus, kj::NullDisposer::instance};
  }

  // Called when a module is being resolved. The returned ResolveStatus
  // object will be used to report the result of the resolution.
  // It is guaranteed that isolate lock is not held during invocation.
  virtual kj::Own<ResolveStatus> onResolveModule(
      kj::StringPtr specifier, Context context, Source source) const {
    static ResolveStatus nonopStatus;
    return {&nonopStatus, kj::NullDisposer::instance};
  }
};

struct CompilationObserver {
  virtual ~CompilationObserver() noexcept(false) {}

  // see ModuleInfoCompileOption
  enum class Option { BUNDLE, BUILTIN };

  // Monitors behavior of compilation processes.

  // Called at the start of ESM compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onEsmCompilationStart(
      v8::Isolate* isolate, kj::StringPtr name, Option option) const {
    return kj::Own<void>();
  }

  // Called at the start of Script (e.g. non-ESM) compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onScriptCompilationStart(
      v8::Isolate* isolate, kj::Maybe<kj::StringPtr> name = kj::none) const {
    return kj::Own<void>();
  }

  // Called at the start of wasm compilation.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationStart(v8::Isolate* isolate, size_t codeSize) const {
    return kj::Own<void>();
  }

  // Variation that is called at the start of wasm compilation from cache.
  // Returned value will be destroyed when module compilation finishes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onWasmCompilationFromCacheStart(v8::Isolate* isolate) const {
    return kj::Own<void>();
  }

  // Called at the start of json module parsing.
  // Returned value will be destroyed when parsing completes.
  // It is guaranteed that isolate lock is held during invocation.
  virtual kj::Own<void> onJsonCompilationStart(v8::Isolate* isolate, size_t inputSize) const {
    return kj::Own<void>();
  }

  virtual void onCompileCacheFound(v8::Isolate* isolate) const {}
  virtual void onCompileCacheRejected(v8::Isolate* isolate) const {}
  virtual void onCompileCacheGenerated(v8::Isolate* isolate) const {}
  virtual void onCompileCacheGenerationFailed(v8::Isolate* isolate) const {}
};

struct InternalExceptionObserver {
  virtual ~InternalExceptionObserver() noexcept(false) {}

  struct Detail {
    bool isInternal;
    bool isFromRemote;
    bool isDurableObjectReset;
    using InternalErrorId = kj::FixedArray<char, 24>;
    kj::Maybe<InternalErrorId> internalErrorId;
  };

  // Called when an internal exception is created (see exceptionToJs).
  // Used to collect metrics on various internal error conditions.
  virtual void reportInternalException(const kj::Exception&, Detail detail) {}
};

WD_STRONG_BOOL(IsCodeLike);

// A destination for the samples of one of V8's internal histograms, for one isolate.
class V8HistogramSink {
 public:
  virtual ~V8HistogramSink() noexcept(false) = default;

  // Called with every sample V8 records. V8 may call this from one of its background threads
  // (Wasm compilation), so implementations must be thread-safe.
  virtual void addSample(int sample) = 0;
};

struct IsolateObserver: public CompilationObserver,
                        public InternalExceptionObserver,
                        public ResolveObserver {
  virtual ~IsolateObserver() noexcept(false) {}

  // Called when eval(), new Function(), or similar dynamic code generation
  // is performed. Note that the source here may not be a string if isCodeLike
  // is YES.
  virtual void onDynamicEval(
      v8::Local<v8::Context> context, v8::Local<v8::Value> source, IsCodeLike isCodeLike) {
    // Default is to do nothing.
  }

  // V8 keeps histograms of its own work: compile and deserialize times, optimization time, GC
  // phases, code cache outcomes, and more; V8's counters-definitions.h lists them by name. V8
  // records into a histogram only once the embedder has supplied a sink for it. Until then the
  // code paths feeding it skip the clock reads and bookkeeping, so leaving a histogram off costs
  // nothing beyond this call.
  //
  // Called the first time V8 uses the named histogram in this isolate. Return a sink to enable
  // it, or none to leave it off. V8 asks again the next time it uses a histogram that was left
  // off, so the decision should be cheap. `min`, `max`, and `buckets` are V8's own bucket layout;
  // for histograms that record an enumeration, `max` is the number of values. The sink must stay
  // alive as long as the isolate. Both this method and the sink are called from inside V8 and
  // must not throw.
  virtual kj::Maybe<V8HistogramSink&> tryCreateV8HistogramSink(
      kj::StringPtr name, int min, int max, size_t buckets) {
    return kj::none;
  }
};

}  // namespace workerd::jsg
