// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "setup.h"

#include <v8-wasm.h>

#include <kj/mutex.h>

#include <set>

namespace workerd::jsg {
namespace {

// Implementation of `v8::Module::ResolveCallback`.
v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions,
    v8::Local<v8::Module> referrer) {
  auto& js = jsg::Lock::from(context->GetIsolate());
  v8::MaybeLocal<v8::Module> result;

  // TODO(new-module-registry): The specification for import assertions strongly
  // recommends that embedders reject import attributes and types they do not
  // understand/implement. This is because import attributes can alter the
  // interpretation of a module and are considered to be part of the unique
  // key for caching a module.
  // Throwing an error for things we do not understand is the safest thing to do.
  // However, historically we have not followed this guideline in the spec and
  // it's not clear if enforcing that constraint would be breaking so let's
  // first try to determine if anyone is making use of import assertions.
  // If we're lucky, we won't receive any hits on this and we can start
  // enforcing the rule without a compat flag.
  if (import_assertions->Length() > 0) {
    LOG_NOSENTRY(WARNING, "Import attributes specified (and ignored) on static import");
  }

  js.tryCatch([&] {
    auto registry = getModulesForResolveCallback(js.v8Isolate);
    KJ_REQUIRE(registry != nullptr, "didn't expect resolveCallback() now");

    auto ref = KJ_ASSERT_NONNULL(registry->resolve(js, referrer),
        "referrer passed to resolveCallback isn't in modules table");

    auto spec = kj::str(specifier);

    if (isNodeJsCompatEnabled(js)) {
      KJ_IF_SOME(nodeSpec, checkNodeSpecifier(spec)) {
        spec = kj::mv(nodeSpec);
      }
    }

    // If the referrer module is a built-in, it is only permitted to resolve
    // internal modules. If the worker bundle provided an override for a builtin,
    // then internalOnly will be false.
    bool internalOnly =
        ref.type == ModuleRegistry::Type::BUILTIN || ref.type == ModuleRegistry::Type::INTERNAL;

    kj::Path targetPath = ([&] {
      // If the specifier begins with one of our known prefixes, let's not resolve
      // it against the referrer.
      if (internalOnly || spec.startsWith("node:") || spec.startsWith("cloudflare:") ||
          spec.startsWith("workerd:")) {
        return kj::Path::parse(spec);
      }
      return ref.specifier.parent().eval(spec);
    })();

    KJ_IF_SOME(resolved,
        registry->resolve(js, targetPath, ref.specifier,
            internalOnly ? ModuleRegistry::ResolveOption::INTERNAL_ONLY
                         : ModuleRegistry::ResolveOption::DEFAULT,
            ModuleRegistry::ResolveMethod::IMPORT, spec.asPtr())) {
      result = resolved.module.getHandle(js);
    } else {
      // This is a bit annoying. If the module was not found, then
      // we need to check to see if it is a prefixed specifier. If it is,
      // we'll try again with only the specifier and not the ref.specifier
      // as parent. We have to do it this way just in case the worker bundle
      // is using the prefix itself. (which isn't likely but is possible).
      // We only need to do this if internalOnly is false.
      if (!internalOnly && (spec.startsWith("node:") || spec.startsWith("cloudflare:"))) {
        KJ_IF_SOME(resolve,
            registry->resolve(js, kj::Path::parse(spec), ref.specifier,
                ModuleRegistry::ResolveOption::DEFAULT, ModuleRegistry::ResolveMethod::IMPORT,
                spec.asPtr())) {
          result = resolve.module.getHandle(js);
          return;
        }
      }
      JSG_FAIL_REQUIRE(Error, "No such module \"", targetPath.toString(), "\".\n  imported from \"",
          ref.specifier.toString(), "\"");
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
    auto defaultStr = js.strIntern("default"_kj);

    KJ_SWITCH_ONEOF(synthetic) {
      KJ_CASE_ONEOF(info, ModuleRegistry::CapnpModuleInfo) {
        bool success = true;
        success = success &&
            module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.fileScope.getHandle(js))
                .IsJust();
        for (auto& decl: info.topLevelDecls) {
          success = success &&
              module
                  ->SetSyntheticModuleExport(
                      js.v8Isolate, v8StrIntern(js.v8Isolate, decl.key), decl.value.getHandle(js))
                  .IsJust();
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
          auto exports = commonjs.getExports(js);
          if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, exports).IsJust()) {
            KJ_IF_SOME(obj, exports.tryCast<JsObject>()) {
              KJ_IF_SOME(exports, ref.module.maybeNamedExports) {
                for (auto& name: exports) {
                  // Ignore default... just in case someone was silly enough to include it.
                  if (name == "default"_kj) continue;
                  auto val = obj.get(js, name);
                  if (!module->SetSyntheticModuleExport(js.v8Isolate, js.strIntern(name), val)
                           .IsJust()) {
                    break;
                  }
                }
              }
            }
            result = makeResolvedPromise();
          }
        } catch (const JsExceptionThrown&) {
          if (catcher.CanContinue()) catcher.ReThrow();
          // leave `result` empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::TextModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.value.getHandle(js))
                .IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::DataModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.value.getHandle(js))
                .IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::WasmModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.value.getHandle(js))
                .IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::JsonModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.value.getHandle(js))
                .IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::ObjectModuleInfo) {
        if (module->SetSyntheticModuleExport(js.v8Isolate, defaultStr, info.value.getHandle(js))
                .IsJust()) {
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

void instantiateModule(
    jsg::Lock& js, v8::Local<v8::Module>& module, InstantiateModuleOptions options) {
  KJ_ASSERT(!module.IsEmpty());
  auto isolate = js.v8Isolate;
  auto context = js.v8Context();

  auto status = module->GetStatus();

  // If the previous instantiation failed, throw the exception.
  if (status == v8::Module::Status::kErrored) {
    isolate->ThrowException(module->GetException());
    throw jsg::JsExceptionThrown();
  }

  // Nothing to do if the module is already evaluated.
  if (status == v8::Module::Status::kEvaluated || status == v8::Module::Status::kEvaluating) return;

  if (status == v8::Module::Status::kUninstantiated) {
    jsg::check(module->InstantiateModule(context, &resolveCallback));
  }

  auto prom = jsg::check(module->Evaluate(context)).As<v8::Promise>();

  if (module->IsGraphAsync() && prom->State() == v8::Promise::kPending) {
    // If top level await has been disable, error.
    JSG_REQUIRE(options != InstantiateModuleOptions::NO_TOP_LEVEL_AWAIT, Error,
        "Top-level await in module is not permitted at this time.");
  }
  // We run microtasks to ensure that any promises that happen to be scheduled
  // during the evaluation of the top level scope have a chance to be settled,
  // even if those are not directly awaited.
  js.runMicrotasks();

  switch (prom->State()) {
    case v8::Promise::kPending:
      // Let's make sure nobody is depending on modules awaiting on pending promises.
      JSG_FAIL_REQUIRE(Error, "Top-level await in module is unsettled.");
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
    case ModuleInfoCompileOption::BUILTIN:
      return CompilationObserver::Option::BUILTIN;
    case ModuleInfoCompileOption::BUNDLE:
      return CompilationObserver::Option::BUNDLE;
  }
  KJ_UNREACHABLE;
}

v8::Local<v8::Module> compileEsmModule(jsg::Lock& js,
    kj::StringPtr name,
    kj::ArrayPtr<const char> content,
    kj::ArrayPtr<const kj::byte> compileCache,
    ModuleInfoCompileOption option,
    const CompilationObserver& observer) {
  // destroy the observer after compilation finished to indicate the end of the process.
  auto compilationObserver =
      observer.onEsmCompilationStart(js.v8Isolate, name, convertOption(option));

  // Must pass true for `is_module`, but we can skip everything else.
  constexpr int resourceLineOffset = 0;
  constexpr int resourceColumnOffset = 0;
  constexpr bool resourceIsSharedCrossOrigin = false;
  constexpr int scriptId = -1;
  constexpr bool resourceIsOpaque = false;
  constexpr bool isWasm = false;
  constexpr bool isModule = true;
  v8::ScriptOrigin origin(v8StrIntern(js.v8Isolate, name), resourceLineOffset, resourceColumnOffset,
      resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque, isWasm, isModule);
  v8::Local<v8::String> contentStr;

  if (option == ModuleInfoCompileOption::BUILTIN) {
    // TODO(later): Use of newExternalOneByteString here limits our built-in source
    // modules (for which this path is used) to only the latin1 character set. We
    // may need to revisit that to import built-ins as UTF-16 (two-byte).
    contentStr = jsg::newExternalOneByteString(js, content);
  } else {
    contentStr = jsg::v8Str(js.v8Isolate, content);
  }

  if (compileCache.size() > 0 && compileCache.begin() != nullptr) {
    auto cached =
        std::make_unique<v8::ScriptCompiler::CachedData>(compileCache.begin(), compileCache.size());
    v8::ScriptCompiler::Source source(contentStr, origin, cached.release());
    return jsg::check(v8::ScriptCompiler::CompileModule(
        js.v8Isolate, &source, v8::ScriptCompiler::kConsumeCodeCache));
  }

  v8::ScriptCompiler::Source source(contentStr, origin);
  return jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
}

v8::Local<v8::Module> createSyntheticModule(
    jsg::Lock& js, kj::StringPtr name, kj::Maybe<kj::ArrayPtr<const kj::StringPtr>> maybeExports) {
  v8::LocalVector<v8::String> exportNames(js.v8Isolate);
  exportNames.push_back(v8StrIntern(js.v8Isolate, "default"_kj));
  KJ_IF_SOME(exports, maybeExports) {
    exportNames.reserve(exports.size());
    for (auto& name: exports) {
      exportNames.push_back(v8StrIntern(js.v8Isolate, name));
    }
  }
  return v8::Module::CreateSyntheticModule(js.v8Isolate, v8StrIntern(js.v8Isolate, name),
      v8::MemorySpan<const v8::Local<v8::String>>(exportNames.data(), exportNames.size()),
      &evaluateSyntheticModuleCallback);
}
}  // namespace

ModuleRegistry::ModuleInfo::ModuleInfo(
    jsg::Lock& js, v8::Local<v8::Module> module, kj::Maybe<SyntheticModuleInfo> maybeSynthetic)
    : module(js.v8Isolate, module),
      maybeSynthetic(kj::mv(maybeSynthetic)) {}

ModuleRegistry::ModuleInfo::ModuleInfo(jsg::Lock& js,
    kj::StringPtr name,
    kj::ArrayPtr<const char> content,
    kj::ArrayPtr<const kj::byte> compileCache,
    ModuleInfoCompileOption flags,
    const CompilationObserver& observer)
    : ModuleInfo(js, compileEsmModule(js, name, content, compileCache, flags, observer)) {}

ModuleRegistry::ModuleInfo::ModuleInfo(jsg::Lock& js,
    kj::StringPtr name,
    kj::Maybe<kj::ArrayPtr<const kj::StringPtr>> maybeExports,
    SyntheticModuleInfo synthetic)
    : ModuleInfo(js, createSyntheticModule(js, name, maybeExports), kj::mv(synthetic)) {
  KJ_IF_SOME(exports, maybeExports) {
    maybeNamedExports = KJ_MAP(name, exports) { return kj::str(name); };
  }
}

jsg::JsValue ModuleRegistry::CommonJsModuleInfo::getExports(jsg::Lock& js) {
  return provider->getExports(js);
}

ModuleRegistry::CapnpModuleInfo::CapnpModuleInfo(
    Value fileScope, kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls)
    : fileScope(kj::mv(fileScope)),
      topLevelDecls(kj::mv(topLevelDecls)) {}

v8::Local<v8::WasmModuleObject> compileWasmModule(
    jsg::Lock& js, kj::ArrayPtr<const uint8_t> code, const CompilationObserver& observer) {
  // destroy the observer after compilation finishes to indicate the end of the process.
  auto compilationObserver = observer.onWasmCompilationStart(js.v8Isolate, code.size());

  return jsg::check(v8::WasmModuleObject::Compile(
      js.v8Isolate, v8::MemorySpan<const uint8_t>(code.begin(), code.size())));
}

// ======================================================================================

kj::Maybe<kj::OneOf<kj::String, ModuleRegistry::ModuleInfo>> tryResolveFromFallbackService(Lock& js,
    const kj::Path& specifier,
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

JsValue ModuleRegistry::requireImpl(Lock& js, ModuleInfo& info, RequireImplOptions options) {
  auto module = info.module.getHandle(js);

  // If the module status is evaluating or instantiating then the module is likely
  // has a circular dependency on itself. If the module is a CommonJS or NodeJS
  // module, we can return the exports object directly here.
  if (module->GetStatus() == v8::Module::Status::kEvaluating ||
      module->GetStatus() == v8::Module::Status::kInstantiating) {
    KJ_IF_SOME(synth, info.maybeSynthetic) {
      KJ_IF_SOME(cjs, synth.tryGet<ModuleRegistry::CommonJsModuleInfo>()) {
        return cjs.getExports(js);
      }
    }
  }

  // When using require(...) we previously allowed the required modules to use
  // top-level await. With a compat flag we disable use of top-level await but
  // ONLY when the module is synchronously required. The same module being imported
  // either statically or dynamically can still use TLA. This aligns with behavior
  // being implemented in other JS runtimes.
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  jsg::InstantiateModuleOptions opts = jsg::InstantiateModuleOptions::DEFAULT;
  if (!isolateBase.isTopLevelAwaitEnabled()) {
    opts = jsg::InstantiateModuleOptions::NO_TOP_LEVEL_AWAIT;

    // If the module was already evaluated, let's check if it is async.
    // If it is, we will throw an error. This case can happen if a previous
    // attempt to require the module failed because the module was async.
    if (module->GetStatus() == v8::Module::kEvaluated) {
      JSG_REQUIRE(!module->IsGraphAsync(), Error,
          "Top-level await in module is not permitted at this time.");
    }
  }

  jsg::instantiateModule(js, module, opts);

  if (info.maybeSynthetic == kj::none) {
    // If the module is an ESM and the __cjsUnwrapDefault flag is set to true, we will
    // always return the default export regardless of the options.
    // Otherwise fallback to the options. This is an early version of the "module.exports"
    // convention that Node.js finally adopted for require(esm) that was not officially
    // adopted but there are a handful of modules in the ecosystem that supported it
    // early. It's trivial for us to support here so let's just do so.
    JsObject obj(module->GetModuleNamespace().As<v8::Object>());
    if (obj.get(js, "__cjsUnwrapDefault"_kj) == js.boolean(true)) {
      return obj.get(js, "default"_kj);
    }
    // If the ES Module namespace exports a "module.exports" key then that will be the
    // export that is returned by the require(...) call per Node.js' recently added
    // require(esm) support.
    // See: https://nodejs.org/docs/latest/api/modules.html#loading-ecmascript-modules-using-require
    if (obj.has(js, "module.exports"_kj)) {
      // We only want to return the value if it is explicitly specified, otherwise we'll
      // always be returning undefined.
      return obj.get(js, "module.exports"_kj);
    }
  }

  // Originally, require returned an object like `{default: module.exports}` when we really
  // intended to return the module exports raw. We should be extracting `default` here.
  // When Node.js recently finally adopted require(esm), they adopted the default behavior
  // of exporting the module namespace, which is fun. We'll stick with our default here for
  // now but users can get Node.js-like behavior by switching off the
  // exportCommonJsDefaultNamespace compat flag.
  if (options == RequireImplOptions::EXPORT_DEFAULT) {
    return JsValue(check(module->GetModuleNamespace().As<v8::Object>()->Get(
        js.v8Context(), v8StrIntern(js.v8Isolate, "default"))));
  }

  return JsValue(module->GetModuleNamespace());
}

}  // namespace workerd::jsg
