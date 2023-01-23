// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "modules.h"
#include "promise.h"
#include <kj/mutex.h>

namespace workerd::jsg {
namespace {

class CompileCache {
  // The CompileCache is used to hold cached compilation data for built-in JavaScript modules.
  //
  // Importantly, this is a process-lifetime in-memory cache that is only appropriate for
  // built-in modules.
  //
  // The memory-safety of this cache depends on the assumption that entries are never removed
  // or replaced. If things are ever changed such that entries are removed/replaced, then
  // we'd likely need to have find return an atomic refcount or something similar.
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
  kj::MutexGuarded<kj::HashMap<const void*, std::unique_ptr<v8::ScriptCompiler::CachedData>>> cache;
  // The key is the address of the static global that was compiled to produce the CachedData.
};

ModuleRegistry* getModulesForResolveCallback(v8::Isolate* isolate) {
  return static_cast<ModuleRegistry*>(
      isolate->GetCurrentContext()->GetAlignedPointerFromEmbedderData(2));
}

v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
                                           v8::Local<v8::String> specifier,
                                           v8::Local<v8::FixedArray> import_assertions,
                                           v8::Local<v8::Module> referrer) {
  // Implementation of `v8::Module::ResolveCallback`.

  v8::Isolate* isolate = context->GetIsolate();
  auto& js = jsg::Lock::from(isolate);
  v8::MaybeLocal<v8::Module> result;

  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    auto registry = getModulesForResolveCallback(isolate);
    KJ_REQUIRE(registry != nullptr, "didn't expect resolveCallback() now");

    auto ref = KJ_ASSERT_NONNULL(registry->resolve(js, referrer),
        "referrer passed to resolveCallback isn't in modules table");

    kj::Path targetPath = ref.specifier.parent().eval(kj::str(specifier));

    // If the referrer module is a built-in, it is only permitted to resolve
    // internal modules. If the worker bundle provided an override for a builtin,
    // then internalOnly will be false.
    bool internalOnly = ref.type == ModuleRegistry::Type::BUILTIN ||
                        ref.type == ModuleRegistry::Type::INTERNAL;

    result = JSG_REQUIRE_NONNULL(registry->resolve(js, targetPath, internalOnly), Error,
        "No such module \"", targetPath.toString(),
        "\".\n  imported from \"", ref.specifier.toString(), "\"")
        .module.getHandle(js);

  })) {
    isolate->ThrowException(makeInternalError(isolate, kj::mv(*exception)));
    result = v8::MaybeLocal<v8::Module>();
  }

  return result;
}

v8::MaybeLocal<v8::Value> evaluateSyntheticModuleCallback(
    v8::Local<v8::Context> context, v8::Local<v8::Module> module) {
  // Implementation of `v8::Module::SyntheticModuleEvaluationSteps`, which is called to initialize
  // the exports on a synthetic module. Obnoxiously, you can only initialize the exports in this
  // callback; V8 will crash if you try to call `SetSyntheticModuleExport()` from anywhere else.
  v8::Isolate* isolate = context->GetIsolate();
  auto& js = Lock::from(isolate);
  v8::EscapableHandleScope scope(isolate);
  v8::MaybeLocal<v8::Value> result;

  KJ_IF_MAYBE(exception, kj::runCatchingExceptions([&]() {
    auto registry = getModulesForResolveCallback(isolate);
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
      if (!resolver->Resolve(context, v8::Undefined(isolate)).IsJust()) {
        // Return empty local and allow error to propagate.
        return v8::Local<v8::Promise>();
      }
      return resolver->GetPromise();
    };

    auto& synthetic = KJ_REQUIRE_NONNULL(ref.module.maybeSynthetic, "Not a synthetic module.");

    KJ_SWITCH_ONEOF(synthetic) {
      KJ_CASE_ONEOF(info, ModuleRegistry::CapnpModuleInfo) {
        bool success = true;
        success = success && module->SetSyntheticModuleExport(
            isolate,
            v8StrIntern(isolate, "default"_kj),
            info.fileScope.getHandle(isolate)).IsJust();
        for (auto& decl: info.topLevelDecls) {
          success = success && module->SetSyntheticModuleExport(
              isolate,
              v8StrIntern(isolate, decl.key),
              decl.value.getHandle(isolate)).IsJust();
        }

        if (success) {
          result = makeResolvedPromise();
        } else {
          // leave `result` empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::CommonJsModuleInfo) {
        v8::TryCatch catcher(isolate);
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
                isolate,
                v8StrIntern(isolate, "default"_kj),
                commonjs.moduleContext->module->getExports(js.v8Isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave `result` empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::TextModuleInfo) {
        if (module->SetSyntheticModuleExport(isolate,
                                             v8StrIntern(isolate, "default"_kj),
                                             info.value.getHandle(isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::DataModuleInfo) {
        if (module->SetSyntheticModuleExport(isolate,
                                             v8StrIntern(isolate, "default"_kj),
                                             info.value.getHandle(isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::WasmModuleInfo) {
        if (module->SetSyntheticModuleExport(isolate,
                                             v8StrIntern(isolate, "default"_kj),
                                             info.value.getHandle(isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::JsonModuleInfo) {
        if (module->SetSyntheticModuleExport(isolate,
                                             v8StrIntern(isolate, "default"_kj),
                                             info.value.getHandle(isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
      KJ_CASE_ONEOF(info, ModuleRegistry::ObjectModuleInfo) {
        if (module->SetSyntheticModuleExport(isolate,
                                             v8StrIntern(isolate, "default"_kj),
                                             info.value.getHandle(isolate)).IsJust()) {
          result = makeResolvedPromise();
        } else {
          // leave 'result' empty to propagate the JS exception
        }
      }
    }
  })) {
    // V8 doc comments say in the case of an error, throw the error and return an empty Maybe.
    // I.e. NOT a rejected promise. OK...
    context->GetIsolate()->ThrowException(makeInternalError(isolate, kj::mv(*exception)));
    result = v8::Local<v8::Promise>();
  }

  return scope.EscapeMaybe(result);
}

}  // namespace

v8::Local<v8::Value> CommonJsModuleContext::require(kj::String specifier, v8::Isolate* isolate) {
  auto modulesForResolveCallback = getModulesForResolveCallback(isolate);
  KJ_REQUIRE(modulesForResolveCallback != nullptr, "didn't expect resolveCallback() now");

  kj::Path targetPath = path.parent().eval(specifier);
  auto& js = Lock::from(isolate);

  // require() is only exposed to worker bundle modules so the resolve here is only
  // permitted to require worker bundle or built-in modules. Internal modules are
  // excluded.
  auto& info = JSG_REQUIRE_NONNULL(modulesForResolveCallback->resolve(js, targetPath),
      Error, "No such module \"", targetPath.toString(), "\".");
  // Adding imported from suffix here not necessary like it is for resolveCallback, since we have a
  // js stack that will include the parent module's name and location of the failed require().

  JSG_REQUIRE_NONNULL(info.maybeSynthetic, TypeError,
      "Cannot use require() to import an ES Module.");

  auto module = info.module.getHandle(js);
  auto context = isolate->GetCurrentContext();

  check(module->InstantiateModule(context, &resolveCallback));
  auto handle = check(module->Evaluate(context));
  KJ_ASSERT(handle->IsPromise());
  auto prom = handle.As<v8::Promise>();

  // This assert should always pass since evaluateSyntheticModuleCallback() for CommonJS
  // modules (below) always returns an already-resolved promise.
  KJ_ASSERT(prom->State() != v8::Promise::PromiseState::kPending);

  if (module->GetStatus() == v8::Module::kErrored) {
    throwTunneledException(isolate, module->GetException());
  }

  // Originally, This returned an object like `{default: module.exports}` when we really
  // intended to return the module exports raw. We should be extracting `default` here.
  // Unfortunately, there is a user depending on the wrong behavior in production, so we
  // needed a compatibility flag to fix.
  if (getCommonJsExportDefault(isolate)) {
    return check(module->GetModuleNamespace().As<v8::Object>()
        ->Get(context, v8StrIntern(isolate, "default")));
  } else {
    return module->GetModuleNamespace();
  }
}

void NonModuleScript::run(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.Get(isolate)->BindToCurrentContext();
  check(boundScript->Run(context));
}

NonModuleScript NonModuleScript::compile(kj::StringPtr code, jsg::Lock& js, kj::StringPtr name) {
  // Create a dummy script origin for it to appear in Sources panel.
  auto isolate = js.v8Isolate;
  v8::ScriptOrigin origin(isolate, v8StrIntern(isolate, name));
  v8::ScriptCompiler::Source source(v8Str(isolate, code), origin);
  return NonModuleScript(js,
      check(v8::ScriptCompiler::CompileUnboundScript(isolate, &source)));
}

void instantiateModule(jsg::Lock& js, v8::Local<v8::Module>& module) {
  KJ_ASSERT(!module.IsEmpty());
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  jsg::check(module->InstantiateModule(context, &resolveCallback));
  auto prom = jsg::check(module->Evaluate(context)).As<v8::Promise>();

  isolate->PerformMicrotaskCheckpoint();

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
v8::Local<v8::Module> compileEsmModule(
    jsg::Lock& js,
    kj::StringPtr name,
    kj::ArrayPtr<const char> content,
    ModuleInfoCompileOption option) {
  // Must pass true for `is_module`, but we can skip everything else.
  const int resourceLineOffset = 0;
  const int resourceColumnOffset = 0;
  const bool resourceIsSharedCrossOrigin = false;
  const int scriptId = -1;
  const bool resourceIsOpaque = false;
  const bool isWasm = false;
  const bool isModule = true;
  v8::ScriptOrigin origin(js.v8Isolate,
                          v8StrIntern(js.v8Isolate, name),
                          resourceLineOffset,
                          resourceColumnOffset,
                          resourceIsSharedCrossOrigin, scriptId, {},
                          resourceIsOpaque, isWasm, isModule);
  v8::Local<v8::String> contentStr;

  if (option == ModuleInfoCompileOption::BUILTIN) {
    // TODO(later): Use of newExternalOneByteString here limits our built-in source
    // modules (for which this path is used) to only the latin1 character set. We
    // may need to revisit that to import built-ins as UTF-16 (two-byte).
    contentStr = jsg::check(jsg::newExternalOneByteString(js, content));

    // TODO(bug): The cache is failing under certain conditions. Disabling this logic
    // for now until it can be debugged.
    // const auto& compileCache = CompileCache::get();
    // KJ_IF_MAYBE(cached, compileCache.find(content.begin())) {
    //   v8::ScriptCompiler::Source source(contentStr, origin, cached);
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
  KJ_IF_MAYBE(exports, maybeExports) {
    for (auto& name : *exports) {
      exportNames.push_back(v8StrIntern(js.v8Isolate, name));
    }
  }
  return v8::Module::CreateSyntheticModule(
      js.v8Isolate,
      v8StrIntern(js.v8Isolate, name),
      exportNames,
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
    CompileOption flags)
    : ModuleInfo(js, compileEsmModule(js, name, content, flags)) {}

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
  return jsg::alloc<jsg::CommonJsModuleContext>(js.v8Isolate, kj::Path::parse(name));
}

ModuleRegistry::CapnpModuleInfo::CapnpModuleInfo(
    Value fileScope,
    kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls)
    : fileScope(kj::mv(fileScope)),
      topLevelDecls(kj::mv(topLevelDecls)) {}

}  // namespace workerd::jsg
