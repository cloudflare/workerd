// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "promise.h"
#include "setup.h"
#include <kj/mutex.h>
#include <set>

namespace workerd::jsg {
namespace {

// The CompileCache is used to hold cached compilation data for built-in JavaScript modules.
//
// Importantly, this is a process-lifetime in-memory cache that is only appropriate for
// built-in modules.
//
// The memory-safety of this cache depends on the assumption that entries are never removed
// or replaced. If things are ever changed such that entries are removed/replaced, then
// we'd likely need to have find return an atomic refcount or something similar.
class CompileCache {
public:
  void add(const void* key, std::unique_ptr<v8::ScriptCompiler::CachedData> cached) const {
    cache.lockExclusive()->upsert(key, kj::mv(cached), [](auto&,auto&&) {});
  }

  kj::Maybe<v8::ScriptCompiler::CachedData&> find(const void* key) const {
    return cache.lockShared()->find(key).map([](auto& data)
        -> v8::ScriptCompiler::CachedData& {
      return *data;
    });
  }

  static const CompileCache& get() {
    static CompileCache instance;
    return instance;
  }

private:
  // The key is the address of the static global that was compiled to produce the CachedData.
  kj::MutexGuarded<kj::HashMap<const void*, std::unique_ptr<v8::ScriptCompiler::CachedData>>> cache;
};

// Implementation of `v8::Module::ResolveCallback`.
v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
                                           v8::Local<v8::String> specifier,
                                           v8::Local<v8::FixedArray> import_assertions,
                                           v8::Local<v8::Module> referrer) {
  auto& js = jsg::Lock::from(context->GetIsolate());
  v8::MaybeLocal<v8::Module> result;

  js.tryCatch([&] {
    auto registry = getModulesForResolveCallback(js.v8Isolate);
    KJ_REQUIRE(registry != nullptr, "didn't expect resolveCallback() now");

    auto ref = KJ_ASSERT_NONNULL(registry->resolve(js, referrer),
        "referrer passed to resolveCallback isn't in modules table");

    auto spec = kj::str(specifier);

    // If the referrer module is a built-in, it is only permitted to resolve
    // internal modules. If the worker bundle provided an override for a builtin,
    // then internalOnly will be false.
    bool internalOnly = ref.type == ModuleRegistry::Type::BUILTIN ||
                        ref.type == ModuleRegistry::Type::INTERNAL;

    kj::Path targetPath = ([&] {
      // If the specifier begins with one of our known prefixes, let's not resolve
      // it against the referrer.
      if (internalOnly ||
          spec.startsWith("node:") ||
          spec.startsWith("cloudflare:") ||
          spec.startsWith("workerd:")) {
        return kj::Path::parse(spec);
      }
      return ref.specifier.parent().eval(spec);
    })();

    KJ_IF_SOME(resolved, registry->resolve(js, targetPath, ref.specifier,
        internalOnly ?
            ModuleRegistry::ResolveOption::INTERNAL_ONLY :
            ModuleRegistry::ResolveOption::DEFAULT,
        ModuleRegistry::ResolveMethod::IMPORT,
        spec.asPtr())) {
      result = resolved.module.getHandle(js);
    } else {
      // This is a bit annoying. If the module was not found, then
      // we need to check to see if it is a prefixed specifier. If it is,
      // we'll try again with only the specifier and not the ref.specifier
      // as parent. We have to do it this way just in case the worker bundle
      // is using the prefix itself. (which isn't likely but is possible).
      // We only need to do this if internalOnly is false.
      if (!internalOnly && (spec.startsWith("node:") || spec.startsWith("cloudflare:"))) {
        KJ_IF_SOME(resolve, registry->resolve(js, kj::Path::parse(spec), ref.specifier,
             ModuleRegistry::ResolveOption::DEFAULT,
             ModuleRegistry::ResolveMethod::IMPORT, spec.asPtr())) {
          result = resolve.module.getHandle(js);
          return;
        }
      }
      JSG_FAIL_REQUIRE(Error, "No such module \"", targetPath.toString(),
        "\".\n  imported from \"", ref.specifier.toString(), "\"");
    }
  }, [&](Value value) {
    // We do not call js.throwException here since that will throw a JsExceptionThrown,
    // which we do not want here. Instead, we'll schedule an exception on the isolate
    // directly and set the result to an empty v8::MaybeLocal.
    js.v8Isolate->ThrowException(value.getHandle(js));
    result = v8::MaybeLocal<v8::Module>();
  });

  return result;
}

// Implementation of `v8::Module::SyntheticModuleEvaluationSteps`, which is called to initialize
// the exports on a synthetic module. Obnoxiously, you can only initialize the exports in this
// callback; V8 will crash if you try to call `SetSyntheticModuleExport()` from anywhere else.
v8::MaybeLocal<v8::Value> evaluateSyntheticModuleCallback(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
  auto& js = Lock::from(context->GetIsolate());
  v8::EscapableHandleScope scope(js.v8Isolate);
  v8::MaybeLocal<v8::Value> result;

  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
    auto registry = getModulesForResolveCallback(js.v8Isolate);
    auto ref = KJ_ASSERT_NONNULL(registry->resolve(js, module),
        "module passed to evaluateSyntheticModuleCallback isn't in modules table");

    // V8 doc comments say this callback must always return an already-resolved promise... I don't
    // know what the point of that is but I guess we'd better do what it says.
    const auto makeResolvedPromise = [&]() {
      v8::Local<v8::Promise::Resolver> resolver;
      if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
        // Return empty local and allow error to propagate.
        return v8::Local<v8::Promise>();
      }
      if (!resolver->Resolve(context, js.v8Undefined()).IsJust()) {
        // Return empty local and allow error to propagate.
        return v8::Local<v8::Promise>();
      }
      return resolver->GetPromise();
    };

    auto& synthetic = KJ_REQUIRE_NONNULL(ref.module.maybeSynthetic, "Not a synthetic module.");
    auto defaultStr = v8StrIntern(js.v8Isolate, "default"_kj);

    KJ_SWITCH_ONEOF(synthetic) {
      KJ_CASE_ONEOF(info, ModuleRegistry::CapnpModuleInfo) {
        bool success = true;
        success = success && module->SetSyntheticModuleExport(
            js.v8Isolate,
            defaultStr,
            info.fileScope.getHandle(js)).IsJust();
        for (auto& decl: info.topLevelDecls) {
          success = success && module->SetSyntheticModuleExport(
              js.v8Isolate,
              v8StrIntern(js.v8Isolate, decl.key),
              decl.value.getHandle(js)).IsJust();
        }

        if (success) {
          result = makeResolvedPromise();
        } else {
          // leave `result` empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::CommonJsModuleInfo) {
        v8::TryCatch catcher(js.v8Isolate);
        // const_cast is safe here because we're protected by the isolate js.
        auto& commonjs = const_cast<ModuleRegistry::CommonJsModuleInfo&>(info);
        try {
          commonjs.evalFunc(js);
        } catch (const JsExceptionThrown&) {
          if (catcher.CanContinue()) catcher.ReThrow();
          // leave `result` empty to propagate the JS exception
          return;
        }

        if (module->SetSyntheticModuleExport(
                js.v8Isolate,
                defaultStr,
                commonjs.moduleContext->module->getExports(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave `result` empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::NodeJsModuleInfo) {
        result = info.evaluate(js, const_cast<ModuleRegistry::NodeJsModuleInfo&>(info), module);
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::TextModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate,
                                             defaultStr,
                                             info.value.getHandle(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::DataModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate,
                                             defaultStr,
                                             info.value.getHandle(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::WasmModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate,
                                             defaultStr,
                                             info.value.getHandle(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::JsonModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate,
                                             defaultStr,
                                             info.value.getHandle(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::ObjectModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate,
                                             defaultStr,
                                             info.value.getHandle(js)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
    }
  })) {
    // V8 doc comments say in the case of an error, throw the error and return an empty Maybe.
    // I.e. NOT a rejected promise. OK...
    context->GetIsolate()->ThrowException(makeInternalError(js.v8Isolate, kj::mv(exception)));
    result = v8::Local<v8::Promise>();
  }

  return scope.EscapeMaybe(result);
}

}  // namespace

ModuleRegistry* getModulesForResolveCallback(v8::Isolate* isolate) {
  return static_cast<ModuleRegistry*>(
      isolate->GetCurrentContext()->GetAlignedPointerFromEmbedderData(2));
}

v8::Local<v8::Value> CommonJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  auto modulesForResolveCallback = getModulesForResolveCallback(js.v8Isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

  kj::Path targetPath = ([&] {
    // If the specifier begins with one of our known prefixes, let's not resolve
    // it against the referrer.
    if (specifier.startsWith("node:") ||
        specifier.startsWith("cloudflare:") ||
        specifier.startsWith("workerd:")) {
      return kj::Path::parse(specifier);
    }
    return path.parent().eval(specifier);
  })();

  // require() is only exposed to worker bundle modules so the resolve here is only
  // permitted to require worker bundle or built-in modules. Internal modules are
  // excluded.
  auto& info = JSG_REQUIRE_NONNULL(
      modulesForResolveCallback->resolve(js, targetPath, path,
                                         ModuleRegistry::ResolveOption::DEFAULT,
                                         ModuleRegistry::ResolveMethod::REQUIRE,
                                         specifier.asPtr()),
      Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  JSG_REQUIRE_NONNULL(info.maybeSynthetic, TypeError,
      "Cannot use require() to import an ES Module.");

  auto module = info.module.getHandle(js);

  check(module->InstantiateModule(js.v8Context(), &resolveCallback));
  auto handle = check(module->Evaluate(js.v8Context()));
  KJ_ASSERT(handle->IsPromise());
  auto prom = handle.As<v8::Promise>();

  // This assert should always pass since evaluateSyntheticModuleCallback() for CommonJS
  // modules (below) always returns an already-resolved promise.
  KJ_ASSERT(prom->State() != v8::Promise::PromiseState::kPending);

  if (module->GetStatus() == v8::Module::kErrored) {
    throwTunneledException(js.v8Isolate, module->GetException());
  }

  // Originally, This returned an object like `{default: module.exports}` when we really
  // intended to return the module exports raw. We should be extracting `default` here.
  // Unfortunately, there is a user depending on the wrong behavior in production, so we
  // needed a compatibility flag to fix.
  if (getCommonJsExportDefault(js.v8Isolate)) {
    return check(module->GetModuleNamespace().As<v8::Object>()
        ->Get(js.v8Context(), v8StrIntern(js.v8Isolate, "default")));
  } else {
    return module->GetModuleNamespace();
  }
}

v8::Local<v8::Value> NonModuleScript::runAndReturn(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.Get(isolate)->BindToCurrentContext();
  return check(boundScript->Run(context));
}

void NonModuleScript::run(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.Get(isolate)->BindToCurrentContext();
  check(boundScript->Run(context));
}

NonModuleScript NonModuleScript::compile(kj::StringPtr code, jsg::Lock& js, kj::StringPtr name) {
  // Create a dummy script origin for it to appear in Sources panel.
  auto isolate = js.v8Isolate;
  v8::ScriptOrigin origin(v8StrIntern(isolate, name));
  v8::ScriptCompiler::Source source(v8Str(isolate, code), origin);
  return NonModuleScript(js,
      check(v8::ScriptCompiler::CompileUnboundScript(isolate, &source)));
}

void instantiateModule(jsg::Lock& js, v8::Local<v8::Module>& module) {
  KJ_ASSERT(!module.IsEmpty());
  auto isolate = js.v8Isolate;
  auto context = js.v8Context();

  auto status = module->GetStatus();
  // Nothing to do if the module is already instantiated, evaluated, or errored.
  if (status == v8::Module::Status::kInstantiated ||
      status == v8::Module::Status::kEvaluated ||
      status == v8::Module::Status::kErrored) return;

  JSG_REQUIRE(status == v8::Module::Status::kUninstantiated, Error,
      "Module cannot be synchronously required while it is being instantiated or evaluated. "
      "This error typically means that a CommonJS or NodeJS-Compat type module has a circular "
      "dependency on itself, and that a synchronous require() is being called while the module "
      "is being loaded.");

  jsg::check(module->InstantiateModule(context, &resolveCallback));
  auto prom = jsg::check(module->Evaluate(context)).As<v8::Promise>();
  js.runMicrotasks();

  switch (prom->State()) {
    case v8::Promise::kPending:
      // Let's make sure nobody is depending on pending modules that do not resolve first.
      KJ_LOG(ERROR, "Async module was not immediately resolved.");
    break;
    case v8::Promise::kRejected:
      // Since we don't actually support I/O when instantiating a worker, we don't return the
      // promise from module->Evaluate, which means we lose any errors that happen during
      // instantiation if we don't throw the rejection exception here.
      isolate->ThrowException(module->GetException());
      throw jsg::JsExceptionThrown();
    case v8::Promise::kFulfilled:
    break;
  }
}

// ===================================================================================

namespace {

static CompilationObserver::Option convertOption(ModuleInfoCompileOption option) {
  switch (option) {
    case ModuleInfoCompileOption::BUILTIN: return CompilationObserver::Option::BUILTIN;
    case ModuleInfoCompileOption::BUNDLE: return CompilationObserver::Option::BUNDLE;
  }
  KJ_UNREACHABLE;
}

v8::Local<v8::Module> compileEsmModule(
    jsg::Lock& js,
    kj::StringPtr name,
    kj::ArrayPtr<const char> content,
    ModuleInfoCompileOption option,
    const CompilationObserver& observer) {
  // destroy the observer after compilation finished to indicate the end of the process.
  auto compilationObserver = observer.onEsmCompilationStart(js.v8Isolate, name, convertOption(option));

  // Must pass true for `is_module`, but we can skip everything else.
  const int resourceLineOffset = 0;
  const int resourceColumnOffset = 0;
  const bool resourceIsSharedCrossOrigin = false;
  const int scriptId = -1;
  const bool resourceIsOpaque = false;
  const bool isWasm = false;
  const bool isModule = true;
  v8::ScriptOrigin origin(v8StrIntern(js.v8Isolate, name),
                          resourceLineOffset,
                          resourceColumnOffset,
                          resourceIsSharedCrossOrigin, scriptId, {},
                          resourceIsOpaque, isWasm, isModule);
  v8::Local<v8::String> contentStr;

  if (option == ModuleInfoCompileOption::BUILTIN) {
    // TODO(later): Use of newExternalOneByteString here limits our built-in source
    // modules (for which this path is used) to only the latin1 character set. We
    // may need to revisit that to import built-ins as UTF-16 (two-byte).
    contentStr = jsg::newExternalOneByteString(js, content);

    // TODO(bug): The cache is failing under certain conditions. Disabling this logic
    // for now until it can be debugged.
    // const auto& compileCache = CompileCache::get();
    // KJ_IF_SOME(cached, compileCache.find(content.begin())) {
    //   v8::ScriptCompiler::Source source(contentStr, origin, &cached);
    //   v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::kConsumeCodeCache;
    //   KJ_DEFER(if (source.GetCachedData()->rejected) {
    //     KJ_LOG(ERROR, kj::str("Failed to load module '", name ,"' using compile cache"));
    //     js.throwException(KJ_EXCEPTION(FAILED, "jsg.Error: Internal error"));
    //   });
    //   return jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, options));
    // }

    v8::ScriptCompiler::Source source(contentStr, origin);
    auto module = jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));

    auto cachedData = std::unique_ptr<v8::ScriptCompiler::CachedData>(
        v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript()));
    // compileCache.add(content.begin(), kj::mv(cachedData));
    return module;
  }

  contentStr = jsg::v8Str(js.v8Isolate, content);

  v8::ScriptCompiler::Source source(contentStr, origin);
  auto module = jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));

  return module;
}

v8::Local<v8::Module> createSyntheticModule(
    jsg::Lock& js,
    kj::StringPtr name,
    kj::Maybe<kj::ArrayPtr<kj::StringPtr>> maybeExports) {
  std::vector<v8::Local<v8::String>> exportNames;
  exportNames.push_back(v8StrIntern(js.v8Isolate, "default"_kj));
  KJ_IF_SOME(exports, maybeExports) {
    for (auto& name : exports) {
      exportNames.push_back(v8StrIntern(js.v8Isolate, name));
    }
  }
  return v8::Module::CreateSyntheticModule(
      js.v8Isolate,
      v8StrIntern(js.v8Isolate, name),
      v8::MemorySpan<const v8::Local<v8::String>>(exportNames.data(), exportNames.size()),
      &evaluateSyntheticModuleCallback);
}
}  // namespace

ModuleRegistry::ModuleInfo::ModuleInfo(
    jsg::Lock& js,
    v8::Local<v8::Module> module,
    kj::Maybe<SyntheticModuleInfo> maybeSynthetic)
    : module(js.v8Isolate, module),
      maybeSynthetic(kj::mv(maybeSynthetic)) {}

ModuleRegistry::ModuleInfo::ModuleInfo(
    jsg::Lock& js,
    kj::StringPtr name,
    kj::ArrayPtr<const char> content,
    ModuleInfoCompileOption flags,
    const CompilationObserver& observer)
    : ModuleInfo(js, compileEsmModule(js, name, content, flags, observer)) {}

ModuleRegistry::ModuleInfo::ModuleInfo(
    jsg::Lock& js,
    kj::StringPtr name,
    kj::Maybe<kj::ArrayPtr<kj::StringPtr>> maybeExports,
    SyntheticModuleInfo synthetic)
    : ModuleInfo(js, createSyntheticModule(js, name, maybeExports), kj::mv(synthetic)) {}

Ref<CommonJsModuleContext>
ModuleRegistry::CommonJsModuleInfo::initModuleContext(
    jsg::Lock& js,
    kj::StringPtr name) {
  return jsg::alloc<jsg::CommonJsModuleContext>(js, kj::Path::parse(name));
}

ModuleRegistry::CapnpModuleInfo::CapnpModuleInfo(
    Value fileScope,
    kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls)
    : fileScope(kj::mv(fileScope)),
      topLevelDecls(kj::mv(topLevelDecls)) {}


v8::Local<v8::WasmModuleObject> compileWasmModule(jsg::Lock& js,
    kj::ArrayPtr<const uint8_t> code,
    const CompilationObserver& observer) {
  // destroy the observer after compilation finishes to indicate the end of the process.
  auto compilationObserver = observer.onWasmCompilationStart(js.v8Isolate, code.size());

  return jsg::check(v8::WasmModuleObject::Compile(
      js.v8Isolate,
      v8::MemorySpan<const uint8_t>(code.begin(), code.size())));
}

// ======================================================================================

jsg::Ref<jsg::Object> ModuleRegistry::NodeJsModuleInfo::initModuleContext(
    jsg::Lock& js,
    kj::StringPtr name) {
  return jsg::alloc<NodeJsModuleContext>(js, kj::Path::parse(name));
}

v8::MaybeLocal<v8::Value> ModuleRegistry::NodeJsModuleInfo::evaluate(
      jsg::Lock& js,
      ModuleRegistry::NodeJsModuleInfo& info,
      v8::Local<v8::Module> module) {
  const auto makeResolvedPromise = [&]() {
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver)) {
      // Return empty local and allow error to propagate.
      return v8::Local<v8::Promise>();
    }
    if (!resolver->Resolve(js.v8Context(), js.v8Undefined()).IsJust()) {
      // Return empty local and allow error to propagate.
      return v8::Local<v8::Promise>();
    }
    return resolver->GetPromise();
  };

  v8::MaybeLocal<v8::Value> result;
  v8::TryCatch catcher(js.v8Isolate);
  try {
    info.evalFunc(js);
  } catch (const JsExceptionThrown&) {
    if (catcher.CanContinue()) catcher.ReThrow();
    // leave `result` empty to propagate the JS exception
    return v8::MaybeLocal<v8::Value>();
  }

  auto ctx = static_cast<NodeJsModuleContext*>(info.moduleContext.get());

  if (module->SetSyntheticModuleExport(
          js.v8Isolate,
          v8StrIntern(js.v8Isolate, "default"_kj),
          ctx->module->getExports(js)).IsJust()) {
    result = makeResolvedPromise();
  } else {
    // leave `result` empty to propagate the JS exception
  }
  return result;
}

NodeJsModuleContext::NodeJsModuleContext(jsg::Lock& js, kj::Path path)
    : module(jsg::alloc<NodeJsModuleObject>(js, path.toString(true))),
      path(kj::mv(path)),
      exports(js.v8Ref(module->getExports(js))) {}

v8::Local<v8::Value> NodeJsModuleContext::require(jsg::Lock& js, kj::String specifier) {
  // This list must be kept in sync with the list of builtins from Node.js.
  // It should be unlikely that anything is ever removed from this list, and
  // adding items to it is considered a semver-major change in Node.js.
  static const std::set<kj::StringPtr> NODEJS_BUILTINS {
    "_http_agent",         "_http_client",        "_http_common",
    "_http_incoming",      "_http_outgoing",      "_http_server",
    "_stream_duplex",      "_stream_passthrough", "_stream_readable",
    "_stream_transform",   "_stream_wrap",        "_stream_writable",
    "_tls_common",         "_tls_wrap",           "assert",
    "assert/strict",       "async_hooks",         "buffer",
    "child_process",       "cluster",             "console",
    "constants",           "crypto",              "dgram",
    "diagnostics_channel", "dns",                 "dns/promises",
    "domain",              "events",              "fs",
    "fs/promises",         "http",                "http2",
    "https",               "inspector",           "inspector/promises",
    "module",              "net",                 "os",
    "path",                "path/posix",          "path/win32",
    "perf_hooks",          "process",             "punycode",
    "querystring",         "readline",            "readline/promises",
    "repl",                "stream",              "stream/consumers",
    "stream/promises",     "stream/web",          "string_decoder",
    "sys",                 "timers",              "timers/promises",
    "tls",                 "trace_events",        "tty",
    "url",                 "util",                "util/types",
    "v8",                  "vm",                  "worker_threads",
    "zlib"
  };

  // If it is a bare specifier known to be a Node.js built-in, then prefix the
  // specifier with node:
  bool isNodeBuiltin = false;
  auto resolveOption = jsg::ModuleRegistry::ResolveOption::DEFAULT;
  if (NODEJS_BUILTINS.contains(specifier)) {
    specifier = kj::str("node:", specifier);
    isNodeBuiltin = true;
    resolveOption = jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY;
  } else if (specifier.startsWith("node:")) {
    isNodeBuiltin = true;
    resolveOption = jsg::ModuleRegistry::ResolveOption::BUILTIN_ONLY;
  }

  // TODO(cleanup): This implementation from here on is identical to the
  // CommonJsModuleContext::require. We should consolidate these as the
  // next step.

  auto modulesForResolveCallback = jsg::getModulesForResolveCallback(js.v8Isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

  kj::Path targetPath = ([&] {
    // If the specifier begins with one of our known prefixes, let's not resolve
    // it against the referrer.
    if (specifier.startsWith("node:") ||
        specifier.startsWith("cloudflare:") ||
        specifier.startsWith("workerd:")) {
      return kj::Path::parse(specifier);
    }
    return path.parent().eval(specifier);
  })();

  // require() is only exposed to worker bundle modules so the resolve here is only
  // permitted to require worker bundle or built-in modules. Internal modules are
  // excluded.
  auto& info = JSG_REQUIRE_NONNULL(
      modulesForResolveCallback->resolve(js, targetPath, path, resolveOption,
                                         ModuleRegistry::ResolveMethod::REQUIRE,
                                         specifier.asPtr()),
      Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  if (!isNodeBuiltin) {
    JSG_REQUIRE_NONNULL(info.maybeSynthetic, TypeError,
        "Cannot use require() to import an ES Module.");
  }

  auto module = info.module.getHandle(js);

  jsg::instantiateModule(js, module);

  auto handle = jsg::check(module->Evaluate(js.v8Context()));
  KJ_ASSERT(handle->IsPromise());
  auto prom = handle.As<v8::Promise>();

  // This assert should always pass since evaluateSyntheticModuleCallback() for CommonJS
  // modules (below) always returns an already-resolved promise.
  KJ_ASSERT(prom->State() != v8::Promise::PromiseState::kPending);

  if (module->GetStatus() == v8::Module::kErrored) {
    jsg::throwTunneledException(js.v8Isolate, module->GetException());
  }

  return js.v8Get(module->GetModuleNamespace().As<v8::Object>(), "default"_kj);
}

v8::Local<v8::Value> NodeJsModuleContext::getBuffer(jsg::Lock& js) {
  auto value = require(js, kj::str("node:buffer"));
  JSG_REQUIRE(value->IsObject(), TypeError, "Invalid node:buffer implementation");
  auto module = value.As<v8::Object>();
  auto buffer = js.v8Get(module, "Buffer"_kj);
  JSG_REQUIRE(buffer->IsFunction(), TypeError, "Invalid node:buffer implementation");
  return buffer;
}

v8::Local<v8::Value> NodeJsModuleContext::getProcess(jsg::Lock& js) {
  auto value = require(js, kj::str("node:process"));
  JSG_REQUIRE(value->IsObject(), TypeError, "Invalid node:process implementation");
  return value;
}

kj::String NodeJsModuleContext::getFilename() {
  return path.toString(true);
}

kj::String NodeJsModuleContext::getDirname() {
  return path.parent().toString(true);
}

jsg::Ref<NodeJsModuleObject> NodeJsModuleContext::getModule(jsg::Lock& js) {
  return module.addRef();
}

v8::Local<v8::Value> NodeJsModuleContext::getExports(jsg::Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleContext::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

NodeJsModuleObject::NodeJsModuleObject(jsg::Lock& js, kj::String path)
    : exports(js.v8Isolate, v8::Object::New(js.v8Isolate)),
      path(kj::mv(path)) {}

v8::Local<v8::Value> NodeJsModuleObject::getExports(jsg::Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleObject::setExports(jsg::Value value) {
  exports = kj::mv(value);
}

kj::StringPtr NodeJsModuleObject::getPath() { return path; }

kj::Maybe<kj::OneOf<kj::String, ModuleRegistry::ModuleInfo>> tryResolveFromFallbackService(
    Lock& js, const kj::Path& specifier,
    kj::Maybe<const kj::Path&>& referrer,
    CompilationObserver& observer,
    ModuleRegistry::ResolveMethod method,
    kj::Maybe<kj::StringPtr> rawSpecifier) {
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  KJ_IF_SOME(fallback, isolateBase.tryGetModuleFallback()) {
    kj::Maybe<kj::String> maybeRef;
    KJ_IF_SOME(ref, referrer) {
      maybeRef = ref.toString(true);
    }
    return fallback(js, specifier.toString(true), kj::mv(maybeRef), observer, method, rawSpecifier);
  }
  return kj::none;
}

}  // namespace workerd::jsg
