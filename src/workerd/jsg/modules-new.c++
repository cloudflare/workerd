// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "modules-new.h"

#include "buffersource.h"

#include <workerd/jsg/function.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/util.h>

#include <kj/mutex.h>
#include <kj/table.h>

namespace workerd::jsg::modules {

namespace {
// Returns kj::none if this given module is incapable of resolving the given
// context. Otherwise, returns the module.
kj::Maybe<const Module&> checkModule(const ResolveContext& context, const Module& module) {
  if (!module.evaluateContext(context)) {
    return kj::none;
  }
  return module;
};

// If the specifier is "node:process", returns the appropriate internal module
// URL based on the enable_nodejs_process_v2 flag. Otherwise returns kj::none.
kj::Maybe<const Url&> maybeRedirectNodeProcess(Lock& js, kj::ArrayPtr<const char> spec) {
  if (spec == "node:process"_kjb.asChars()) {
    static const auto publicProcess = "node-internal:public_process"_url;
    static const auto legacyProcess = "node-internal:legacy_process"_url;
    return isNodeJsProcessV2Enabled(js) ? publicProcess : legacyProcess;
  }
  return kj::none;
}

kj::String specifierToString(jsg::Lock& js, v8::Local<v8::String> spec) {
  // Source files in workers end up being converted to UTF-8 bytes, so if the specifier
  // string contains non-ASCII unicode characters, those will be directly encoded as UTF-8
  // bytes, which unfortunately end up double-encoded if we try to read them using the
  // regular js.toString() method. Doh! Fortunately they come through as one-byte strings,
  // so we can detect that case and handle those correctly here.
  if (spec->ContainsOnlyOneByte()) {
    auto buf = kj::heapArray<char>(spec->Length() + 1);
    spec->WriteOneByteV2(js.v8Isolate, 0, spec->Length(), buf.asBytes().begin(),
        v8::String::WriteFlags::kNullTerminate);
    KJ_ASSERT(buf[buf.size() - 1] == '\0');
    return kj::String(kj::mv(buf));
  }
  return js.toString(spec);
}

// Ensure that the given module has been instantiated or errored.
// If false is returned, then an exception should have been scheduled
// on the isolate.
bool ensureInstantiated(Lock& js,
    v8::Local<v8::Module> module,
    const CompilationObserver& observer,
    const Module& self) {
  return module->GetStatus() != v8::Module::kUninstantiated ||
      self.instantiate(js, module, observer);
}

constexpr ResolveContext::Type moduleTypeToResolveContextType(Module::Type type) {
  switch (type) {
    case Module::Type::BUNDLE: {
      return ResolveContext::Type::BUNDLE;
    }
    case Module::Type::BUILTIN: {
      return ResolveContext::Type::BUILTIN;
    }
    case Module::Type::BUILTIN_ONLY: {
      return ResolveContext::Type::BUILTIN_ONLY;
    }
    case Module::Type::FALLBACK: {
      return ResolveContext::Type::BUNDLE;
    }
  }
  KJ_UNREACHABLE;
}

constexpr ModuleBundle::Type toModuleBuilderType(ModuleBundle::BuiltinBuilder::Type type) {
  switch (type) {
    case ModuleBundle::BuiltinBuilder::Type::BUILTIN:
      return ModuleBundle::Type::BUILTIN;
    case ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY:
      return ModuleBundle::Type::BUILTIN_ONLY;
  }
  KJ_UNREACHABLE;
}

// The implementation of Module for ESM.
class EsModule final: public Module {
 public:
  explicit EsModule(Url id, Type type, Flags flags, kj::ArrayPtr<const char> source)
      : Module(kj::mv(id), type, flags | Flags::ESM | Flags::EVAL),
        source(source),
        cachedData(kj::none) {
    KJ_DASSERT(isEsm());
  }
  KJ_DISALLOW_COPY_AND_MOVE(EsModule);

  v8::MaybeLocal<v8::Module> getDescriptor(
      Lock& js, const CompilationObserver& observer) const override {
    auto metrics = observer.onEsmCompilationStart(js.v8Isolate, kj::str(id().getHref()),
        type() == Type::BUNDLE ? CompilationObserver::Option::BUNDLE
                               : CompilationObserver::Option::BUILTIN);

    static constexpr int resourceLineOffset = 0;
    static constexpr int resourceColumnOffset = 0;
    static constexpr bool resourceIsSharedCrossOrigin = false;
    static constexpr int scriptId = -1;
    static constexpr bool resourceIsOpaque = false;
    static constexpr bool isWasm = false;
    v8::ScriptOrigin origin(js.str(id().getHref()), resourceLineOffset, resourceColumnOffset,
        resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque, isWasm, true);

    auto options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
    bool cacheWasRejected = false;

    v8::Local<v8::Module> module;
    {
      v8::ScriptCompiler::CachedData* data = nullptr;

      // Check to see if we have cached compilation data for this module.
      // Importantly, we want to allow multiple threads to be capable of
      // reading and using the cached data without blocking each other
      // (which is fine since using the cache does not modify it).
      auto lock = cachedData.lockShared();
      KJ_IF_SOME(c, *lock) {
        // We new new here because v8 will take ownership of the CachedData instance,
        // even tho we are maintaining ownership of the underlying buffer.
        data = new v8::ScriptCompiler::CachedData(
            c->data, c->length, v8::ScriptCompiler::CachedData::BufferPolicy::BufferNotOwned);
        auto check = data->CompatibilityCheck(js.v8Isolate);
        if (check != v8::ScriptCompiler::CachedData::kSuccess) {
          // The cached data is not compatible with the current isolate. Let's
          // not try using it.
          delete data;
          data = nullptr;
        } else {
          observer.onCompileCacheFound(js.v8Isolate);
        }
      }

      // Note that the Source takes ownership of the CachedData pointer that we pass in.
      // (but not the actual buffer it holds). Do not use data after this point.
      v8::ScriptCompiler::Source source(js.strExtern(this->source), origin, data);

      auto maybeCached = source.GetCachedData();
      if (maybeCached != nullptr) {
        if (!maybeCached->rejected) {
          // We found valid cached data and set the option to consume it to avoid
          // compiling again below...
          options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
        } else {
          // In this case we'll just log a warning and continue on. This is potentially
          // a signal that something with the compile cache is not working correctly but
          // it is not a fatal error. If we spot this in the wild, it warrants some
          // investigation but is not critical.
          LOG_WARNING_ONCE("NOSENTRY Cached data for an ESM module was rejected");
          observer.onCompileCacheRejected(js.v8Isolate);
          cacheWasRejected = true;
        }
      }

      // Let's just double check that our options are valid. They should be
      // since we're either consuming cached data or not using any options at all.
      KJ_ASSERT(v8::ScriptCompiler::CompileOptionsIsValid(options));
      if (!v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, options).ToLocal(&module)) {
        return {};
      }
    }

    // If the cached data was rejected, clear it so subsequent isolates don't
    // repeatedly check stale data. We then fall through to regenerate the cache
    // below. In practice this is exceedingly unlikely since V8 version changes
    // are the primary cause of cache rejection and we don't change V8 versions
    // within a single binary, but we handle it for correctness.
    if (cacheWasRejected) {
      auto lock = cachedData.lockExclusive();
      *lock = kj::none;
    }

    // If options is still kNoCompileOptions at this point, it means that we did not
    // find any cached data for this module, or the cached data was rejected. In
    // either case, we try generating it and store it. Multiple threads can end up
    // lining up here to acquire the lock and generate the cache. We'll test to see
    // if the cached data is still empty once the lock is acquired, and if it is
    // not, we'll skip generation.
    if (options == v8::ScriptCompiler::CompileOptions::kNoCompileOptions) {
      auto lock = cachedData.lockExclusive();
      if (*lock == kj::none) {
        if (auto ptr = v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript())) {
          // Using the technically private kj::_::HeapDisposer to wrap the V8-allocated
          // CachedData in a kj::Own. This pattern has precedent in io-own.h.
          kj::Own<v8::ScriptCompiler::CachedData> cached(
              ptr, kj::_::HeapDisposer<v8::ScriptCompiler::CachedData>::instance);
          *lock = kj::mv(cached);
          observer.onCompileCacheGenerated(js.v8Isolate);
        } else {
          observer.onCompileCacheGenerationFailed(js.v8Isolate);
        }
      }
    }

    return module;
  }

 private:
  v8::MaybeLocal<v8::Value> actuallyEvaluate(
      Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const override {
    return module->Evaluate(js.v8Context());
  }

  v8::MaybeLocal<v8::Value> evaluate(Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer,
      const Evaluator& maybeEvaluate) const override {
    if (!ensureInstantiated(js, module, observer, *this)) {
      if (!js.v8Isolate->HasPendingException()) {
        js.v8Isolate->ThrowError(js.str("Failed to instantiate module"_kj));
      }
      return {};
    }

    KJ_IF_SOME(result, maybeEvaluate(js, *this, module, observer)) {
      v8::Local<v8::Value> val = result;
      return val;
    }

    return actuallyEvaluate(js, module, observer);
  }

  kj::ArrayPtr<const char> source;

  // The cachedData holds the cached compilation data for this module, if any. It is
  // generated on-demand the first time the module is compiled, if possible.
  kj::MutexGuarded<kj::Maybe<kj::Own<v8::ScriptCompiler::CachedData>>> cachedData;
};

// A SyntheticModule is essentially any type of module that is not backed by an ESM
// script. More specifically, it's a module in which we synthetically construct the
// module namespace (i.e. the exports) and the evaluation steps. This is used for things
// like CommonJS modules, JSON modules, etc.
class SyntheticModule final: public Module {
 public:
  // The name of the default export.
  static constexpr auto DEFAULT = "default"_kjc;

  SyntheticModule(Url id,
      Type type,
      ModuleBundle::BundleBuilder::EvaluateCallback callback,
      kj::Array<kj::String> namedExports,
      Flags flags = Flags::NONE,
      ContentType contentType = ContentType::NONE)
      : Module(kj::mv(id), type, flags, contentType),
        callback(kj::mv(callback)),
        namedExports(kj::mv(namedExports)) {
    // Synthetic modules can never be ESM or Main
    KJ_DASSERT(!isEsm() && !isMain());
  }

  v8::MaybeLocal<v8::Module> getDescriptor(Lock& js, const CompilationObserver&) const override {
    // We add one to the size to accomodate the default export.
    v8::LocalVector<v8::String> exports(js.v8Isolate, namedExports.size() + 1);
    int n = 0;
    exports[n++] = js.strIntern(DEFAULT);
    for (const auto& exp: namedExports) {
      exports[n++] = js.strIntern(exp);
    }
    return v8::Module::CreateSyntheticModule(js.v8Isolate, js.str(id().getHref()),
        v8::MemorySpan<const v8::Local<v8::String>>(exports.data(), exports.size()),
        evaluationSteps);
  }

 private:
  static v8::MaybeLocal<v8::Value> evaluationSteps(
      v8::Local<v8::Context> context, v8::Local<v8::Module> module);

  v8::MaybeLocal<v8::Value> actuallyEvaluate(
      Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const override {
    // The return value will be a resolved promise.
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver)) {
      return {};
    }

    ModuleNamespace ns(module, namedExports);
    if (!callback(js, id(), ns, observer) ||
        resolver->Resolve(js.v8Context(), js.v8Undefined()).IsNothing()) {
      // An exception should already be scheduled with the isolate
      return {};
    }

    return resolver->GetPromise();
  }

  v8::MaybeLocal<v8::Value> evaluate(Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer,
      const Evaluator& maybeEvaluate) const override {
    if (!ensureInstantiated(js, module, observer, *this)) {
      if (!js.v8Isolate->HasPendingException()) {
        js.v8Isolate->ThrowError(js.str("Failed to instantiate module"_kj));
      }
      return {};
    }
    // If this synthetic module is marked with Flags::EVAL, and the evalCallback
    // is specified, then we defer evaluation to the given callback.
    if (isEval()) {
      KJ_IF_SOME(result, maybeEvaluate(js, *this, module, observer)) {
        v8::Local<v8::Value> val = result;
        return val;
      }
    }
    return module->Evaluate(js.v8Context());
  }

  // Marked mutable because kj::Function::operator() is non-const, but evaluation
  // callbacks are conceptually const — they produce new JS objects each time without
  // modifying the module's logical state. The callback is only ever invoked while
  // holding the isolate lock, so concurrent mutation is not a concern.
  mutable ModuleBundle::BundleBuilder::EvaluateCallback callback;
  kj::Array<kj::String> namedExports;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
WD_STRONG_BOOL(SourcePhase);
#pragma clang diagnostic pop

// Parses import attributes from V8's FixedArray format (key-value-location triples).
// Returns the value of the "type" attribute if present, or kj::none if no attributes.
// Throws TypeError for any unrecognized attribute keys or unsupported type values.
kj::Maybe<kj::StringPtr> parseImportAttributes(
    Lock& js, v8::Local<v8::FixedArray> import_attributes) {
  if (import_attributes.IsEmpty() || import_attributes->Length() == 0) {
    return kj::none;
  }
  // V8 encodes import attributes as a FixedArray of triples: [key, value, location, ...]
  kj::Maybe<kj::StringPtr> typeValue;
  for (int i = 0; i < import_attributes->Length(); i += 3) {
    auto key = js.toString(import_attributes->Get(i).As<v8::String>());
    if (key == "type"_kj) {
      auto value = js.toString(import_attributes->Get(i + 1).As<v8::String>());
      if (value == "json"_kj) {
        typeValue = "json"_kjc;
      } else if (value == "text"_kj) {
        typeValue = "text"_kjc;
      } else if (value == "bytes"_kj) {
        typeValue = "bytes"_kjc;
      } else {
        js.throwException(
            js.typeError(kj::str("Unsupported import attribute type: \"", value, "\"")));
      }
    } else {
      js.throwException(js.typeError(kj::str("Unsupported import attribute: \"", key, "\"")));
    }
  }
  return typeValue;
}

// Validates that the resolved module's content type matches the import attribute "type" value.
// Throws TypeError on mismatch. Does nothing if no type attribute was specified.
void validateImportType(
    Lock& js, kj::Maybe<kj::StringPtr> importType, const Module& module, kj::StringPtr specifier) {
  KJ_IF_SOME(type, importType) {
    // Import Text (TC39 Stage 3) and Import Bytes (TC39 Stage 2.7) are
    // recognized but not yet supported. Text support is pending the proposal
    // reaching Stage 4. Bytes support requires Uint8Array backed by an
    // immutable ArrayBuffer, which is not yet implemented.
    if (type == "text"_kj) {
      js.throwException(js.typeError("Import attribute type \"text\" is not yet supported"_kj));
    }
    if (type == "bytes"_kj) {
      js.throwException(js.typeError("Import attribute type \"bytes\" is not yet supported"_kj));
    }

    Module::ContentType expected = Module::ContentType::NONE;
    if (type == "json"_kj) {
      expected = Module::ContentType::JSON;
    }
    // TODO(later): Enable when Import Text (TC39) reaches Stage 4.
    // else if (type == "text"_kj) {
    //   expected = Module::ContentType::TEXT;
    // }
    // TODO(later): Enable when immutable ArrayBuffer is implemented.
    // else if (type == "bytes"_kj) {
    //   expected = Module::ContentType::DATA;
    // }
    if (module.contentType() != expected) {
      js.throwException(
          js.typeError(kj::str("Module \"", specifier, "\" is not of type \"", type, "\"")));
    }
  }
}

// Binds a ModuleRegistry to an Isolate.
class IsolateModuleRegistry final {
 public:
  static IsolateModuleRegistry& from(v8::Isolate* isolate) {
    return KJ_ASSERT_NONNULL(jsg::getAlignedPointerFromEmbedderData<IsolateModuleRegistry>(
        isolate->GetCurrentContext(), jsg::ContextPointerSlot::MODULE_REGISTRY));
  }

  struct SpecifierContext final {
    ResolveContext::Type type;
    Url id;
    SpecifierContext(const ResolveContext& resolveContext)
        : type(resolveContext.type),
          id(resolveContext.normalizedSpecifier.clone()) {}
    bool operator==(const SpecifierContext& other) const {
      return type == other.type && id == other.id;
    }
    uint hashCode() const {
      return kj::hashCode(type, id);
    }
  };

  struct Entry final {
    HashableV8Ref<v8::Module> key;
    SpecifierContext context;
    const Module& module;
  };

  IsolateModuleRegistry(
      Lock& js, const ModuleRegistry& registry, const CompilationObserver& observer);
  KJ_DISALLOW_COPY_AND_MOVE(IsolateModuleRegistry);

  // Used to implement the normal static import of modules (using `import ... from`).
  // Returns the v8::Module descriptor. If an empty v8::MaybeLocal is returned, then
  // an exception has been scheduled with the isolate.
  v8::MaybeLocal<v8::Module> resolve(Lock& js, const ResolveContext& context) {
    // Do we already have a cached module for this context?
    KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
      return found.key.getHandle(js);
    }
    // No? That's OK, let's look it up.
    KJ_IF_SOME(found, resolveWithCaching(js, context)) {
      return found.key.getHandle(js);
    }

    // Nothing found? Aw... fail!
    JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", context.normalizedSpecifier.getHref()));
  }

  // Used to implement the async dynamic import of modules (using `await import(...)`)
  // Returns a promise that is resolved once the module is resolved. If any empty
  // v8::MaybeLocal is returned, then an exception has been scheduled with the isolate.
  v8::MaybeLocal<v8::Promise> dynamicResolve(Lock& js,
      Url normalizedSpecifier,
      Url referrer,
      kj::StringPtr rawSpecifier,
      SourcePhase sourcePhase,
      kj::Maybe<kj::StringPtr> importType = kj::none) {
    // Note: Takes v8::Local<v8::Module> and const Module& directly rather than
    // Entry& for the same reason as require()'s evaluate lambda — the lookupCache
    // table may rehash during evaluate(), invalidating Entry& references.
    static constexpr auto evaluate =
        [](Lock& js, v8::Local<v8::Module> module, const Module& moduleDef,
            const CompilationObserver& observer, const Module::Evaluator& maybeEvaluate) {
      return js
          .toPromise(
              check(moduleDef.evaluate(js, module, observer, maybeEvaluate)).As<v8::Promise>())
          .then(js, [module = js.v8Ref(module)](Lock& js, Value) mutable -> Promise<Value> {
        return js.resolvedPromise(js.v8Ref(module.getHandle(js)->GetModuleNamespace()));
      });
    };

    return js.wrapSimplePromise(js.tryCatch([&] -> Promise<Value> {
      // The referrer should absolutely already be known to the registry
      // or something bad happened.
      auto& referring = JSG_REQUIRE_NONNULL(lookupCache.find<kj::HashIndex<UrlCallbacks>>(referrer),
          TypeError, kj::str("Referring module not found in the registry: ", referrer.getHref()));

      // Now that we know the referrer module, we can set the context for the
      // next resolve. In particular, the "type" of the context is determine
      // by the type of the referring module.
      ResolveContext context = {
        .type = moduleTypeToResolveContextType(referring.module.type()),
        .source = ResolveContext::Source::DYNAMIC_IMPORT,
        .normalizedSpecifier = normalizedSpecifier,
        .referrerNormalizedSpecifier = referrer,
        .rawSpecifier = rawSpecifier,
      };

      auto handleFoundModule = [&](Entry& found) -> Promise<Value> {
        // Extract module handle and Module& before calling evaluate, since
        // evaluate may trigger table rehashing that invalidates the Entry&.
        auto v8Module = found.key.getHandle(js);
        auto& moduleDef = found.module;

        // Validate import type attribute against the resolved module's content type.
        validateImportType(js, importType, moduleDef, rawSpecifier);

        if (v8Module->GetStatus() == v8::Module::kErrored) {
          return js.rejectedPromise<Value>(v8Module->GetException());
        }

        auto evaluatePromise =
            evaluate(js, v8Module, moduleDef, getObserver(), inner.getEvaluator());
        auto isWasm = moduleDef.isWasm();

        if (!sourcePhase) {
          return evaluatePromise;
        } else {
          // We only support source phase imports for Wasm modules.
          // Source phase imports provide uninstantiated and unlinked representations for modules, as distinct
          // from module instances. They effectively represent the compiled module without state.
          // WebAssembly.Module is this representation for WebAssembly.
          // JS source module handles as an instance of `ModuleSource` will be supported in due course,
          // but are specified in a different spec ESM Phase Imports (https://github.com/tc39/proposal-esm-phase-imports).
          // For builtins and other synthetic modules, there are currently no plans to make a source phase
          // representation available, so that these would remain with the specified syntax error as
          // implemented below.
          if (isWasm) {
            return evaluatePromise.then(js,
                [normalizedSpecifier = normalizedSpecifier.clone()](
                    Lock& js, Value namespaceValue) -> Value {
              auto moduleNamespace = namespaceValue.getHandle(js).As<v8::Object>();
              v8::Local<v8::Value> defaultExport;
              if (moduleNamespace->Get(js.v8Context(), js.strIntern("default"_kj))
                      .ToLocal(&defaultExport)) {
                if (defaultExport->IsWasmModuleObject()) {
                  return js.v8Ref(defaultExport);
                }
              }
              js.throwException(js.v8Ref(v8::Exception::SyntaxError(
                  js.str(kj::str("Source phase import not available for module: "_kj,
                      normalizedSpecifier.getHref())))));
            });
          }
          return js.rejectedPromise<Value>(js.v8Ref(v8::Exception::SyntaxError(js.strIntern(kj::str(
              "Source phase import not available for module: ", normalizedSpecifier.getHref())))));
        }
      };

      // Do we already have a cached module for this context?
      KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
        return handleFoundModule(found);
      }

      // No? That's OK, let's look it up.
      KJ_IF_SOME(found, resolveWithCaching(js, context)) {
        return handleFoundModule(found);
      }

      // Nothing found? Aw... fail!
      JSG_FAIL_REQUIRE(TypeError, kj::str("Module not found: ", normalizedSpecifier.getHref()));
    }, [&](Value exception) -> Promise<Value> {
      return js.rejectedPromise<Value>(kj::mv(exception));
    }));
  }

  enum class RequireOption {
    DEFAULT = 0,
    RETURN_EMPTY = 1 << 0,
    NO_TOP_LEVEL_AWAIT = 1 << 1,
    // When set, the default export is returned instead of the module namespace.
    // This matches Node.js require() semantics where require() returns the
    // default export (module.exports for CJS, default export for ESM builtins,
    // parsed value for JSON, etc.).
    UNWRAP_DEFAULT = 1 << 2,
  };

  friend constexpr RequireOption operator|(RequireOption a, RequireOption b) {
    return static_cast<RequireOption>(static_cast<int>(a) | static_cast<int>(b));
  }
  friend constexpr RequireOption operator&(RequireOption a, RequireOption b) {
    return static_cast<RequireOption>(static_cast<int>(a) & static_cast<int>(b));
  }

  // Used to implement the synchronous dynamic import of modules in support of APIs
  // like the CommonJS require. Returns the instantiated/evaluated module namespace.
  // If an empty v8::MaybeLocal is returned and the default option is given, then an
  // exception has been scheduled.
  v8::MaybeLocal<v8::Value> require(
      Lock& js, const ResolveContext& context, RequireOption option = RequireOption::DEFAULT) {
    // Returns either the module namespace or, when UNWRAP_DEFAULT is set and
    // the module is not ESM, the default export from the namespace. This matches
    // Node.js require() semantics: require('esm') returns the namespace,
    // require('data.json') returns the parsed value.
    // When UNWRAP_DEFAULT is set, returns the default export for all module types
    // except user bundle ESM, which returns the namespace (matching Node.js require(esm)
    // behavior). Builtin ESM returns default because workerd wraps CJS-style APIs in
    // ESM default exports. Synthetic modules (CJS, JSON, Text, etc.) return default
    // because that's where their value lives.
    static constexpr auto maybeUnwrapDefault =
        [](Lock& js, v8::Local<v8::Module> module, const Module& moduleDef,
            RequireOption option) -> v8::MaybeLocal<v8::Value> {
      auto ns = module->GetModuleNamespace().As<v8::Object>();
      if ((option & RequireOption::UNWRAP_DEFAULT) == RequireOption::UNWRAP_DEFAULT) {
        // User bundle ESM returns the full namespace, matching Node.js require(esm),
        // unless the module has __cjsUnwrapDefault set (a convention used by bundlers
        // like esbuild when transpiling CJS to ESM), in which case we return the
        // default export.
        if (moduleDef.type() == Module::Type::BUNDLE && moduleDef.isEsm()) {
          auto unwrap = ns->Get(js.v8Context(), js.strIntern("__cjsUnwrapDefault"_kj));
          v8::Local<v8::Value> unwrapValue;
          if (unwrap.ToLocal(&unwrapValue) && unwrapValue->BooleanValue(js.v8Isolate)) {
            return check(ns->Get(js.v8Context(), js.strIntern("default"_kj)));
          }
          return ns;
        }
        // Everything else (builtins, synthetic modules) returns the default export.
        // Note: The default export may be a primitive (e.g. Text module returns a string).
        // We cast to v8::Object here because require() returns MaybeLocal<Object>, but
        // callers immediately convert to JsValue. The cast is safe because v8::Local is
        // just a pointer wrapper.
        return check(ns->Get(js.v8Context(), js.strIntern("default"_kj)));
      }
      return ns;
    };

    // Note: This lambda takes v8::Local<v8::Module> and const Module& directly
    // rather than Entry& because the lookupCache table may rehash during
    // ensureInstantiated() or evaluate() (when V8 resolves static import
    // dependencies via resolveModuleCallback -> resolveWithCaching -> upsert),
    // which would invalidate any Entry& reference into the table.
    static constexpr auto evaluate =
        [](Lock& js, v8::Local<v8::Module> module, const Module& moduleDef, const Url& id,
            const CompilationObserver& observer, const Module::Evaluator& maybeEvaluate,
            RequireOption option) -> v8::MaybeLocal<v8::Value> {
      auto status = module->GetStatus();

      // If status is kErrored, that means a prior attempt to evaluate the module
      // failed. We simply propagate the same error here.
      if (status == v8::Module::kErrored) {
        js.throwException(JsValue(module->GetException()));
      }

      // Circular dependencies should be fine when we are talking strictly
      // about CJS/Node.js style modules. For ESM, it becomes more problematic
      // because v8 will not allow us to grab the default export while the module
      // is still evaluating.

      if (moduleDef.isEsm() && status == v8::Module::kEvaluating) {
        JSG_FAIL_REQUIRE(Error, "Circular dependency when resolving module: ", id);
      }

      // If the module has already been evaluated, or is in the process of being
      // evaluated, return the module namespace object directly. Note that if the
      // module is a synthetic module, and status is kEvaluating, it is possible
      // and likely that the namespace has not yet been fully evaluated and will
      // be incomplete here. This allows CJS circular dependencies to be supported
      // to a degree. Just like in Node.js, however, such circular dependencies
      // can still be problematic depending on how they are used.
      if (status == v8::Module::kEvaluated || status == v8::Module::kEvaluating) {
        return maybeUnwrapDefault(js, module, moduleDef, option);
      }

      // Matches the require(esm) behavior implemented in Node.js, which is to
      // throw if the module being imported uses top-level await.
      if ((option & RequireOption::NO_TOP_LEVEL_AWAIT) == RequireOption::NO_TOP_LEVEL_AWAIT) {
        // We have to ensure the module is instantiated before we can check for top-level await.
        JSG_REQUIRE(ensureInstantiated(js, module, observer, moduleDef), Error,
            "Failed to instantiate module: ", id);
        JSG_REQUIRE(!module->IsGraphAsync(), Error,
            "Top-level await is not supported in this context for module: ", id);
      }

      // Evaluate the module and grab the default export from the module namespace.
      auto promise =
          check(moduleDef.evaluate(js, module, observer, maybeEvaluate)).As<v8::Promise>();

      // Run the microtasks to ensure that any promises that happen to be scheduled
      // during the evaluation of the top-level scope have a chance to be settled.
      // We only pump the microtasks queue if NO_TOP_LEVEL_AWAIT is not set.
      if ((option & RequireOption::NO_TOP_LEVEL_AWAIT) != RequireOption::NO_TOP_LEVEL_AWAIT) {
        js.runMicrotasks();

        static const auto kTopLevelAwaitError =
            "Use of top-level await in a synchronously required module is restricted to "
            "promises that are resolved synchronously. This includes any top-level awaits "
            "in the entrypoint module for a worker."_kj;

        switch (promise->State()) {
          case v8::Promise::kFulfilled: {
            // This is what we want. The module namespace should be fully populated
            // and evaluated at this point.
            return maybeUnwrapDefault(js, module, moduleDef, option);
          }
          case v8::Promise::kRejected: {
            // Oops, there was an error. We should throw it.
            js.throwException(JsValue(promise->Result()));
            break;
          }
          case v8::Promise::kPending: {
            // The module evaluation could not complete in a single drain of the
            // microtask queue. This means we've got a pending promise somewhere
            // that is being awaited preventing the module from being ready to
            // go. We can't have that! Throw! Throw!
            JSG_FAIL_REQUIRE(Error, kTopLevelAwaitError, " Specifier: \"", id, "\".");
          }
        }
      } else {
        KJ_ASSERT(!module->IsGraphAsync() && promise->State() != v8::Promise::kPending,
            "Top-level await is not supported in this context, so the module promise "
            "should never be pending");
        if (promise->State() == v8::Promise::kRejected) {
          js.throwException(JsValue(promise->Result()));
        }
        return maybeUnwrapDefault(js, module, moduleDef, option);
      }
      KJ_UNREACHABLE;
    };

    return js.tryCatch([&]() -> v8::MaybeLocal<v8::Value> {
      KJ_IF_SOME(processUrl, maybeRedirectNodeProcess(js, context.normalizedSpecifier.getHref())) {
        ResolveContext newContext{
          .type = ResolveContext::Type::BUILTIN_ONLY,
          .source = context.source,
          .normalizedSpecifier = processUrl,
          .referrerNormalizedSpecifier = context.referrerNormalizedSpecifier,
          .rawSpecifier = context.rawSpecifier,
        };
        return require(js, newContext, option);
      }

      // Do we already have a cached module for this context?
      KJ_IF_SOME(found, lookupCache.find<kj::HashIndex<ContextCallbacks>>(context)) {
        // Extract module handle and Module& before calling evaluate, since
        // evaluate may trigger table rehashing that invalidates the Entry&.
        auto foundModule = found.key.getHandle(js);
        auto& foundModuleDef = found.module;
        return evaluate(js, foundModule, foundModuleDef, context.normalizedSpecifier, getObserver(),
            inner.getEvaluator(), option);
      }

      KJ_IF_SOME(found, resolveWithCaching(js, context)) {
        auto foundModule = found.key.getHandle(js);
        auto& foundModuleDef = found.module;
        return evaluate(js, foundModule, foundModuleDef, context.normalizedSpecifier, getObserver(),
            inner.getEvaluator(), option);
      }

      if ((option & RequireOption::RETURN_EMPTY) == RequireOption::RETURN_EMPTY) {
        return {};
      }
      JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", context.normalizedSpecifier.getHref()));
    }, [&](Value exception) -> v8::MaybeLocal<v8::Object> {
      // Use the isolate to rethrow the exception here instead of using the lock.
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return {};
    });
  }

  // Lookup a module that may have already been previously resolved and cached.
  kj::Maybe<Entry&> lookup(Lock& js, v8::Local<v8::Module> module) {
    return lookupCache
        .find<kj::HashIndex<EntryCallbacks>>(HashableV8Ref<v8::Module>(js.v8Isolate, module))
        .map([](Entry& entry) -> Entry& { return entry; });
  }

  const jsg::Url& getBundleBase() const {
    return inner.getBundleBase();
  }

 private:
  const ModuleRegistry& inner;
  const CompilationObserver& observer;

  const CompilationObserver& getObserver() const {
    return observer;
  }

  struct EntryCallbacks final {
    const HashableV8Ref<v8::Module>& keyForRow(const Entry& entry) const {
      return entry.key;
    }
    bool matches(const Entry& entry, const HashableV8Ref<v8::Module>& key) const {
      return entry.key == key;
    }
    uint hashCode(const HashableV8Ref<v8::Module>& ref) const {
      return ref.hashCode();
    }
  };

  struct ContextCallbacks final {
    const SpecifierContext& keyForRow(const Entry& entry) const {
      return entry.context;
    }
    bool matches(const Entry& entry, const SpecifierContext& context) const {
      return entry.context == context;
    }
    uint hashCode(const SpecifierContext& context) const {
      return context.hashCode();
    }
  };

  struct UrlCallbacks final {
    const Url& keyForRow(const Entry& entry) const {
      return entry.context.id;
    }
    bool matches(const Entry& entry, const Url& id) const {
      return entry.context.id == id;
    }
    uint hashCode(const Url& id) const {
      return id.hashCode();
    }
  };

  // Resolves the module from the inner ModuleRegistry, caching the results.
  kj::Maybe<Entry&> resolveWithCaching(
      Lock& js, const ResolveContext& context) KJ_WARN_UNUSED_RESULT {
    // Clone attributes so the fallback bundle callback can see them.
    kj::HashMap<kj::StringPtr, kj::StringPtr> clonedAttrs;
    for (const auto& [key, value]: context.attributes) {
      clonedAttrs.insert(key, value);
    }
    ResolveContext innerContext{
      // The type identifies the resolution context as a bundle, builtin, or builtin-only.
      .type = context.type,
      // The source identifies the method of resolution (static import, dynamic import, etc).
      // This is passed along for informational purposes only.
      .source = context.source,
      // The inner registry should ignore all URL query parameters and fragments
      .normalizedSpecifier = context.normalizedSpecifier.clone(
          Url::EquivalenceOption::IGNORE_FRAGMENTS | Url::EquivalenceOption::IGNORE_SEARCH),
      // The referrer is passed along for informational purposes only.
      .referrerNormalizedSpecifier = context.referrerNormalizedSpecifier,
      // The raw specifier and attributes are passed along for informational purposes
      // (used by the fallback service protocol).
      .rawSpecifier = context.rawSpecifier,
      .attributes = kj::mv(clonedAttrs),
    };

    KJ_IF_SOME(found, inner.lookup(innerContext)) {
      return kj::Maybe<Entry&>(lookupCache.upsert(
          Entry{
            .key = HashableV8Ref<v8::Module>(
                js.v8Isolate, check(found.getDescriptor(js, getObserver()))),
            // Note that we cache specifically with the passed in context and not the
            // innerContext that was created. This is because we want to use the original
            // specifier URL (with query parameters and fragments) as part of the key for
            // the lookup cache.
            .context = context,
            .module = found,
          },
          [](auto&, auto&&) {}));
    }
    return kj::none;
  }

  kj::Table<Entry,
      kj::HashIndex<EntryCallbacks>,
      kj::HashIndex<ContextCallbacks>,
      kj::HashIndex<UrlCallbacks>>
      lookupCache;
  friend class SyntheticModule;
};

v8::MaybeLocal<v8::Value> SyntheticModule::evaluationSteps(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
  auto& js = Lock::current();
  KJ_TRY {
    auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
    KJ_IF_SOME(found, registry.lookup(js, module)) {
      return found.module.actuallyEvaluate(js, module, registry.getObserver());
    }
    KJ_LOG(ERROR, "Synthetic module not found in registry for evaluation");
    js.v8Isolate->ThrowError(js.str("Requested module does not exist"_kj));
    return {};
  }
  KJ_CATCH(exception) {
    auto ex = js.exceptionToJsValue(kj::mv(exception));
    js.v8Isolate->ThrowException(ex.getHandle(js));
    return {};
  }
}

// Set up the special `import.meta` property for the module.
void importMeta(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module, v8::Local<v8::Object> meta) {
  auto& js = Lock::current();
  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  try {
    js.tryCatch([&] {
      KJ_IF_SOME(found, registry.lookup(js, module)) {
        auto href = found.context.id.getHref();

        // V8's documentation says that the host should set the properties
        // using CreateDataProperty.

        if (meta->CreateDataProperty(js.v8Context(), v8::Local<v8::String>(js.strIntern("main"_kj)),
                    js.boolean(found.module.isMain()))
                .IsNothing()) {
          // Notice that we do not use check here. There should be an exception
          // scheduled with the isolate, it will take care of it at this point.
          return;
        }

        if (meta->CreateDataProperty(
                    js.v8Context(), v8::Local<v8::String>(js.strIntern("url"_kj)), js.str(href))
                .IsNothing()) {
          return;
        }

        // The import.meta.resolve(...) function is effectively a shortcut for
        // new URL(specifier, import.meta.url).href. The idea is that it allows
        // resolving import specifiers relative to the current modules base URL.
        // Note that we do not validate that the resolved URL actually matches
        // anything in the registry.
        auto resolve = js.wrapReturningFunction(js.v8Context(),
            [href = kj::mv(href)](
                Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) -> JsValue {
          // Note that we intentionally use ToString here to coerce whatever value is given
          // into a string or throw if it cannot be coerced.
          auto specifier = js.toString(args[0]);
          KJ_IF_SOME(resolved, Url::tryParse(specifier.asPtr(), href)) {
            auto normalized = resolved.clone(Url::EquivalenceOption::NORMALIZE_PATH);
            return js.str(normalized.getHref());
          } else {
            // If the specifier could not be parsed and resolved successfully,
            // the spec says to return null.
            return js.null();
          }
        });

        if (meta->CreateDataProperty(
                    js.v8Context(), v8::Local<v8::String>(js.strIntern("resolve"_kj)), resolve)
                .IsNothing()) {
          return;
        }
      }
    }, [&](Value exception) {
      // It would be exceedingly odd to end up here but we handle it anyway,
      // just to ensure that we do not crash the isolate. The only thing we'll
      // do is rethrow the error.
      js.v8Isolate->ThrowException(exception.getHandle(js));
    });
  } catch (...) {
    kj::throwFatalException(kj::getCaughtExceptionAsKj());
  }
}

// Templated implementation for both evaluation and source phase dynamic imports
v8::MaybeLocal<v8::Promise> dynamicImportModuleCallback(v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    SourcePhase isSourcePhase,
    v8::Local<v8::FixedArray> import_attributes) {
  auto& js = Lock::current();

  // Since this method is called directly by V8, we don't want to use jsg::check
  // or the js.rejectedPromise variants since those can throw JsExceptionThrown.
  constexpr static auto rejected = [](jsg::Lock& js,
                                       const jsg::JsValue& error) -> v8::MaybeLocal<v8::Promise> {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver) ||
        resolver->Reject(js.v8Context(), error).IsNothing()) {
      return {};
    }
    return resolver->GetPromise();
  };

  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  KJ_TRY {
    return js.tryCatch([&]() -> v8::MaybeLocal<v8::Promise> {
      auto spec = specifierToString(js, specifier);

      // Parse import attributes. Throws for unrecognized attribute keys.
      // Returns the "type" value if specified, or kj::none.
      auto importType = parseImportAttributes(js, import_attributes);

      Url referrer = ([&] {
        if (resource_name.IsEmpty()) {
          return registry.getBundleBase().clone();
        }
        auto str = js.toString(resource_name);
        return KJ_ASSERT_NONNULL(Url::tryParse(str.asPtr()));
      })();

      // If Node.js Compat v2 mode is enable, we have to check to see if the specifier
      // is a bare node specifier and resolve it to a full node: URL.
      if (isNodeJsCompatEnabled(js)) {
        KJ_IF_SOME(nodeSpec, checkNodeSpecifier(spec)) {
          spec = kj::mv(nodeSpec);
        }
      }

      // Handle process module redirection based on enable_nodejs_process_v2 flag
      KJ_IF_SOME(processUrl, maybeRedirectNodeProcess(js, spec.asPtr())) {
        auto processSpec = kj::str(processUrl.getHref());
        return registry.dynamicResolve(
            js, processUrl.clone(), kj::mv(referrer), processSpec, isSourcePhase, importType);
      }

      KJ_IF_SOME(url, referrer.tryResolve(spec.asPtr())) {
        return registry.dynamicResolve(js, url.clone(Url::EquivalenceOption::NORMALIZE_PATH),
            kj::mv(referrer), spec, isSourcePhase, importType);
      }

      // We were not able to parse the specifier. We'll return a rejected promise.
      return rejected(js, js.typeError(kj::str("Invalid module specifier: ", spec)));
    }, [&](Value exception) -> v8::MaybeLocal<v8::Promise> {
      // If there are any synchronously thrown exceptions, we want to catch them
      // here and convert them into a rejected promise. The only exception are
      // fatal cases where the isolate is terminating which won't make it here
      // anyway.
      return rejected(js, jsg::JsValue(exception.getHandle(js)));
    });
  }
  KJ_CATCH(exception) {
    auto ex = js.exceptionToJsValue(kj::mv(exception));
    return rejected(js, ex.getHandle(js));
  }
}

// Wrapper functions to match the V8 callback signatures
v8::MaybeLocal<v8::Promise> dynamicImport(v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes) {
  return dynamicImportModuleCallback(
      context, host_defined_options, resource_name, specifier, SourcePhase::NO, import_attributes);
}

v8::MaybeLocal<v8::Promise> dynamicImportWithPhase(v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::ModuleImportPhase phase,
    v8::Local<v8::FixedArray> import_attributes) {
  SourcePhase sourcePhase =
      (phase == v8::ModuleImportPhase::kSource) ? SourcePhase::YES : SourcePhase::NO;
  return dynamicImportModuleCallback(
      context, host_defined_options, resource_name, specifier, sourcePhase, import_attributes);
}

IsolateModuleRegistry::IsolateModuleRegistry(
    Lock& js, const ModuleRegistry& registry, const CompilationObserver& observer)
    : inner(registry),
      observer(observer),
      lookupCache(EntryCallbacks{}, ContextCallbacks{}, UrlCallbacks{}) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  KJ_ASSERT(!context.IsEmpty());
  setAlignedPointerInEmbedderData(context, ContextPointerSlot::MODULE_REGISTRY, this);
  isolate->SetHostImportModuleDynamicallyCallback(&dynamicImport);
  isolate->SetHostImportModuleWithPhaseDynamicallyCallback(&dynamicImportWithPhase);
  isolate->SetHostInitializeImportMetaObjectCallback(&importMeta);
}

// Generalized module resolution callback that handles both evaluation and source phase imports
template <bool IsSourcePhase>
v8::MaybeLocal<std::conditional_t<IsSourcePhase, v8::Object, v8::Module>> resolveModuleCallback(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_attributes,
    v8::Local<v8::Module> referrer) {
  using ReturnType = std::conditional_t<IsSourcePhase, v8::Object, v8::Module>;
  auto& js = Lock::current();
  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);

  return js.tryCatch([&]() -> v8::MaybeLocal<ReturnType> {
    auto spec = specifierToString(js, specifier);

    // The proposed specification for import attributes strongly recommends that
    // embedders reject import attributes and types they do not understand/implement.
    // This is because import attributes can alter the interpretation of a module.
    // Throwing an error for things we do not understand is the safest thing to do
    // for backwards compatibility.
    //
    // Parse import attributes. Throws for unrecognized attribute keys.
    auto importType = parseImportAttributes(js, import_attributes);

    ResolveContext::Type type = ResolveContext::Type::BUNDLE;

    // Clone the referrer URL out of the lookup cache entry rather than holding
    // a reference into it. The lookupCache table may rehash during resolve()
    // (via resolveWithCaching -> upsert), which would invalidate any reference
    // into the table's storage.
    Url referrerUrl = registry.lookup(js, referrer)
                          .map([&](IsolateModuleRegistry::Entry& entry) -> Url {
      type = moduleTypeToResolveContextType(entry.module.type());
      return entry.context.id.clone();
    }).orDefault(registry.getBundleBase().clone());

    // If Node.js Compat v2 mode is enable, we have to check to see if the specifier
    // is a bare node specifier and resolve it to a full node: URL.
    if (isNodeJsCompatEnabled(js)) {
      KJ_IF_SOME(nodeSpec, checkNodeSpecifier(spec)) {
        spec = kj::mv(nodeSpec);
      }
    }

    // Handle process module redirection based on enable_nodejs_process_v2 flag
    if constexpr (!IsSourcePhase) {
      KJ_IF_SOME(processUrl, maybeRedirectNodeProcess(js, spec.asPtr())) {
        auto processSpec = kj::str(processUrl.getHref());
        ResolveContext resolveContext = {
          .type = ResolveContext::Type::BUILTIN_ONLY,
          .source = ResolveContext::Source::STATIC_IMPORT,
          .normalizedSpecifier = processUrl,
          .referrerNormalizedSpecifier = referrerUrl,
          .rawSpecifier = processSpec.asPtr(),
        };
        auto maybeResolved = registry.resolve(js, resolveContext);
        v8::Local<v8::Module> resolved;
        if (!maybeResolved.ToLocal(&resolved)) {
          return {};
        }
        if (resolved->GetStatus() == v8::Module::kErrored) {
          js.throwException(JsValue(resolved->GetException()));
          return {};
        }
        if (resolved->GetStatus() == v8::Module::kEvaluating) {
          js.throwException(
              js.typeError(kj::str("Circular dependency when resolving module: ", spec)));
          return {};
        }
        // Validate import type attribute against the resolved module's content type.
        KJ_IF_SOME(entry, registry.lookup(js, resolved)) {
          validateImportType(js, importType, entry.module, spec);
        }
        return resolved;
      }
    }

    KJ_IF_SOME(url, referrerUrl.tryResolve(spec)) {
      // Make sure that percent-encoding in the path is normalized so we can match correctly.
      auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
      ResolveContext resolveContext = {
        .type = type,
        .source = ResolveContext::Source::STATIC_IMPORT,
        .normalizedSpecifier = normalized,
        .referrerNormalizedSpecifier = referrerUrl,
        .rawSpecifier = spec.asPtr(),
      };

      auto maybeResolved = registry.resolve(js, resolveContext);

      v8::Local<v8::Module> resolved;
      if (!maybeResolved.ToLocal(&resolved)) {
        return {};
      }

      // If the resolved module is in an errored state, we will rethrow the same exception here.
      if (resolved->GetStatus() == v8::Module::kErrored) {
        js.throwException(JsValue(resolved->GetException()));
        return {};
      }
      if (resolved->GetStatus() == v8::Module::kEvaluating) {
        js.throwException(
            js.typeError(kj::str("Circular dependency when resolving module: ", spec)));
        return v8::MaybeLocal<ReturnType>();
      }

      // Validate import type attribute against the resolved module's content type.
      KJ_IF_SOME(entry, registry.lookup(js, resolved)) {
        validateImportType(js, importType, entry.module, spec);
      }

      if constexpr (!IsSourcePhase) {
        return resolved;
      } else {
        KJ_IF_SOME(entry, registry.lookup(js, resolved)) {
          // We only support source phase imports for Wasm modules.
          // Since we do not have an async pre-instantiation phase which populates compilation,
          // and instead have compilation happening lazily in evaluate calls, we implement this
          // hack to synchronously obtain the compiled Wasm leaning into the require implementation
          // for the Wasm then plucking out the compiled Wasm module.
          // In future, the source phase should be eagerly populated during pre-innstantiation
          // with the compiled record, so that we can just directly read `sourceObject_` off of
          // entry.module instead.
          if (entry.module.isWasm()) {
            v8::Local<v8::Value> moduleNamespace;
            if (registry
                    .require(js, resolveContext, IsolateModuleRegistry::RequireOption::RETURN_EMPTY)
                    .ToLocal(&moduleNamespace)) {
              v8::Local<v8::Value> defaultExport;
              if (moduleNamespace.As<v8::Object>()
                      ->Get(js.v8Context(), js.strIntern("default"_kj))
                      .ToLocal(&defaultExport)) {
                if (defaultExport->IsWasmModuleObject()) {
                  return defaultExport.As<v8::Object>();
                }
              }
            }
            // If require() failed with an exception (e.g. WASM compilation error),
            // propagate that instead of masking it with the generic message below.
            if (js.v8Isolate->HasPendingException()) {
              return {};
            }
          }
        }
        js.throwException(js.v8Ref(v8::Exception::SyntaxError(
            js.str(kj::str("Source phase import not available for module: "_kj, spec)))));
        return {};
      }
      KJ_UNREACHABLE;
    }

    js.throwException(js.error(kj::str("Invalid module specifier: "_kj, specifier)));
    return {};
  }, [&](Value exception) -> v8::MaybeLocal<ReturnType> {
    // If there are any synchronously thrown exceptions, we want to catch them
    // here and convert them into a rejected promise. The only exception are
    // fatal cases where the isolate is terminating which won't make it here
    // anyway.
    js.v8Isolate->ThrowException(exception.getHandle(js));
    return {};
  });
}

// The fallback module bundle calls a single resolve callback to resolve all modules
// it is asked to resolve. Thread safety is provided by the ModuleRegistry's
// MutexGuarded<Impl> exclusive lock — all calls to lookup() are made while
// holding that lock.
class FallbackModuleBundle final: public ModuleBundle {
 public:
  FallbackModuleBundle(Builder::ResolveCallback&& callback)
      : ModuleBundle(Type::FALLBACK),
        callback(kj::mv(callback)) {}

  kj::Maybe<Resolved> lookup(const ResolveContext& context) override {
    // Maybe it's an alias? If so, we just return the aliased specifier.
    // We don't resolve again because the alias might be to a specifier
    // in another bundle. We should start the resolution process over
    // from the start.
    KJ_IF_SOME(found, aliases.find(context.normalizedSpecifier)) {
      return Resolved{
        .specifier = kj::str(found),
      };
    }

    // Maybe it's already cached? If so, we just return the cached module.
    KJ_IF_SOME(found, storage.find(context.normalizedSpecifier)) {
      return Resolved{
        .module = *found,
      };
    }

    // Well, let's actually try to resolve it.
    KJ_IF_SOME(resolved, callback(context)) {
      KJ_SWITCH_ONEOF(resolved) {
        KJ_CASE_ONEOF(str, kj::String) {
          // We got an alias back. Store it and return it, unless it's an alias
          // to itself, in which case return kj::none. It's possible that a buggy
          // fallback resolver could end up in an infinite loop of aliasing.
          if (str == context.normalizedSpecifier.getHref()) {
            return kj::none;
          }
          aliases.insert(context.normalizedSpecifier.clone(), kj::str(str));
          return Resolved{
            .specifier = kj::mv(str),
          };
        }
        KJ_CASE_ONEOF(resolved, kj::Own<Module>) {
          auto& module = *resolved;
          // If the fallback service returned a module with a specifier that
          // already exists in storage, ignore it and return kj::none. We can't
          // have two different modules with the same specifier in the bundle.
          if (storage.find(module.id()) != kj::none) {
            return kj::none;
          }
          storage.insert(module.id().clone(), kj::mv(resolved));
          if (context.normalizedSpecifier != module.id()) {
            // We checked for the existence of the specifier alias above so this
            // insert should always succeed. In debug mode, let's check.
            KJ_DASSERT(aliases.find(context.normalizedSpecifier) == kj::none);
            aliases.insert(context.normalizedSpecifier.clone(), kj::str(module.id().getHref()));
          }
          return Resolved{
            .module = module,
          };
        }
      }
      KJ_UNREACHABLE;
    }

    return kj::none;
  }

 private:
  Builder::ResolveCallback callback;

  kj::HashMap<Url, kj::Own<Module>> storage;
  kj::HashMap<Url, kj::String> aliases;
};

// The static module bundle maintains an internal table of specifiers to resolve callbacks
// in memory. Thread safety is provided by the ModuleRegistry's MutexGuarded<Impl>
// exclusive lock — all calls to lookup() are made while holding that lock.
class StaticModuleBundle final: public ModuleBundle {
 public:
  StaticModuleBundle(Type type,
      kj::HashMap<Url, ModuleBundle::Builder::ResolveCallback> modules,
      kj::HashMap<Url, Url> aliases)
      : ModuleBundle(type),
        modules(kj::mv(modules)),
        aliases(kj::mv(aliases)) {}
  KJ_DISALLOW_COPY_AND_MOVE(StaticModuleBundle);

  kj::Maybe<Resolved> lookup(const ResolveContext& context) override {
    // Is it an alias? If so, we just return the aliased specifier.
    KJ_IF_SOME(aliased, aliases.find(context.normalizedSpecifier)) {
      return Resolved{
        .specifier = kj::str(aliased.getHref()),
      };
    }

    // It's not an alias, maybe it's already cached?
    KJ_IF_SOME(cached, cache.find(context.normalizedSpecifier)) {
      return Resolved{
        .module = checkModule(context, *cached),
      };
    }

    // Not aliased or cached, we need to look it up.
    KJ_IF_SOME(found, modules.find(context.normalizedSpecifier)) {
      KJ_IF_SOME(resolved, found(context)) {
        KJ_SWITCH_ONEOF(resolved) {
          KJ_CASE_ONEOF(str, kj::String) {
            return Resolved{
              .specifier = kj::mv(str),
            };
          }
          KJ_CASE_ONEOF(resolved, kj::Own<Module>) {
            const Module& module = *resolved;
            cache.insert(context.normalizedSpecifier.clone(), kj::mv(resolved));
            return Resolved{
              .module = checkModule(context, module),
            };
          }
        }
        KJ_UNREACHABLE;
      }
    }

    return kj::none;
  }

 private:
  kj::HashMap<Url, ModuleBundle::Builder::ResolveCallback> modules;
  kj::HashMap<Url, Url> aliases;
  kj::HashMap<Url, kj::Own<Module>> cache;
};

kj::HashSet<kj::StringPtr> toHashSet(kj::ArrayPtr<const kj::String> arr) {
  kj::HashSet<kj::StringPtr> set;
  set.insertAll(arr);
  // Make sure there is no "default" export listed explicitly in the set.
  set.eraseMatch("default"_kj);
  return kj::mv(set);
}

}  // namespace

// ======================================================================================
kj::Own<ModuleBundle> ModuleBundle::newFallbackBundle(Builder::ResolveCallback callback) {
  return kj::heap<FallbackModuleBundle>(kj::mv(callback));
}

void ModuleBundle::getBuiltInBundleFromCapnp(BuiltinBuilder& builder, Bundle::Reader bundle) {
  getBuiltInBundleFromCapnp(builder, bundle, [](workerd::jsg::Module::Reader) { return true; });
}

void ModuleBundle::getBuiltInBundleFromCapnp(BuiltinBuilder& builder,
    Bundle::Reader bundle,
    kj::Function<bool(workerd::jsg::Module::Reader)> filter) {
  auto typeFilter = ([&] {
    switch (builder.type()) {
      case Module::Type::BUILTIN:
        return ModuleType::BUILTIN;
      case Module::Type::BUILTIN_ONLY:
        return ModuleType::INTERNAL;
      case Module::Type::BUNDLE:
        break;
      case Module::Type::FALLBACK:
        break;
    }
    KJ_UNREACHABLE;
  })();

  for (auto module: bundle.getModules()) {
    if (module.getType() == typeFilter && filter(module)) {
      auto id = KJ_ASSERT_NONNULL(Url::tryParse(module.getName()));
      switch (module.which()) {
        case workerd::jsg::Module::SRC: {
          builder.addEsm(id, module.getSrc().asChars());
          continue;
        }
        case workerd::jsg::Module::WASM: {
          builder.addSynthetic(id, Module::newWasmModuleHandler(module.getWasm().asBytes()));
          continue;
        }
        case workerd::jsg::Module::DATA: {
          builder.addSynthetic(id, Module::newDataModuleHandler(module.getData().asBytes()));
          continue;
        }
        case workerd::jsg::Module::JSON: {
          builder.addSynthetic(id, Module::newJsonModuleHandler(module.getJson().asArray()));
          continue;
        }
      }
      KJ_UNREACHABLE;
    }
  }
}

ModuleBundle::ModuleBundle(Type type): type_(type) {}

ModuleBundle::Builder::Builder(Type type): type_(type) {}

ModuleBundle::Builder& ModuleBundle::Builder::alias(const Url& alias, const Url& id) {
  auto aliasNormed = alias.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  if (modules_.find(aliasNormed) != kj::none || aliases_.find(aliasNormed) != kj::none) {
    KJ_FAIL_REQUIRE(kj::str("Module \"", aliasNormed.getHref(), "\" already added to bundle"));
  }
  aliases_.insert(kj::mv(aliasNormed), id.clone(Url::EquivalenceOption::NORMALIZE_PATH));
  return *this;
}

ModuleBundle::Builder& ModuleBundle::Builder::add(
    const Url& id, Builder::ResolveCallback callback) {
  if (modules_.find(id) != kj::none || aliases_.find(id) != kj::none) {
    KJ_FAIL_REQUIRE(kj::str("Module \"", id.getHref(), "\" already added to bundle"));
  }
  modules_.insert(id.clone(), kj::mv(callback));
  return *this;
}

kj::Own<ModuleBundle> ModuleBundle::Builder::finish() {
  return kj::heap<StaticModuleBundle>(type_, kj::mv(modules_), kj::mv(aliases_));
}

void ModuleBundle::Builder::ensureIsNotBundleSpecifier(const Url& id) {
  // The file: protocol is reserved for bundle type modules.
  KJ_REQUIRE(
      id.getProtocol() != "file:"_kjc, "The file: protocol is reserved for bundle type modules");
}

// ======================================================================================

ModuleBundle::BundleBuilder::BundleBuilder(const jsg::Url& bundleBase)
    : ModuleBundle::Builder(Type::BUNDLE),
      bundleBase(bundleBase) {}

namespace {
static constexpr auto BUNDLE_CLONE_OPTIONS = jsg::Url::EquivalenceOption::IGNORE_FRAGMENTS |
    jsg::Url::EquivalenceOption::IGNORE_SEARCH | jsg::Url::EquivalenceOption::NORMALIZE_PATH;

// Takes the user-provided module name and normalizes it to a form that can be
// resolved relative to the bundle base. This involves pre-parsing the name as a URL
// relative to a dummy base URL in order to normalize out dot and double-dot segments,
// then stripping off any leading slashes so that the name is always relative and cannot
// be interpreted as an absolute path.
jsg::Url normalizeModuleName(kj::StringPtr name, const jsg::Url& base) {
  // This first step normalizes out path segments like "." and "..", drops query
  // strings and fragments, and normalizes percent-encoding in the path.
  auto url = KJ_ASSERT_NONNULL(base.tryResolve(name)).clone(BUNDLE_CLONE_OPTIONS);

  // If the protocol is not file:, then we don't need to do any more processing
  // here. We will check the validity of the result as a module URL in the next
  // step.
  if (url.getProtocol() != "file:"_kj) {
    return kj::mv(url);
  }

  auto urlPath = url.getPathname();
  auto basePath = base.getPathname();

  // The url path must not be identical to the base...
  KJ_REQUIRE(urlPath != basePath, "Invalid empty module name");

  // If the url path starts with the base path, then we're good!
  if (urlPath.startsWith(basePath)) {
    return kj::mv(url);
  }

  // Otherwise, let's make sure that the url path is processed as
  // relative to the base path. We do this by stripping off any
  // leading slashes from the front of the URL then re-resolve
  // against the base. This should be an exceedingly rare edge
  // case if the worker bundle is being constructed properly. It's
  // meant only to handle cases where silliness like "///foo" is
  // given as a module name.
  while (urlPath.startsWith("/"_kj) && urlPath.size() > 0) {
    urlPath = urlPath.slice(1);
  }
  KJ_REQUIRE(urlPath.size() > 0, "Invalid empty module name");

  return KJ_ASSERT_NONNULL(base.tryResolve(urlPath));
}

bool isValidBundleModuleUrl(const jsg::Url& url, const jsg::Url& base) {
  KJ_DASSERT(base.getProtocol() == "file:"_kj);
  KJ_DASSERT(base.getPathname().endsWith("/"_kj));

  // Let's forbid users from using cloudflare: and workerd: URLs in bundles so that
  // we can protect those namespaces for our own future use. Specifically, these
  // should only be used by the runtime to refer to built-in modules. We don't
  // restrict other non-standard protocols like node:
  KJ_REQUIRE(url.getProtocol() != "cloudflare:"_kj,
      "The cloudflare: protocol is reserved and cannot be used in module bundles");
  KJ_REQUIRE(url.getProtocol() != "workerd:"_kj,
      "The workerd: protocol is reserved and cannot be used in module bundles");

  // Let's forbid data: URL use from module bundles. They are not yet supported
  // by the runtime due to dynamic eval restrictions. Even when we do support them
  // eventually, we want to be able to reserve the data: URL namespace for that
  // use. If we allowed worker bundles to use data: URLs that we could end up
  // requiring a compat flag later to actually properly enable them.
  KJ_REQUIRE(
      url.getProtocol() != "data:"_kj, "The data: protocol cannot be used in module bundles");

  if (url.getProtocol() != "file:"_kj) {
    // Different protocols are always OK
    return true;
  }

  // Module file: URLs must not have a host component.
  // We already know the protocol is "file:" here because of the check above.
  if (url.getHost() != ""_kj) {
    return false;
  }

  // Check if url is subordinate to the base.
  // This means url's path should start with base's path as a prefix
  auto aPath = url.getPathname();
  auto bPath = base.getPathname();

  return aPath.startsWith(bPath);
}

// Converts the name given for a user-bundle module into a fully qualified module url.
// This involves normalizing the name such that it is relative to the bundle base, removes
// any query parameters or fragments, removes dot and double-dot path segments, normalizes
// percent-encoding, and otherwise validates that the resulting URL is a valid URL.
const jsg::Url processModuleName(kj::StringPtr name, const jsg::Url& base) {
  auto url = normalizeModuleName(name, base);
  KJ_REQUIRE(isValidBundleModuleUrl(url, base), "Invalid module name: ", name);
  return url;
}

}  // namespace

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addSyntheticModule(kj::StringPtr name,
    EvaluateCallback callback,
    kj::Array<kj::String> namedExports,
    Module::ContentType contentType) {
  const auto url = processModuleName(name, bundleBase);
  add(url,
      [url = url.clone(), callback = kj::mv(callback), namedExports = kj::mv(namedExports),
          type = type(), contentType](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newSynthetic(kj::mv(url), type, kj::mv(callback),
        kj::mv(namedExports), Module::Flags::NONE, contentType);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addEsmModule(
    kj::StringPtr name, kj::ArrayPtr<const char> source, Module::Flags flags) {
  const auto url = processModuleName(name, bundleBase);
  add(url,
      [url = url.clone(), source, flags, type = type()](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = kj::heap<EsModule>(kj::mv(url), type, flags, source);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addEsmModule(
    kj::StringPtr name, kj::Array<const char> source, Module::Flags flags) {
  const auto url = processModuleName(name, bundleBase);
  add(url,
      [url = url.clone(), source = kj::mv(source), flags, type = type()](
          const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newEsm(kj::mv(url), type, kj::mv(source), flags);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::addWasmModule(
    kj::StringPtr name, kj::ArrayPtr<const kj::byte> data) {
  const auto url = processModuleName(name, bundleBase);
  auto callback = jsg::modules::Module::newWasmModuleHandler(data);
  add(url,
      [url = url.clone(), callback = kj::mv(callback), type = type()](
          const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newSynthetic(kj::mv(url), type, kj::mv(callback), nullptr,
        EsModule::Flags::WASM, Module::ContentType::WASM);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BundleBuilder& ModuleBundle::BundleBuilder::alias(
    kj::StringPtr alias, kj::StringPtr name) {
  const auto id = processModuleName(name, bundleBase);
  const auto aliasUrl = processModuleName(alias, bundleBase);
  Builder::alias(aliasUrl, id);
  return *this;
}

// ======================================================================================

ModuleBundle::BuiltinBuilder::BuiltinBuilder(Type type)
    : ModuleBundle::Builder(toModuleBuilderType(type)) {}

ModuleBundle::BuiltinBuilder& ModuleBundle::BuiltinBuilder::addSynthetic(
    const Url& id, ModuleBundle::BundleBuilder::EvaluateCallback callback) {
  ensureIsNotBundleSpecifier(id);
  Builder::add(id,
      [url = id.clone(), callback = kj::mv(callback), type = type()](
          const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newSynthetic(kj::mv(url), type, kj::mv(callback));
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

ModuleBundle::BuiltinBuilder& ModuleBundle::BuiltinBuilder::addEsm(
    const Url& id, kj::ArrayPtr<const char> source) {
  ensureIsNotBundleSpecifier(id);
  Builder::add(id,
      [url = id.clone(), source, type = type()](const ResolveContext& context) mutable
      -> kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>> {
    kj::Own<Module> mod = Module::newEsm(kj::mv(url), type, source);
    return kj::Maybe<kj::OneOf<kj::String, kj::Own<Module>>>(kj::mv(mod));
  });
  return *this;
}

// ======================================================================================
ModuleRegistry::Impl::Impl(kj::ArrayPtr<kj::Vector<kj::Own<ModuleBundle>>> vectors) {
  bundles[kBundle] = vectors[kBundle].releaseAsArray();
  bundles[kBuiltin] = vectors[kBuiltin].releaseAsArray();
  bundles[kBuiltinOnly] = vectors[kBuiltinOnly].releaseAsArray();
  bundles[kFallback] = vectors[kFallback].releaseAsArray();
}

ModuleRegistry::Builder::Builder(
    const ResolveObserver& observer, const jsg::Url& bundleBase, Options options)
    : observer(observer),
      bundleBase(bundleBase),
      options(options),
      schemaLoader(kj::heap<capnp::SchemaLoader>()) {}

bool ModuleRegistry::Builder::allowsFallback() const {
  return (options & Options::ALLOW_FALLBACK) == Options::ALLOW_FALLBACK;
}

ModuleRegistry::Builder& ModuleRegistry::Builder::add(kj::Own<ModuleBundle> bundle) {
  if (!allowsFallback()) {
    KJ_REQUIRE(bundle->type() != ModuleBundle::Type::FALLBACK,
        "Fallback bundle types are not allowed for this registry");
  }
  bundles_[static_cast<int>(bundle->type())].add(kj::mv(bundle));
  return *this;
}

ModuleRegistry::Builder& ModuleRegistry::Builder::setEvalCallback(EvalCallback callback) {
  maybeEvalCallback = kj::mv(callback);
  return *this;
}

kj::Arc<ModuleRegistry> ModuleRegistry::Builder::finish() {
  return kj::arc<ModuleRegistry>(this);
}

ModuleRegistry::ModuleRegistry(ModuleRegistry::Builder* builder)
    : observer(builder->observer),
      bundleBase(builder->bundleBase),
      impl(Impl(builder->bundles_.asPtr())),
      maybeEvalCallback(kj::mv(builder->maybeEvalCallback)),
      schemaLoader(kj::mv(builder->schemaLoader)) {}

kj::Maybe<jsg::JsPromise> ModuleRegistry::evaluateImpl(jsg::Lock& js,
    const Module& module,
    v8::Local<v8::Module> v8Module,
    const CompilationObserver& observer) const {
  KJ_IF_SOME(callback, maybeEvalCallback) {
    return callback(js, module, v8Module, observer);
  }
  return kj::none;
}

kj::Own<void> ModuleRegistry::attachToIsolate(Lock& js, const CompilationObserver& observer) const {
  // The IsolateModuleRegistry is attached to the isolate as an embedder data slot.
  // We have to keep it alive for the duration of the v8::Context so we return a
  // kj::Own and store that in the jsg::JsContext
  return kj::heap<IsolateModuleRegistry>(js, *this, observer);
}

kj::Maybe<ModuleRegistry::ModuleOrRedirect> ModuleRegistry::tryFindInBundle(
    const ResolveContext& context, ModuleBundle& bundle, const Url& bundleBase) {
  KJ_IF_SOME(found, bundle.lookup(context)) {
    KJ_IF_SOME(str, found.specifier) {
      // We received a redirect to another module specifier. Let's
      // start resolution over again with the new specifier... but only
      // if we can successfully parse the specifier as a URL.
      KJ_IF_SOME(id, jsg::Url::tryParse(str.asPtr(), bundleBase.getHref())) {
        return kj::Maybe(kj::mv(id));
      }
    }
    KJ_IF_SOME(module, found.module) {
      return kj::Maybe(ModuleRef{
        .module = module,
      });
    }
  }
  return kj::none;
}

kj::Maybe<ModuleRegistry::ModuleOrRedirect> ModuleRegistry::tryFindInBundleGroup(
    const ResolveContext& context, kj::ArrayPtr<kj::Own<ModuleBundle>> bundles) const {
  for (auto& bundle: bundles) {
    KJ_IF_SOME(found, tryFindInBundle(context, *bundle, bundleBase)) {
      return kj::mv(found);
    }
  }
  return kj::none;
}

kj::Maybe<const Module&> ModuleRegistry::lookupImpl(
    Impl& impl, const ResolveContext& context, bool recursed) const {
#define MODULE_LOOKUP(context, bundle)                                                             \
  KJ_IF_SOME(found, tryFindInBundleGroup(context, impl.bundles[bundle])) {                         \
    KJ_SWITCH_ONEOF(found) {                                                                       \
      KJ_CASE_ONEOF(url, Url) {                                                                    \
        if (recursed) { /* avoid recursing indefinitely */                                         \
          return kj::none;                                                                         \
        }                                                                                          \
        kj::HashMap<kj::StringPtr, kj::StringPtr> clonedAttrs;                                     \
        for (const auto& [key, value]: context.attributes) {                                       \
          clonedAttrs.insert(key, value);                                                          \
        }                                                                                          \
        ResolveContext ctx{                                                                        \
          .type = context.type,                                                                    \
          .source = context.source,                                                                \
          .normalizedSpecifier = url,                                                              \
          .referrerNormalizedSpecifier = context.referrerNormalizedSpecifier,                      \
          .rawSpecifier =                                                                          \
              context.rawSpecifier.map([](auto& str) -> kj::StringPtr { return str; }),            \
          .attributes = kj::mv(clonedAttrs),                                                       \
        };                                                                                         \
        return lookupImpl(impl, ctx, true);                                                        \
      }                                                                                            \
      KJ_CASE_ONEOF(mod, ModuleRef) {                                                              \
        return mod.module;                                                                         \
      }                                                                                            \
    }                                                                                              \
    KJ_UNREACHABLE;                                                                                \
  }

  switch (context.type) {
    case ResolveContext::Type::BUNDLE: {
      // For bundle resolution, we only use Bundle, Builtin, and Fallback bundles,
      // in that order.
      MODULE_LOOKUP(context, kBundle);
      MODULE_LOOKUP(context, kBuiltin);
      MODULE_LOOKUP(context, kFallback);
      break;
    }
    case ResolveContext::Type::BUILTIN: {
      // For built-in resolution, we only use builtin and builtin-only bundles.
      MODULE_LOOKUP(context, kBuiltin);
      MODULE_LOOKUP(context, kBuiltinOnly);
      break;
    }
    case ResolveContext::Type::BUILTIN_ONLY: {
      // For built-in only resolution, we only use builtin-only bundles.
      MODULE_LOOKUP(context, kBuiltinOnly);
      break;
    }
    case ResolveContext::Type::PUBLIC_BUILTIN: {
      // For public built-in resolution, we only use builtin bundles.
      // This excludes both worker bundle modules and internal-only modules,
      // returning only built-ins that are normally importable by user code.
      MODULE_LOOKUP(context, kBuiltin);
      break;
    }
  }

#undef MODULE_LOOKUP

  return kj::none;
}

kj::Maybe<const Module&> ModuleRegistry::lookup(const ResolveContext& context) const {
  // If the embedder supports it, collect metrics on what modules were resolved.
  auto metrics =
      observer.onResolveModule(context.normalizedSpecifier, context.type, context.source);

  // While multiple threads may be holding references to the registry, only one thread
  // at a time may resolve a module. Resolving a module may involve mutating internal
  // state (e.g. caching) so we lock here. Fortunately, module resolution should be
  // fast, especially with caching, so this lock should be held only briefly.
  auto lock = impl.lockExclusive();
  KJ_IF_SOME(found, lookupImpl(*lock, context, false)) {
    metrics->found();
    return found;
  }

  metrics->notFound();
  return kj::none;
}

kj::Maybe<JsValue> ModuleRegistry::tryResolveModuleNamespace(Lock& js,
    kj::StringPtr specifier,
    ResolveContext::Type type,
    ResolveContext::Source source,
    kj::Maybe<const Url&> maybeReferrer,
    UnwrapDefault unwrapDefault) {
  auto& bound = IsolateModuleRegistry::from(js.v8Isolate);
  auto url = ([&] {
    KJ_IF_SOME(referrer, maybeReferrer) {
      return KJ_ASSERT_NONNULL(referrer.tryResolve(specifier));
    }
    return KJ_ASSERT_NONNULL(bound.getBundleBase().tryResolve(specifier));
  })();
  auto normalized = url.clone(Url::EquivalenceOption::NORMALIZE_PATH);
  ResolveContext context{
    .type = type,
    .source = source,
    .normalizedSpecifier = normalized,
    .referrerNormalizedSpecifier = maybeReferrer.orDefault(bound.getBundleBase()),
    .rawSpecifier = specifier,
  };
  v8::TryCatch tryCatch(js.v8Isolate);
  auto option = IsolateModuleRegistry::RequireOption::RETURN_EMPTY;
  // Following the behavior of Node.js' require(esm) implementation, we disallow top-level await
  // in synchronously required modules.
  if (source == ResolveContext::Source::REQUIRE) {
    option = option | IsolateModuleRegistry::RequireOption::NO_TOP_LEVEL_AWAIT;
  }
  if (unwrapDefault == UnwrapDefault::YES) {
    option = option | IsolateModuleRegistry::RequireOption::UNWRAP_DEFAULT;
  }

  auto ns = bound.require(js, context, option);
  if (tryCatch.HasCaught()) {
    tryCatch.ReThrow();
    throw JsExceptionThrown();
  }
  if (ns.IsEmpty()) return kj::none;
  return JsValue(check(ns));
}

JsValue ModuleRegistry::resolve(Lock& js,
    kj::StringPtr specifier,
    kj::StringPtr exportName,
    ResolveContext::Type type,
    ResolveContext::Source source,
    kj::Maybe<const Url&> maybeReferrer) {
  KJ_IF_SOME(val, tryResolveModuleNamespace(js, specifier, type, source, maybeReferrer)) {
    auto ns = KJ_ASSERT_NONNULL(val.tryCast<JsObject>());
    return ns.get(js, exportName);
  }
  JSG_FAIL_REQUIRE(Error, kj::str("Module not found: ", specifier));
}

// ======================================================================================

Module::Module(Url id, Type type, Flags flags, ContentType contentType)
    : id_(kj::mv(id)),
      type_(type),
      flags_(flags),
      contentType_(contentType) {}

kj::Maybe<jsg::JsPromise> Module::Evaluator::operator()(jsg::Lock& js,
    const Module& module,
    v8::Local<v8::Module> v8Module,
    const CompilationObserver& observer) const {
  return registry.evaluateImpl(js, module, v8Module, observer);
}

bool Module::instantiate(
    Lock& js, v8::Local<v8::Module> module, const CompilationObserver& observer) const {
  if (module->GetStatus() != v8::Module::kUninstantiated) {
    return true;
  }
  // InstantiateModule is one of those methods that returns a Maybe<bool> but
  // never returns Just(false). It either returns Just(true) or an empty Maybe
  // to signal that the instantiation failed. Eventually I would expect V8 to
  // replace the return value with a Maybe<void>.
  return module
      ->InstantiateModule(js.v8Context(), resolveModuleCallback<false>, resolveModuleCallback<true>)
      .IsJust();
}

bool Module::isEval() const {
  return (flags_ & Flags::EVAL) == Flags::EVAL;
}

bool Module::isEsm() const {
  return (flags_ & Flags::ESM) == Flags::ESM;
}

bool Module::isMain() const {
  return (flags_ & Flags::MAIN) == Flags::MAIN;
}

bool Module::isWasm() const {
  return (flags_ & Flags::WASM) == Flags::WASM;
}

bool Module::evaluateContext(const ResolveContext& context) const {
  if (context.normalizedSpecifier != id()) return false;
  // TODO(soon): Check the import attributes in the context.
  return true;
}

kj::Own<Module> Module::newSynthetic(Url id,
    Type type,
    EvaluateCallback callback,
    kj::Array<kj::String> namedExports,
    Flags flags,
    ContentType contentType) {
  return kj::heap<SyntheticModule>(
      kj::mv(id), type, kj::mv(callback), kj::mv(namedExports), flags, contentType);
}

kj::Own<Module> Module::newEsm(Url id, Type type, kj::Array<const char> code, Flags flags) {
  return kj::heap<EsModule>(kj::mv(id), type, flags, code).attach(kj::mv(code));
}

kj::Own<Module> Module::newEsm(Url id, Type type, kj::ArrayPtr<const char> code) {
  return kj::heap<EsModule>(kj::mv(id), type, Flags::ESM, code);
}

Module::ModuleNamespace::ModuleNamespace(
    v8::Local<v8::Module> inner, kj::ArrayPtr<const kj::String> namedExports)
    : inner(inner),
      namedExports(toHashSet(namedExports)) {}

bool Module::ModuleNamespace::set(Lock& js, kj::StringPtr name, JsValue value) const {
  if (name != "default"_kj) {
    KJ_REQUIRE(namedExports.find(name) != kj::none, kj::str("Module does not export ", name));
  }

  bool result;
  if (!inner->SetSyntheticModuleExport(js.v8Isolate, js.strIntern(name), value).To(&result)) {
    return false;
  }
  if (!result) {
    js.v8Isolate->ThrowError(js.str(kj::str("Failed to set synthetic module export ", name)));
  }
  return result;
}

bool Module::ModuleNamespace::setDefault(Lock& js, JsValue value) const {
  return set(js, SyntheticModule::DEFAULT, value);
}

kj::ArrayPtr<const kj::StringPtr> Module::ModuleNamespace::getNamedExports() const {
  return kj::ArrayPtr<const kj::StringPtr>(namedExports.begin(), namedExports.size());
}

// ======================================================================================
// Methods to create evaluation callbacks for common synthetic module types. It is
// important to remember that evaluation callbacks can be called multiple times and
// from multiple threads. The callbacks must be thread-safe and idempotent.

Module::EvaluateCallback Module::newTextModuleHandler(kj::ArrayPtr<const char> data) {
  return [data](Lock& js, const Url& id, const ModuleNamespace& ns,
             const CompilationObserver&) -> bool {
    JSG_TRY(js) {
      return ns.setDefault(js, js.str(data));
    }
    JSG_CATCH(exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    }
  };
}

Module::EvaluateCallback Module::newDataModuleHandler(kj::ArrayPtr<const kj::byte> data) {
  return [data](Lock& js, const Url& id, const ModuleNamespace& ns,
             const CompilationObserver&) -> bool {
    JSG_TRY(js) {
      auto backing = jsg::BackingStore::alloc<v8::ArrayBuffer>(js, data.size());
      backing.asArrayPtr().copyFrom(data);
      auto buffer = jsg::BufferSource(js, kj::mv(backing));
      return ns.setDefault(js, JsValue(buffer.getHandle(js)));
    }
    JSG_CATCH(exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    }
  };
}

Module::EvaluateCallback Module::newJsonModuleHandler(kj::ArrayPtr<const char> data) {
  return [data](Lock& js, const Url& id, const ModuleNamespace& ns,
             const CompilationObserver& observer) -> bool {
    return js.tryCatch([&] {
      auto metrics = observer.onJsonCompilationStart(js.v8Isolate, data.size());
      return ns.setDefault(js, JsValue(js.parseJson(data).getHandle(js)));
    }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Module::EvaluateCallback Module::newWasmModuleHandler(kj::ArrayPtr<const kj::byte> data) {
  struct Cache final {
    kj::MutexGuarded<kj::Maybe<v8::CompiledWasmModule>> mutex;
  };
  return [data, cache = kj::heap<Cache>()](Lock& js, const Url& id, const ModuleNamespace& ns,
             const CompilationObserver& observer) mutable -> bool {
    return js.tryCatch([&]() -> bool {
      js.setAllowEval(true);
      KJ_DEFER(js.setAllowEval(false));

      // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
      // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
      // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
      // compiles fast but runs slower.
      AllowV8BackgroundThreadsScope scope;

      {
        // See if we can use a cached compiled module to speed things up.
        auto lock = cache->mutex.lockShared();
        KJ_IF_SOME(compiled, *lock) {
          auto metrics = observer.onWasmCompilationFromCacheStart(js.v8Isolate);
          auto result =
              JsValue(check(v8::WasmModuleObject::FromCompiledModule(js.v8Isolate, compiled)));
          return ns.setDefault(js, result);
        }
      }

      auto module = jsg::compileWasmModule(js, data, observer);
      auto lock = cache->mutex.lockExclusive();
      *lock = module->GetCompiledModule();
      auto result = JsValue(module);
      return ns.setDefault(js, result);
    }, [&](Value exception) {
      js.v8Isolate->ThrowException(exception.getHandle(js));
      return false;
    });
  };
}

Function<void()> Module::compileEvalFunction(Lock& js,
    kj::StringPtr code,
    kj::StringPtr name,
    kj::Maybe<JsObject> compileExtensions,
    const CompilationObserver& observer) {
  auto metrics = observer.onScriptCompilationStart(js.v8Isolate, name);
  v8::ScriptOrigin origin(js.str(name));
  v8::ScriptCompiler::Source source(js.str(code), origin);
  auto fn = ([&] {
    KJ_IF_SOME(ext, compileExtensions) {
      v8::Local<v8::Object> obj = ext;
      return check(
          v8::ScriptCompiler::CompileFunction(js.v8Context(), &source, 0, nullptr, 1, &obj));
    } else {
      return check(
          v8::ScriptCompiler::CompileFunction(js.v8Context(), &source, 0, nullptr, 0, nullptr));
    }
  })();

  return [ref = js.v8Ref(fn)](Lock& js) mutable {
    js.withinHandleScope([&] {
      // Any return value is explicitly ignored.
      JsValue(check(ref.getHandle(js)->Call(js.v8Context(), js.v8Context()->Global(), 0, nullptr)));
    });
  };
}

}  // namespace workerd::jsg::modules
