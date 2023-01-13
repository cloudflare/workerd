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
  NonModuleScript(jsg::Lock& js, v8::Local<v8::UnboundScript> script)
      : unboundScript(js.v8Isolate, script) {}

  NonModuleScript(NonModuleScript&&) = default;
  NonModuleScript& operator=(NonModuleScript&&) = default;

  void run(v8::Local<v8::Context> context) const;
  // Running the script will create a v8::Script instance bound to the given
  // context then will run it to completion.

  static jsg::NonModuleScript compile(kj::StringPtr code, jsg::Lock& js);

private:
  v8::Global<v8::UnboundScript> unboundScript;
};

v8::Local<v8::WasmModuleObject> compileWasmModule(jsg::Lock& js, auto&& reader) {
  return jsg::check(v8::WasmModuleObject::Compile(
      js.v8Isolate,
      v8::MemorySpan<const uint8_t>(reader.begin(), reader.size())));
}

void instantiateModule(jsg::Lock& js, v8::Local<v8::Module>& module);

class ModuleRegistry {
  // The ModuleRegistry maintains the collection of modules known to a script that can be
  // required or imported.
public:
  enum class Type {
    // BUNDLE is for modules provided by the worker bundle.
    // BUILTIN is for modules that are provided by the runtime and can be
    // imported by the worker bundle. These can be overridden by modules
    // in the worker bundle.
    // INTERNAL is for BUILTIN modules that can only be imported by other
    // BUILTIN modules. These cannot be overriden by modules in the worker
    // bundle.

    BUNDLE,
    BUILTIN,
    INTERNAL,
  };

  static inline ModuleRegistry* from(jsg::Lock& js) {
    return static_cast<ModuleRegistry*>(
        js.v8Isolate->GetCurrentContext()->GetAlignedPointerFromEmbedderData(2));
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
        : moduleContext(initModuleContext(lock, name)),
          evalFunc(initEvalFunc(lock, moduleContext, name, content)) {}

    CommonJsModuleInfo(CommonJsModuleInfo&&) = default;
    CommonJsModuleInfo& operator=(CommonJsModuleInfo&&) = default;

    static Ref<CommonJsModuleContext> initModuleContext(
        jsg::Lock& js,
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

    ValueModuleInfo(jsg::Lock& js, v8::Local<T> value) : value(js.v8Isolate, value) {}

    ValueModuleInfo(ValueModuleInfo&&) = default;
    ValueModuleInfo& operator=(ValueModuleInfo&&) = default;
  };

  using DataModuleInfo = ValueModuleInfo<v8::ArrayBuffer>;
  using TextModuleInfo = ValueModuleInfo<v8::String>;
  using WasmModuleInfo = ValueModuleInfo<v8::WasmModuleObject>;
  using JsonModuleInfo = ValueModuleInfo<v8::Value>;
  using ObjectModuleInfo = ValueModuleInfo<v8::Object>;

  struct ModuleInfo {
    HashableV8Ref<v8::Module> module;

    using SyntheticModuleInfo = kj::OneOf<CapnpModuleInfo,
                                          CommonJsModuleInfo,
                                          DataModuleInfo,
                                          TextModuleInfo,
                                          WasmModuleInfo,
                                          JsonModuleInfo,
                                          ObjectModuleInfo>;
    kj::Maybe<SyntheticModuleInfo> maybeSynthetic;

    ModuleInfo(jsg::Lock& js,
               v8::Local<v8::Module> module,
               kj::Maybe<SyntheticModuleInfo> maybeSynthetic = nullptr);

    enum class CompileOption {
      BUNDLE,
      // The BUNDLE options tells the compile operation to threat the content as coming
      // from a worker bundle.
      BUILTIN,
      // The BUILTIN option tells the compile operation to treat the content as a builtin
      // module. This implies certain changes in behavior, such as treating the content
      // as an immutable, process-lifetime buffer that will never be destroyed, and caching
      // the compilation data.
    };

    ModuleInfo(jsg::Lock& js,
               kj::StringPtr name,
               kj::ArrayPtr<const char> content,
               CompileOption flags = CompileOption::BUNDLE);

    ModuleInfo(jsg::Lock& js, kj::StringPtr name,
               kj::Maybe<kj::ArrayPtr<kj::StringPtr>> maybeExports,
               SyntheticModuleInfo synthetic);

    ModuleInfo(ModuleInfo&&) = default;
    ModuleInfo& operator=(ModuleInfo&&) = default;

    uint hashCode() const { return module.hashCode(); }
  };

  struct ModuleRef {
    const kj::Path& specifier;
    Type type;
    ModuleInfo& module;
  };

  virtual kj::Maybe<ModuleInfo&> resolve(jsg::Lock& js,
                                         const kj::Path& specifier,
                                         bool internalOnly = false) = 0;

  virtual kj::Maybe<ModuleRef> resolve(jsg::Lock& js, v8::Local<v8::Module> module) = 0;

  virtual Promise<Value> resolveDynamicImport(jsg::Lock& js,
                                              const kj::Path& specifier,
                                              const kj::Path& referrer) = 0;

  using DynamicImportCallback = Promise<Value>(jsg::Lock& js, kj::Function<Value()> handler);
  // The dynamic import callback is provided by the embedder to set up any context necessary
  // for instantiating the module during a dynamic import. The handler function passed into
  // the callback is called to actually perform the instantiation of the module.

  virtual void setDynamicImportCallback(kj::Function<DynamicImportCallback> func) = 0;
};

using ModuleInfoCompileOption = ModuleRegistry::ModuleInfo::CompileOption;

template <typename TypeWrapper>
class ModuleRegistryImpl final: public ModuleRegistry {
public:
  void setDynamicImportCallback(kj::Function<DynamicImportCallback> func) override {
    dynamicImportHandler = kj::mv(func);
  }

  void add(kj::Path& specifier, ModuleInfo&& info) {
    entries.insert(Entry(specifier, Type::BUNDLE, kj::fwd<ModuleInfo>(info)));
  }

  void addBuiltinModule(kj::StringPtr specifier,
                        kj::ArrayPtr<const char> sourceCode,
                        Type type = Type::BUILTIN) {
    // Register new module accessible by a given importPath. The module is instantiated
    // after first resolve attempt within application has failed, i.e. it is possible for
    // application to override the module.
    // sourceCode has to exist while this ModuleRegistry exists.
    // The expectation is for this method to be called during the assembly of worker global context
    // after registering all user modules.
    KJ_ASSERT(type != Type::BUNDLE);
    using Key = typename Entry::Key;

    // We need to make sure there is not an existing worker bundle module with the same
    // name if type == Type::BUILTIN
    auto path = kj::Path::parse(specifier);
    if (type == Type::BUILTIN && entries.find(Key(path, Type::BUNDLE)) != nullptr) {
      return;
    }

    entries.insert(Entry(path, type, sourceCode));
  }

  void addBuiltinModule(kj::StringPtr specifier,
                        kj::Function<ModuleInfo(Lock&)> factory,
                        Type type = Type::BUILTIN) {
    KJ_ASSERT(type != Type::BUNDLE);
    using Key = typename Entry::Key;

    auto path = kj::Path::parse(specifier);

    // We need to make sure there is not an existing worker bundle module with the same
    // name if type == Type::BUILTIN
    if (type == Type::BUILTIN && entries.find(Key(path, Type::BUNDLE)) != nullptr) {
      return;
    }

    entries.insert(Entry(path, type, kj::mv(factory)));
  }

  template <typename T>
  void addBuiltinModule(kj::StringPtr specifier, Type type = Type::BUILTIN) {
    addBuiltinModule(specifier, [specifier=kj::str(specifier)](Lock& js) {
      auto& wrapper = TypeWrapper::from(js.v8Isolate);
      auto wrap = wrapper.wrap(js.v8Isolate->GetCurrentContext(), nullptr, alloc<T>());
      return ModuleInfo(js, specifier, nullptr, ObjectModuleInfo(js, wrap));
    }, type);
  }

  kj::Maybe<ModuleInfo&> resolve(jsg::Lock& js,
                                 const kj::Path& specifier,
                                 bool internalOnly = false) override {
    using Key = typename Entry::Key;
    if (internalOnly) {
      KJ_IF_MAYBE(entry, entries.find(Key(specifier, Type::INTERNAL))) {
        return entry->module(js);
      }
    } else {
      // First, we try to resolve a worker bundle version of the module.
      KJ_IF_MAYBE(entry, entries.find(Key(specifier, Type::BUNDLE))) {
        return entry->module(js);
      }
      // Then we look for a built-in version of the module.
      KJ_IF_MAYBE(entry, entries.find(Key(specifier, Type::BUILTIN))) {
        return entry->module(js);
      }
    }
    return nullptr;
  }

  kj::Maybe<ModuleRef> resolve(jsg::Lock& js, v8::Local<v8::Module> module) override {
    for (const Entry& entry : entries) {
      // Unfortunately we cannot use entries.find(...) in here because the module info can
      // be initialized lazily at any point after the entry is indexed, making the lookup
      // by module a bit problematic. Iterating through the entries is slower but it works.
      KJ_IF_MAYBE(info, entry.info.template tryGet<ModuleInfo>()) {
        if (info->hashCode() == module->GetIdentityHash()) {
          return ModuleRef {
            .specifier = entry.specifier,
            .type = entry.type,
            .module = const_cast<ModuleInfo&>(*info),
          };
        }
      }
    }
    return nullptr;
  }

  size_t size() const { return entries.size(); }

  Promise<Value> resolveDynamicImport(jsg::Lock& js,
                                      const kj::Path& specifier,
                                      const kj::Path& referrer) override {
    // Here, we first need to determine if the referrer is a built-in module
    // or not. If it is a built-in, then we are only permitted to resolve
    // internal modules. If the worker bundle provided an override for the
    // built-in module, then the built-in was never registered and won't
    // be found.
    using Key = typename Entry::Key;
    bool internalOnly = false;
    KJ_IF_MAYBE(entry, entries.find(Key(referrer, Type::BUILTIN))) {
      internalOnly = true;
    }

    KJ_IF_MAYBE(info, resolve(js, specifier, internalOnly)) {
      KJ_IF_MAYBE(func, dynamicImportHandler) {
        auto handler = [&info = *info, isolate = js.v8Isolate]() -> Value {
          auto module = info.module.getHandle(isolate);
          auto& js = Lock::from(isolate);
          instantiateModule(js, module);
          return Value(isolate, module->GetModuleNamespace());
        };
        return (*func)(js, kj::mv(handler));
      }

      // If there is no dynamicImportHandler set, then we are going to handle that as if
      // the module does not exist and fall through to the rejected promise below.
    }

    return rejectedPromise<Value>(js.v8Isolate,
        v8::Exception::Error(v8Str(js.v8Isolate,
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
    using Info = kj::OneOf<ModuleInfo,
                           kj::ArrayPtr<const char>,
                           kj::Function<ModuleInfo(Lock&)>>;

    struct Key {
      const kj::Path& specifier;
      const Type type = Type::BUNDLE;
      uint hash;

      Key(const kj::Path& specifier, Type type)
          : specifier(specifier),
            type(type),
            hash(kj::hashCode(specifier, type)) {}

      uint hashCode() const { return hash; }
    };

    kj::Path specifier;
    Type type;
    Info info;
    // Either instantiated module or module source code.

    Entry(const kj::Path& specifier, Type type, ModuleInfo info)
        : specifier(specifier.clone()),
          type(type),
          info(kj::mv(info)) {}

    Entry(const kj::Path& specifier,Type type, kj::ArrayPtr<const char> src)
        : specifier(specifier.clone()),
          type(type),
          info(src) {}

    Entry(const kj::Path& specifier, Type type, kj::Function<ModuleInfo(Lock&)> factory)
        : specifier(specifier.clone()),
          type(type),
          info(kj::mv(factory)) {}

    Entry(Entry&&) = default;
    Entry& operator=(Entry&&) = default;

    ModuleInfo& module(jsg::Lock& js) {
      // Lazily instantiate module from source code if needed

      KJ_SWITCH_ONEOF(info) {
        KJ_CASE_ONEOF(moduleInfo, ModuleInfo) {
          return moduleInfo;
        }
        KJ_CASE_ONEOF(src, kj::ArrayPtr<const char>) {
          info = ModuleInfo(js, specifier.toString(), src, ModuleInfoCompileOption::BUILTIN);
          return KJ_ASSERT_NONNULL(info.tryGet<ModuleInfo>());
        }
        KJ_CASE_ONEOF(src, kj::Function<ModuleInfo(Lock&)>) {
          info = src(js);
          return KJ_ASSERT_NONNULL(info.tryGet<ModuleInfo>());
        }
      }
      KJ_UNREACHABLE;
    }
  };

  struct SpecifierHashCallbacks {
    using Key = typename Entry::Key;

    const Key keyForRow(const Entry& row) const { return Key(row.specifier, row.type); }

    bool matches(const Entry& row, Key key) const {
      return row.specifier == key.specifier && row.type == key.type;
    }

    uint hashCode(Key key) const {
      return key.hashCode();
    }
  };

  kj::Table<Entry, kj::HashIndex<SpecifierHashCallbacks>> entries;
};

template <typename TypeWrapper>
v8::MaybeLocal<v8::Promise> dynamicImportCallback(v8::Local<v8::Context> context,
                                                  v8::Local<v8::Data> host_defined_options,
                                                  v8::Local<v8::Value> resource_name,
                                                  v8::Local<v8::String> specifier,
                                                  v8::Local<v8::FixedArray> import_assertions) {
  auto isolate = context->GetIsolate();
  auto& lock = Lock::from(isolate);
  auto registry = ModuleRegistry::from(lock);
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
    auto& js = jsg::Lock::from(isolate);
    auto referrerPath = kj::Path::parse(kj::str(resource_name));
    auto specifierPath = referrerPath.parent().eval(kj::str(specifier));
    // TODO(soon): If kj::Path::parse fails it is most likely the application's fault and yet
    // we end up throwing an "internal error" here. We could handle this more gracefully
    // (and correctly) if kj::Path had a tryEval() variant.
    return wrapper.wrap(context, nullptr,
        registry->resolveDynamicImport(js, specifierPath, referrerPath));
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
void setModulesForResolveCallback(jsg::Lock& js, ModuleRegistry* table) {
  KJ_ASSERT(table != nullptr);
  js.v8Isolate->GetCurrentContext()->SetAlignedPointerInEmbedderData(2, table);
  js.v8Isolate->SetHostImportModuleDynamicallyCallback(dynamicImportCallback<TypeWrapper>);
}

}  // namespace workerd::jsg
