// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include <kj/filesystem.h>
#include <kj/map.h>

namespace workerd::jsg {

class CommonJsModuleContext;

class CommonJsModuleObject: public jsg::Object {
public:
  CommonJsModuleObject(v8::Isolate* isolate) : exports(isolate, v8::Object::New(isolate)) {}

  v8::Local<v8::Value> getExports(v8::Isolate* isolate) { return exports.getHandle(isolate); }
  void setExports(jsg::Value value) { exports = kj::mv(value); }

  JSG_RESOURCE_TYPE(CommonJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
  }
private:
  jsg::Value exports;
};

class CommonJsModuleContext: public jsg::Object {
public:
  CommonJsModuleContext(v8::Isolate* isolate, kj::Path path)
      : module(jsg::alloc<CommonJsModuleObject>(isolate)), path(kj::mv(path)),
      exports(isolate, module->getExports(isolate)) {}

  v8::Local<v8::Value> require(kj::String specifier, v8::Isolate* isolate);

  jsg::Ref<CommonJsModuleObject> getModule(v8::Isolate* isolate) { return module.addRef(); }

  v8::Local<v8::Value> getExports(v8::Isolate* isolate) { return exports.getHandle(isolate); }
  void setExports(jsg::Value value) { exports = kj::mv(value); }

  JSG_RESOURCE_TYPE(CommonJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
  }

  jsg::Ref<CommonJsModuleObject> module;
private:
  kj::Path path;
  jsg::Value exports;
};

class NonModuleScript {
  // jsg::NonModuleScript wraps a v8::UnboundScript.
public:
  NonModuleScript(v8::Isolate* isolate, v8::Local<v8::UnboundScript> script)
      : unboundScript(isolate, script) {}

  NonModuleScript(NonModuleScript&&) = default;
  NonModuleScript& operator=(NonModuleScript&&) = default;

  void run(v8::Local<v8::Context> context) const;
  // Running the script will create a v8::Script instance bound to the given
  // context then will run it to completion.

  static jsg::NonModuleScript compile(kj::StringPtr code, v8::Isolate* isolate);

private:
  v8::Global<v8::UnboundScript> unboundScript;
};

v8::Local<v8::WasmModuleObject> compileWasmModule(v8::Isolate* isolate, auto&& reader) {
  return jsg::check(v8::WasmModuleObject::Compile(
      isolate,
      v8::MemorySpan<const uint8_t>(reader.begin(), reader.size())));
}

void instantiateModule(v8::Isolate* isolate, v8::Local<v8::Module>& module);

class ModuleRegistry {
  // The ModuleRegistry maintains the collection of modules known to a script that can be
  // required or imported.
public:

  static inline ModuleRegistry* from(v8::Isolate* isolate) {
    return static_cast<ModuleRegistry*>(
        isolate->GetCurrentContext()->GetAlignedPointerFromEmbedderData(2));
  }

  struct CapnpModuleInfo {
    Value fileScope;  // default import
    kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls;  // named imports

    CapnpModuleInfo(Value fileScope,
                    kj::HashMap<kj::StringPtr, jsg::Value> topLevelDecls);
    CapnpModuleInfo(CapnpModuleInfo&&) = default;
    CapnpModuleInfo& operator=(CapnpModuleInfo&&) = default;
  };
  struct CommonJsModuleInfo {
    Ref<CommonJsModuleContext> moduleContext;
    jsg::Function<void()> evalFunc;

    CommonJsModuleInfo(auto& lock, kj::StringPtr name, kj::StringPtr content)
        : moduleContext(initModuleContext(lock.v8Isolate, name)),
          evalFunc(initEvalFunc(lock, moduleContext, name, content)) {}

    CommonJsModuleInfo(CommonJsModuleInfo&&) = default;
    CommonJsModuleInfo& operator=(CommonJsModuleInfo&&) = default;

    static Ref<CommonJsModuleContext> initModuleContext(
        v8::Isolate* isolate,
        kj::StringPtr name);

    static jsg::Function<void()> initEvalFunc(
        auto& lock,
        Ref<CommonJsModuleContext>& moduleContext,
        kj::StringPtr name,
        kj::StringPtr content) {
      v8::ScriptOrigin origin(lock.v8Isolate, v8StrIntern(lock.v8Isolate, name));
      v8::ScriptCompiler::Source source(v8Str(lock.v8Isolate, content), origin);
      auto context = lock.v8Isolate->GetCurrentContext();
      auto handle = lock.wrap(context, moduleContext.addRef());
      auto fn = jsg::check(v8::ScriptCompiler::CompileFunction(
          context,
          &source,
          0, nullptr,
          1, &handle));
      return lock.template unwrap<jsg::Function<void()>>(context, fn);
    }
  };

  template <typename T>
  struct ValueModuleInfo {
    jsg::V8Ref<T> value;

    ValueModuleInfo(v8::Isolate* isolate, v8::Local<T> value) : value(isolate, value) {}

    ValueModuleInfo(ValueModuleInfo&&) = default;
    ValueModuleInfo& operator=(ValueModuleInfo&&) = default;
  };

  using DataModuleInfo = ValueModuleInfo<v8::ArrayBuffer>;
  using TextModuleInfo = ValueModuleInfo<v8::String>;
  using WasmModuleInfo = ValueModuleInfo<v8::WasmModuleObject>;
  using JsonModuleInfo = ValueModuleInfo<v8::Value>;

  struct ModuleInfo {
    v8::Global<v8::Module> module;
    int hash;

    using SyntheticModuleInfo = kj::OneOf<CapnpModuleInfo,
                                          CommonJsModuleInfo,
                                          DataModuleInfo,
                                          TextModuleInfo,
                                          WasmModuleInfo,
                                          JsonModuleInfo>;
    kj::Maybe<SyntheticModuleInfo> maybeSynthetic;

    ModuleInfo(v8::Isolate* isolate,
               v8::Local<v8::Module> module,
               kj::Maybe<SyntheticModuleInfo> maybeSynthetic = nullptr);

    ModuleInfo(v8::Isolate* isolate, kj::StringPtr name, kj::StringPtr content);

    ModuleInfo(v8::Isolate* isolate, kj::StringPtr name,
               kj::Maybe<kj::ArrayPtr<kj::StringPtr>> maybeExports,
               SyntheticModuleInfo synthetic);

    ModuleInfo(ModuleInfo&&) = default;
    ModuleInfo& operator=(ModuleInfo&&) = default;
  };

  virtual kj::Maybe<ModuleInfo&> resolve(const kj::Path& specifier) = 0;

  virtual kj::Maybe<ModuleInfo&> resolve(v8::Local<v8::Module> module) = 0;

  virtual kj::Maybe<const kj::Path&> resolvePath(v8::Local<v8::Module> referrer)= 0;

  virtual Promise<Value> resolveDynamicImport(v8::Isolate* isolate, kj::Path specifier) = 0;

  using DynamicImportCallback = Promise<Value>(v8::Isolate*, kj::Function<Value()> handler);
  // The dynamic import callback is provided by the embedder to set up any context necessary
  // for instantiating the module during a dynamic import. The handler function passed into
  // the callback is called to actually perform the instantiation of the module.

  virtual void setDynamicImportCallback(kj::Function<DynamicImportCallback> func) = 0;
};

template <typename TypeWrapper>
class ModuleRegistryImpl final: public ModuleRegistry {
public:
  void setDynamicImportCallback(kj::Function<DynamicImportCallback> func) override {
    dynamicImportHandler = kj::mv(func);
  }

  void add(kj::Path& specifier, ModuleInfo&& info) {
    entries.insert(Entry(specifier, kj::fwd<ModuleInfo>(info)));
  }

  kj::Maybe<ModuleInfo&> resolve(const kj::Path& specifier) override {
    // TODO(soon): Soon we will support prefixed imports of Workers built in types.
    KJ_IF_MAYBE(entry, entries.find(specifier)) {
      return entry->info;
    }
    return nullptr;
  }

  kj::Maybe<ModuleInfo&> resolve(v8::Local<v8::Module> module) override {
    KJ_IF_MAYBE(entry, entries.template find<1>(module)) {
      return entry->info;
    }
    return nullptr;
  }

  kj::Maybe<const kj::Path&> resolvePath(v8::Local<v8::Module> module) override {
    KJ_IF_MAYBE(entry, entries.template find<1>(module)) {
      return entry->specifier;
    }
    return nullptr;
  }

  size_t size() const { return entries.size(); }

  Promise<Value> resolveDynamicImport(v8::Isolate* isolate, kj::Path specifier) override {
    KJ_IF_MAYBE(info, resolve(specifier)) {
      KJ_IF_MAYBE(func, dynamicImportHandler) {
        auto handler = [&info = *info, isolate]() -> Value {
          auto module = info.module.Get(isolate);
          instantiateModule(isolate, module);
          return Value(isolate, module->GetModuleNamespace());
        };
        return (*func)(isolate, kj::mv(handler));
      }

      // If there is no dynamicImportHandler set, then we are going to handle that as if
      // the module does not exist and fall through to the rejected promise below.
    }

    return rejectedPromise<Value>(isolate,
        v8::Exception::Error(v8Str(isolate,
            kj::str("No such module \"", specifier.toString(), "\"."))));
  }

private:
  kj::Maybe<kj::Function<DynamicImportCallback>> dynamicImportHandler;

  // When we build a bundle containing modules, we must build a table of modules to resolve imports.
  //
  // Because of the design of V8's resolver callback, we end up needing a table with two indexes:
  // we need to be able to search it by path (filename) as well as search for a specific module
  // object by identity. We use a kj::Table!
  struct Entry {
    kj::Path specifier;
    ModuleInfo info;

    Entry(kj::Path& specifier, ModuleInfo info)
        : specifier(specifier.clone()), info(kj::mv(info)) {}
    Entry(Entry&&) = default;
    Entry& operator=(Entry&&) = default;
  };

  struct SpecifierHashCallbacks {
    const kj::Path& keyForRow(const Entry& row) const { return row.specifier; }

    bool matches(const Entry& row, const kj::Path& specifier) const {
      return row.specifier == specifier;
    }

    uint hashCode(const kj::Path& specifier) const {
      return specifier.hashCode();
    }
  };

  struct InfoHashCallbacks {
    const Entry& keyForRow(const Entry& row) const { return row; }

    bool matches(const Entry& entry, const Entry& other) const {
      return entry.info.hash == other.info.hash;
    }

    bool matches(const Entry& entry, v8::Local<v8::Module>& module) const {
      return entry.info.hash == module->GetIdentityHash();
    }

    uint hashCode(v8::Local<v8::Module>& module) const {
      return kj::hashCode(module->GetIdentityHash());
    }

    uint hashCode(const Entry& entry) const {
      return entry.info.hash;
    }
  };

  kj::Table<Entry, kj::HashIndex<SpecifierHashCallbacks>,
                   kj::HashIndex<InfoHashCallbacks>> entries;
};

template <typename TypeWrapper>
v8::MaybeLocal<v8::Promise> dynamicImportCallback(v8::Local<v8::Context> context,
                                                  v8::Local<v8::Data> host_defined_options,
                                                  v8::Local<v8::Value> resource_name,
                                                  v8::Local<v8::String> specifier,
                                                  v8::Local<v8::FixedArray> import_assertions) {
  auto isolate = context->GetIsolate();
  auto registry = ModuleRegistry::from(isolate);
  auto& wrapper = TypeWrapper::from(isolate);

  const auto makeRejected = [&](auto reason) {
    v8::Local<v8::Promise::Resolver> resolver;
    if (v8::Promise::Resolver::New(context).ToLocal(&resolver) &&
        resolver->Reject(context, reason).IsJust()) {
      return resolver->GetPromise();
    }
    return v8::Local<v8::Promise>();
  };

  // The dynamic import might be resolved synchronously or asynchronously.
  // Accordingly, resolveDynamicImport will return a jsg::Promise<jsg::Value>
  // that will resolve to the module's namespace object or will reject if there
  // was any error.
  //
  // Importantly, we defensively catch any synchronous errors here and handle them
  // explicitly as rejected Promises.
  v8::TryCatch tryCatch(isolate);
  try {
    auto what = kj::Path::parse(kj::str(resource_name)).parent().eval(kj::str(specifier));
    // TODO(soon): If kj::Path::parse fails it is most likely the application's fault and yet
    // we end up throwing an "internal error" here. We could handle this more gracefully
    // (and correctly) if kj::Path had a tryEval() variant.
    return wrapper.wrap(context, nullptr, registry->resolveDynamicImport(isolate, kj::mv(what)));
  } catch (JsExceptionThrown&) {
    if (!tryCatch.CanContinue()) {
      // There's nothing else we can reasonably do.
      return v8::MaybeLocal<v8::Promise>();
    }
    return makeRejected(tryCatch.Exception());
  } catch (kj::Exception& ex) {
    return makeRejected(makeInternalError(isolate, kj::mv(ex)));
  }
  KJ_UNREACHABLE;
}

template <typename TypeWrapper>
void setModulesForResolveCallback(v8::Isolate* isolate, ModuleRegistry* table) {
  KJ_ASSERT(table != nullptr);
  isolate->GetCurrentContext()->SetAlignedPointerInEmbedderData(2, table);
  isolate->SetHostImportModuleDynamicallyCallback(dynamicImportCallback<TypeWrapper>);
}

}  // namespace workerd::jsg
