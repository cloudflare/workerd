// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg.h"
#include "modules.h"
#include "promise.h"
#include <kj/mutex.h>
#include <kj/map.h>
#include <kj/filesystem.h>
#include <set>
#include <ranges>
#include <workerd/util/thread-scopes.h>

namespace workerd::jsg::modules {

// ======================================================================================
// CommonJSModuleContext and CommonJSModuleObject

CommonJsModuleContext::CommonJsModuleContext(Lock& js, Module& inner)
    : inner(inner),
      module(alloc<CommonJsModuleObject>(js)) {}

Ref<CommonJsModuleObject> CommonJsModuleContext::getModule(Lock& js) {
  return module.addRef();
}

JsValue CommonJsModuleContext::getExports(Lock& js) {
  return module->getExports(js);
}

void CommonJsModuleContext::setExports(Lock& js, JsValue value) {
  module->setExports(js, value);
}

JsValue CommonJsModuleContext::require(Lock& js, kj::String specifier) {
  auto resolveOption = ModuleRegistry::ResolveOption::DEFAULT;
  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  KJ_IF_SOME(target, Url::tryParse(specifier, inner.specifier().getHref())) {
    KJ_IF_SOME(module, registry.resolve(js, target, resolveOption)) {
      auto obj = Module::evaluate(js, module);

      // Originally, This returned an object like `{default: module.exports}` when we really
      // intended to return the module exports raw. We should be extracting `default` here.
      // Unfortunately, there is a user depending on the wrong behavior in production, so we
      // needed a compatibility flag to fix.
      if (getCommonJsExportDefault(js.v8Isolate)) {
        return obj.get(js, "default"_kj);
      } else {
        return obj;
      }
    }
  }
  JSG_FAIL_REQUIRE(Error, "No such module \"", specifier, "\".");
}

CommonJsModuleObject::CommonJsModuleObject(Lock& js) : exports(js, js.obj()) {}

JsValue CommonJsModuleObject::getExports(Lock& js) {
  return exports.getHandle(js);
}

void CommonJsModuleObject::setExports(Lock& js, JsValue value) {
  exports = JsRef<JsValue>(js, value);
}

// ======================================================================================
// NodeJsModuleContext and NodeJsModuleObject
NodeJsModuleContext::NodeJsModuleContext(Lock& js, Module& inner)
    : inner(inner),
      module(alloc<NodeJsModuleObject>(js, inner)),
      exports(js, module->getExports(js)) {}

JsValue NodeJsModuleContext::require(Lock& js, kj::String specifier) {
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
    "zlib",                "test"
  };

  // require() is only exposed to worker bundle modules so the resolve here is only
  // permitted to require worker bundle or built-in modules. Internal modules are
  // excluded.
  auto resolveOption = ModuleRegistry::ResolveOption::DEFAULT;

  // If it is a bare specifier known to be a Node.js built-in, then prefix the
  // specifier with node:
  if (NODEJS_BUILTINS.contains(specifier)) {
    specifier = kj::str("node:", specifier);
    resolveOption = ModuleRegistry::ResolveOption::BUILTIN;
  } else if (specifier.startsWith("node:")) {
    resolveOption = ModuleRegistry::ResolveOption::BUILTIN;
  }

  auto& registry = IsolateModuleRegistry::from(js.v8Isolate);
  KJ_IF_SOME(target, Url::tryParse(specifier, FILE_ROOT)) {
    KJ_IF_SOME(module, registry.resolve(js, target, resolveOption)) {
      auto obj = Module::evaluate(js, module);
      return obj.get(js, "default"_kj);
    }
  }
  JSG_FAIL_REQUIRE(Error, "No such module \"", specifier, "\".");
}

JsValue NodeJsModuleContext::getBuffer(Lock& js) {
  auto value = require(js, kj::str("node:buffer"));
  JSG_REQUIRE(value.isObject(), TypeError, "Invalid node:buffer implementation");
  auto obj = JSG_REQUIRE_NONNULL(value.tryCast<JsObject>(), TypeError,
                                 "Invalid node:buffer implementation");
  auto buffer = obj.get(js, "Buffer"_kj);
  JSG_REQUIRE(buffer.isFunction(), TypeError, "Invalid node:buffer implementation");
  return buffer;
}

JsValue NodeJsModuleContext::getProcess(Lock& js) {
  auto value = require(js, kj::str("node:process"));
  JSG_REQUIRE(value.isObject(), TypeError, "Invalid node:process implementation");
  return value;
}

kj::String NodeJsModuleContext::getFilename() {
  // For __filename, we extract the path from the specifier URL for this module,
  // parse it as a filepath, and extract the basename component only.
  auto str = kj::str(inner.specifier().getPathname());
  auto path = kj::Path::parse(str);
  return path.basename().toString();
}

kj::String NodeJsModuleContext::getDirname() {
  // For __dirname, we extract the path from the specifier URL for this module,
  // parse it as a filepath, and extract the parent component only.
  auto str = kj::str(inner.specifier().getPathname());
  auto path = kj::Path::parse(str);
  return path.parent().toString(true);
}

Ref<NodeJsModuleObject> NodeJsModuleContext::getModule(Lock& js) {
  return module.addRef();
}

JsValue NodeJsModuleContext::getExports(Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleContext::setExports(Lock& js, JsValue value) {
  exports = JsRef(js, value);
}

NodeJsModuleObject::NodeJsModuleObject(Lock& js, Module& inner)
    : inner(inner), exports(js, js.obj()) {}

JsValue NodeJsModuleObject::getExports(Lock& js) {
  return exports.getHandle(js);
}

void NodeJsModuleObject::setExports(Lock& js, JsValue value) {
  exports = JsRef(js, value);
}

kj::ArrayPtr<const char> NodeJsModuleObject::getPath() {
  return inner.specifier().getPathname();
}

// ======================================================================================
// NonModuleScript
void NonModuleScript::run(v8::Local<v8::Context> context) const {
  auto isolate = context->GetIsolate();
  auto boundScript = unboundScript.Get(isolate)->BindToCurrentContext();
  check(boundScript->Run(context));
}

NonModuleScript NonModuleScript::compile(Lock& js, kj::StringPtr code, kj::StringPtr name) {
  // Create a dummy script origin for it to appear in Sources panel.
  auto isolate = js.v8Isolate;
  v8::ScriptOrigin origin(isolate, v8StrIntern(isolate, name));
  v8::ScriptCompiler::Source source(v8Str(isolate, code), origin);
  return NonModuleScript(js,
      check(v8::ScriptCompiler::CompileUnboundScript(isolate, &source)));
}

// ===================================================================================
// Module, ModuleBundle, ModuleRegistry
namespace {
static CompilationObserver::Option convertOption(Module::Type type) {
  KJ_DASSERT(type <= Module::Type::INTERNAL);
  switch (type) {
    case Module::Type::BUILTIN: return CompilationObserver::Option::BUILTIN;
    case Module::Type::INTERNAL: return CompilationObserver::Option::BUILTIN;
    case Module::Type::BUNDLE: return CompilationObserver::Option::BUNDLE;
  }
  KJ_UNREACHABLE;
}

kj::Maybe<Module&> tryResolve(kj::ArrayPtr<kj::Own<ModuleBundle>> bundles,
                              const Url& specifier,
                              Module::Type type) {
  for (auto& bundle : bundles) {
    if (bundle->type() == type) {
      KJ_IF_SOME(resolved, bundle->resolve(specifier)) {
        return resolved;
      }
    }
  }
  return kj::none;
}

v8::Local<v8::Promise> makeResolvedPromise(Lock& js) {
  v8::Local<v8::Promise::Resolver> resolver;
  auto context = js.v8Context();
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

v8::Local<v8::Promise> makeRejectedPromise(Lock& js, Value& exception) {
  v8::Local<v8::Promise::Resolver> resolver;
  if (v8::Promise::Resolver::New(js.v8Context()).ToLocal(&resolver) &&
      resolver->Reject(js.v8Context(), exception.getHandle(js)).IsJust()) {
    return resolver->GetPromise();
  }
  return v8::Local<v8::Promise>();
}

class ModuleBundleImpl final: public ModuleBundle {
public:
  ModuleBundleImpl(ModuleBundle::Type type, kj::Array<ModuleBundleBuilder::Entry> entries)
      : ModuleBundle(type) {
    auto lock = modules.lockExclusive();
    for (auto& entry : entries) {
      lock->upsert(kj::mv(entry.specifier), kj::mv(entry.factory));
    }
  }

  kj::Maybe<Module&> resolve(const Url& specifier) {
    auto lock = modules.lockExclusive();
    KJ_IF_SOME(found, lock->find(specifier)) {
      KJ_SWITCH_ONEOF(found) {
        KJ_CASE_ONEOF(factory, BuiltinModuleBundleBuilder::Factory) {
          auto module = factory(specifier);
          auto& ret = *module;
          lock->upsert(specifier.clone(), kj::mv(module));
          return ret;
        }
        KJ_CASE_ONEOF(module, kj::Own<Module>) {
          return *module;
        }
      }
    }
    return kj::none;
  }

private:
  using ModuleSource = kj::OneOf<ModuleBundleBuilder::Factory, kj::Own<Module>>;
  kj::MutexGuarded<kj::HashMap<Url, ModuleSource>> modules;
};

v8::MaybeLocal<v8::Value> evaluateSynthetic(v8::Local<v8::Context> context,
                                            v8::Local<v8::Module> module) {
  auto isolate = context->GetIsolate();
  auto& js = Lock::from(isolate);
  auto& registry = IsolateModuleRegistry::from(isolate);

  // If we got to this point, the module should be known to the registry.
  KJ_IF_SOME(resolved, registry.resolve(js, module)) {
    return js.tryCatch([&]() -> v8::MaybeLocal<v8::Value> {
      if (!resolved.load(js, module)) {
        // If load returns false, that implies an error! We will return an empty
        // value to propagate the error.
        return v8::Local<v8::Value>();
      }
      return makeResolvedPromise(js).As<v8::Value>();
    }, [&](Value exception) -> v8::MaybeLocal<v8::Value> {
      isolate->ThrowException(exception.getHandle(js));
      return v8::Local<v8::Value>();
    });
  }

  // If we got here, the module was not found in the table. That's odd.
  // We schedule an error on the isolate and return an empty value.
  isolate->ThrowError(js.strIntern(MODULE_NOT_FOUND));
  return v8::Local<v8::Promise>();
}

class TextModule final: public SyntheticModule {
public:
  TextModule(Type type, Url url, kj::String data, Flags flags)
      : SyntheticModule(type, kj::mv(url), flags), data(kj::mv(data)) {}

  JsValue getValue(Lock& js) override {
    // Ideally we'd be able to make this externalized so that a copy of the string data
    // does not need to be uploaded into the isolate. Unfortunately, this string data is
    // UTF8 and V8 does not expose any mechanism for externalized UTF8, so we are forced
    // here to copy.
    return js.str(data);
  }

private:
  kj::String data;
};

class DataModule final: public SyntheticModule {
public:
  DataModule(Type type, Url url, kj::Array<const kj::byte> data, Flags flags)
      : SyntheticModule(type, kj::mv(url), flags), data(kj::mv(data)) {}

  JsValue getValue(Lock& js) override {
    // Unfortunately we have to copy here. Why? Well, this type of module ends up being
    // exposed as a *mutable* ArrayBuffer. We don't want the master state here being
    // mutated. Eventually it would be better to expose this as either a copy-on-write
    // BackingStore (if that is ever implemented by v8) or an immutable BackingStore.
    return JsValue(js.wrapBytes(kj::heapArray<kj::byte>(data)));
  }

private:
  kj::Array<const kj::byte> data;
};

class WasmModule final: public SyntheticModule {
public:
  WasmModule(Type type, Url url, kj::Array<const uint8_t> code, Flags flags,
             CompilationObserver& observer)
      : SyntheticModule(type, kj::mv(url), flags),
        code(kj::mv(code)),
        observer(observer) {}

  JsValue getValue(Lock& js) override {
    return JsValue(compileWasmModule(js, code, observer).As<v8::Value>());
  }

private:
  kj::Array<const uint8_t> code;
  CompilationObserver& observer;
};

class JsonModule final: public SyntheticModule {
public:
  JsonModule(Type type, Url url, kj::String data, Flags flags)
      : SyntheticModule(type, kj::mv(url), flags), data(kj::mv(data)) {}

  JsValue getValue(Lock& js) override {
    return JsValue(js.parseJson(data).getHandle(js));
  }

private:
  kj::String data;
};

class EsmModule final: public Module {
public:
  enum class Option {
    UTF8,
    EXTERN,
  };

  EsmModule(Type type, Url url, kj::String code, Flags flags, CompilationObserver& observer)
      : Module(type, kj::mv(url), flags | Flags::ESM),
        code(kj::mv(code)),
        source(this->code.asArray()),
        observer(observer),
        option(Option::UTF8) {}

  EsmModule(Type type, Url url, kj::ArrayPtr<const char> code, Flags flags,
            CompilationObserver& observer)
      : Module(type, kj::mv(url), flags),
        code(nullptr),
        source(code),
        observer(observer),
        option(Option::EXTERN) {}

  bool load(Lock& js, v8::Local<v8::Module> module) override {
    // load(...) is only called for synthetic modules. An EsmModule is never
    // synthethic, so this should never be called.
    KJ_UNREACHABLE;
  }

  v8::Local<v8::Module> getDescriptor(Lock& js) override {
    // Must pass true for `is_module`, but we can skip everything else.
    const int resourceLineOffset = 0;
    const int resourceColumnOffset = 0;
    const bool resourceIsSharedCrossOrigin = false;
    const int scriptId = -1;
    const bool resourceIsOpaque = false;
    const bool isWasm = false;
    const bool isModule = true;
    v8::ScriptOrigin origin(js.v8Isolate,
                            js.strExtern(specifier().getHref()),
                            resourceLineOffset,
                            resourceColumnOffset,
                            resourceIsSharedCrossOrigin, scriptId, {},
                            resourceIsOpaque, isWasm, isModule);
    auto str = option == Option::EXTERN ?
        newExternalOneByteString(js, source) :
        js.str(source);

    auto span = observer.onEsmCompilationStart(js.v8Isolate, specifier().getHref(),
                                               convertOption(type()));
    v8::ScriptCompiler::Source source(str, origin);
    return check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source));
  }

private:
  kj::String code;
  kj::ArrayPtr<const char> source;
  CompilationObserver& observer;
  Option option;
};

v8::MaybeLocal<v8::Module> resolveCallback(v8::Local<v8::Context> context,
                                           v8::Local<v8::String> specifier,
                                           v8::Local<v8::FixedArray> import_assertions,
                                           v8::Local<v8::Module> referrer) {
  static const auto moduleNotFound = [](kj::ArrayPtr<const char> spec, Module& referrer) {
    return kj::str("No such module \"", spec, "\".\n imported from \"",
                   referrer.specifier(), "\".");
  };

  auto& js = Lock::from(context->GetIsolate());
  return js.tryCatch([&] {
    auto& registry = IsolateModuleRegistry::from(js.v8Isolate);

    auto& ref = KJ_ASSERT_NONNULL(registry.resolve(js, referrer),
        "Referrer module isn't in modules table.");

    auto spec = kj::str(specifier);

    auto target = JSG_REQUIRE_NONNULL(Url::tryParse(spec, ref.specifier().getHref()),
        Error, moduleNotFound(spec, ref));

    // If the referrer module is a built-in, it is only permitted to resolve
    // internal modules. If the worker bundle provided an override for a builtin,
    // then internalOnly will be false.
    bool internalOnly = ref.type() == Module::Type::BUILTIN ||
                        ref.type() == Module::Type::INTERNAL;

    return JSG_REQUIRE_NONNULL(
        registry.resolve(js, target, internalOnly ?
            ModuleRegistry::ResolveOption::INTERNAL_ONLY :
            ModuleRegistry::ResolveOption::DEFAULT),
        Error, moduleNotFound(target.getHref(), ref));
  }, [&](Value value) -> v8::Local<v8::Module> {
    // We do not call js.throwException here since that will throw a JsExceptionThrown,
    // which we do not want here. Instead, we'll schedule an exception on the isolate
    // directly and set the result to an empty v8::MaybeLocal.
    js.v8Isolate->ThrowException(value.getHandle(js));
    return v8::Local<v8::Module>();
  });
}
}  // namespace

v8::Local<v8::WasmModuleObject> compileWasmModule(Lock& js,
                                                  kj::ArrayPtr<const uint8_t> code,
                                                  const CompilationObserver& observer) {
  js.setAllowEval(true);
  KJ_DEFER(js.setAllowEval(false));

  // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
  // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
  // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
  // compiles fast but runs slower.
  AllowV8BackgroundThreadsScope scope;

  auto span = observer.onWasmCompilationStart(js.v8Isolate, code.size());
  return check(v8::WasmModuleObject::Compile(js.v8Isolate,
      v8::MemorySpan<const uint8_t>(code.begin(), code.size())));
}

kj::Maybe<Module&> ModuleRegistry::resolve(
    const Url& specifier,
    ModuleRegistry::ResolveOption option) {
  if (option == ModuleRegistry::ResolveOption::INTERNAL_ONLY) {
    KJ_IF_SOME(resolved, tryResolve(bundles, specifier, Module::Type::INTERNAL)) {
      return resolved;
    }
  } else {
    if (option == ModuleRegistry::ResolveOption::DEFAULT) {
      KJ_IF_SOME(resolved, tryResolve(bundles, specifier, Module::Type::BUNDLE)) {
        return resolved;
      }
    }
    KJ_IF_SOME(resolved, tryResolve(bundles, specifier, Module::Type::BUILTIN)) {
      return resolved;
    }
  }
  return kj::none;
}

IsolateModuleRegistry::IsolateModuleRegistry(
    const ModuleRegistry& inner,
    v8::Isolate* isolate,
    kj::Maybe<DynamicImportHandler> dynamicImportHandler)
    : inner(inner), dynamicImportHandler(kj::mv(dynamicImportHandler)) {
  auto context = isolate->GetCurrentContext();
  context->SetAlignedPointerInEmbedderData(2, this);
  isolate->SetHostImportModuleDynamicallyCallback(&dynamicImport);
  isolate->SetHostInitializeImportMetaObjectCallback(&importMeta);
}

IsolateModuleRegistry& IsolateModuleRegistry::from(v8::Isolate* isolate) {
  auto context = isolate->GetCurrentContext();
  auto ptr = context->GetAlignedPointerFromEmbedderData(2);
  KJ_DASSERT(ptr != nullptr);
  return *static_cast<IsolateModuleRegistry*>(ptr);
}

IsolateModuleRegistry::Cached::Cached(Lock& js, v8::Local<v8::Module> module, bool internal)
    : ref(js.v8Ref(module)), internal(internal) {}

kj::Maybe<v8::Local<v8::Module>> IsolateModuleRegistry::resolve(
    Lock& js,
    const Url& specifier,
    ModuleRegistry::ResolveOption option) {
  // Have we already resolved the module?
  KJ_IF_SOME(cached, cache.find(specifier)) {
    if (cached.internal && option != ModuleRegistry::ResolveOption::INTERNAL_ONLY) {
      // For cached internal modules, we will only return the cached result if
      // the resolve option is INTERNAL_ONLY
      return kj::none;
    }
    return cached.ref.getHandle(js);
  }
  // No? Let's look for it.
  KJ_IF_SOME(resolved, const_cast<ModuleRegistry&>(inner).resolve(specifier, option)) {
    auto desc = resolved.getDescriptor(js);
    cache.upsert(specifier.clone(), Cached(js, desc, resolved.type() == Module::Type::INTERNAL));
    modules.upsert(HashableV8Ref<v8::Module>(js.v8Isolate, desc),
                   ModuleRef(resolved));
    return desc;
  }
  return kj::none;
}

kj::Maybe<Module&> IsolateModuleRegistry::resolve(Lock& js, v8::Local<v8::Module> module) {
  auto hash = HashableV8Ref<v8::Module>(js.v8Isolate, module);
  return modules.find(hash).map([](ModuleRef& ref) -> Module& {
    return ref;
  });
}

v8::MaybeLocal<v8::Promise> IsolateModuleRegistry::dynamicImport(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> import_assertions) {
  auto isolate = context->GetIsolate();
  auto& js = Lock::from(isolate);
  auto& registry = IsolateModuleRegistry::from(isolate);
  return js.tryCatch([&] {
    auto referrer = kj::str(resource_name);
    auto referrerUrl = KJ_ASSERT_NONNULL(Url::tryParse(referrer.asPtr()));
    auto module = KJ_ASSERT_NONNULL(registry.resolve(js, referrerUrl));
    auto& ref = KJ_ASSERT_NONNULL(registry.resolve(js, module));
    auto option = (ref.type() == Module::Type::BUILTIN || ref.type() == Module::Type::INTERNAL) ?
        ModuleRegistry::ResolveOption::INTERNAL_ONLY :
        ModuleRegistry::ResolveOption::DEFAULT;
    auto str = kj::str(specifier);

    KJ_IF_SOME(target, Url::tryParse(str.asPtr(), referrer.asPtr())) {
      KJ_IF_SOME(module, registry.resolve(js, target, option)) {
        KJ_IF_SOME(handler, registry.dynamicImportHandler) {
          auto result = handler(js, [handle=js.v8Ref(module)](Lock& js) mutable -> Value {
            auto module = handle.getHandle(js);
            Module::instantiate(js, module);
            return js.v8Ref(module->GetModuleNamespace());
          });
          return js.wrapSimplePromise(kj::mv(result));
        }
      }
    }
    JSG_FAIL_REQUIRE(Error,
                     kj::str("No such module \"", str, "\".\n imported from \"", referrer, "\"."));
  }, [&](Value exception) -> v8::Local<v8::Promise> {
    return makeRejectedPromise(js, exception);
  });
}

void IsolateModuleRegistry::importMeta(v8::Local<v8::Context> context,
                                       v8::Local<v8::Module> module,
                                       v8::Local<v8::Object> meta) {
  auto& js = Lock::from(context->GetIsolate());
  auto& registry = IsolateModuleRegistry::from(context->GetIsolate());
  auto obj = JsObject(meta);
  KJ_IF_SOME(resolved, registry.resolve(js, module)) {
    bool isMain = (resolved.flags() & Module::Flags::MAIN) == Module::Flags::MAIN;

    // import.meta.main
    // import.meta.url
    // import.meta.resolve(specifier)

    obj.set(js, "main"_kj, js.boolean(isMain));
    obj.set(js, "url"_kj, js.str(resolved.specifier().getHref()));

    auto resolve = js.wrapReturningFunction(js.v8Context(),
        [&resolved](Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args)
            -> v8::Local<v8::Value> {
      return js.tryCatch([&] {
        // Note that we intentionally use ToString here to coerce whatever value is given
        // into a string or throw if it cannot be coerced.
        auto specifier = kj::str(check(args[0]->ToString(js.v8Context())));
        KJ_IF_SOME(resolved, Url::tryParse(specifier.asPtr(), resolved.specifier().getHref())) {
          return v8::Local<v8::Value>(js.str(resolved.getHref()));
        } else {
          // If the specifier could not be parsed and resolved successfully,
          // the spec says to return null.
          return js.v8Null();
        }
      }, [&](Value exception) {
        // We only want to schedule the exception on the isolate here.
        // Do *not* throw a JsExceptionThrown.
        js.v8Isolate->ThrowException(exception.getHandle(js));
        return v8::Local<v8::Value>();
      });
    });

    obj.set(js, "resolve"_kj, JsValue(resolve));
  }
}

void Module::instantiate(Lock& js, v8::Local<v8::Module> module) {
  KJ_DASSERT(!module.IsEmpty());
  auto context = js.v8Context();

  auto status = module->GetStatus();
  // Nothing to do if the module is already instantiated, evaluated, or errored.
  if (status == v8::Module::Status::kInstantiated ||
      status == v8::Module::Status::kEvaluated ||
      status == v8::Module::Status::kErrored) return;

  JSG_REQUIRE(status == v8::Module::Status::kUninstantiated, Error,
      "Module cannot be imported or required while it is being instantiated or evaluated. "
      "This error typically means that a module has a circular dependency on itself using "
      "either require(...) or dynamic import(...).");

  check(module->InstantiateModule(context, &resolveCallback));
  auto prom = check(module->Evaluate(context)).As<v8::Promise>();
  js.runMicrotasks();

  switch (prom->State()) {
    case v8::Promise::kPending: {
      JSG_FAIL_REQUIRE(Error, "Async module was not immediate resolved.");
      break;
    }
    case v8::Promise::kRejected: {
      // Since we don't actually support I/O when instantiating a worker, we don't return the
      // promise from module->Evaluate, which means we lose any errors that happen during
      // instantiation if we don't throw the rejection exception here.
      js.throwException(JsValue(module->GetException()));
    }
    case v8::Promise::kFulfilled: return;
  }
  KJ_UNREACHABLE;
}

JsObject Module::evaluate(Lock& js, v8::Local<v8::Module> module) {
  instantiate(js, module);
  auto handle = check(module->Evaluate(js.v8Context()));
  KJ_DASSERT(handle->IsPromise());
  auto promise = handle.As<v8::Promise>();
  js.runMicrotasks();
  KJ_DASSERT(promise->State() != v8::Promise::PromiseState::kPending);
  if (module->GetStatus() == v8::Module::kErrored) {
    js.throwException(JsValue(module->GetException()));
  }
  auto ns = JsValue(module->GetModuleNamespace());
  return JSG_REQUIRE_NONNULL(ns.tryCast<JsObject>(), TypeError,
      "Module namespace is expected to be an object");
}

v8::Local<v8::Module> SyntheticModule::getDescriptor(Lock& js) {
  std::vector<v8::Local<v8::String>> exports;
  exports.push_back(js.strIntern(DEFAULT_STR));
  listExports(js, exports);
  return v8::Module::CreateSyntheticModule(
      js.v8Isolate,
      js.strExtern(specifier().getHref()),
      exports,
      &evaluateSynthetic);
}

bool SyntheticModule::load(Lock& js, v8::Local<v8::Module> module) {
  return module->SetSyntheticModuleExport(js.v8Isolate,
      js.strIntern(DEFAULT_STR),
      getValue(js)).IsJust();
}

kj::Own<ModuleBundle> ModuleBundleBuilder::finish() {
  return kj::heap<ModuleBundleImpl>(type, entries.releaseAsArray());
}

void BuiltinModuleBundleBuilder::add(kj::StringPtr specifier, Factory factory) {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(specifier),
      "The specifier must be a fully-qualified absolute URL");
  entries.add(Entry {
    .specifier = kj::mv(url),
    .factory = kj::mv(factory),
  });
}

void BuiltinModuleBundleBuilder::add(kj::StringPtr specifier, kj::ArrayPtr<const char> source) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier,
      [type=type, &observer=observer(), source](const Url& specifier) -> kj::Own<Module> {
    return kj::heap<EsmModule>(type, specifier.clone(), source, Module::Flags::NONE, observer);
  });
}

void BuiltinModuleBundleBuilder::add(Bundle::Reader bundle) {
  for (auto module : bundle.getModules()) {
    if (type == module.getType()) {
      // TODO: asChars() might be wrong for wide characters
      add(module.getName(), module.getSrc().asChars());
    }
  }
}

void WorkerModuleBundleBuilder::addTextModule(kj::StringPtr specifier, kj::String data,
                                              Module::Flags flags) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier, [type=type, data=kj::mv(data), flags](const Url& specifier) mutable {
    return kj::heap<TextModule>(type, specifier.clone(), kj::mv(data), flags);
  });
}

void WorkerModuleBundleBuilder::addDataModule(kj::StringPtr specifier,
                                              kj::Array<const kj::byte> data,
                                              Module::Flags flags) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier, [type=type, data=kj::mv(data), flags](const Url& specifier) mutable {
    return kj::heap<DataModule>(type, specifier.clone(), kj::mv(data), flags);
  });
}

void WorkerModuleBundleBuilder::addWasmModule(kj::StringPtr specifier,
                                              kj::Array<const uint8_t> code,
                                              Module::Flags flags) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier,
      [type=type, &observer=observer(), code=kj::mv(code), flags](const Url& specifier) mutable {
    return kj::heap<WasmModule>(type, specifier.clone(), kj::mv(code), flags, observer);
  });
}

void WorkerModuleBundleBuilder::addJsonModule(kj::StringPtr specifier, kj::String data,
                                              Module::Flags flags) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier, [type=type, data=kj::mv(data), flags](const Url& specifier) mutable {
    return kj::heap<JsonModule>(type, specifier.clone(), kj::mv(data), flags);
  });
}

void WorkerModuleBundleBuilder::addEsmModule(kj::StringPtr specifier, kj::String code,
                                              Module::Flags flags) {
  // Do *not* capture this in the lambda below. The builder instance will be destroyed
  // by the time the lambda is called.
  add(specifier,
      [type=type, &observer=observer(), code=kj::mv(code), flags](const Url& specifier) mutable {
    return kj::heap<EsmModule>(type, specifier.clone(), kj::mv(code), flags, observer);
  });
}

void WorkerModuleBundleBuilder::add(kj::StringPtr specifier, Factory factory) {
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(specifier, FILE_ROOT),
                               "Specifier is not a valid URL specifier");
  entries.add(Entry {
    .specifier = kj::mv(url),
    .factory = kj::mv(factory),
  });
}

v8::Local<v8::Value> resolvePropertyFromModule(Lock& js,
                                               kj::StringPtr name,
                                               kj::StringPtr property) {
  auto& registry = modules::IsolateModuleRegistry::from(js.v8Isolate);
  auto target = KJ_ASSERT_NONNULL(Url::tryParse(name, modules::FILE_ROOT));
  auto module = KJ_REQUIRE_NONNULL(
      registry.resolve(js, target, modules::ModuleRegistry::ResolveOption::INTERNAL_ONLY),
      "Could not resolve bootstrap module", name);
  auto obj = modules::Module::evaluate(js, module);
  return obj.get(js, property);
}

}  // namespace workerd::jsg::modules
