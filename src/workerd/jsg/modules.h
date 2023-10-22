// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "jsg.h"
#include <workerd/jsg/modules.capnp.h>
#include <workerd/jsg/observer.h>
#include <capnp/schema.h>
#include <kj/mutex.h>
#include <kj/map.h>
#include <vector>

namespace workerd::jsg::modules {

// Defines and implements workerd's module loader subsystem.

// Every Worker has a ModuleRegistry associated with it.
// The ModuleRegistry is the collection of individual Modules that can be
// imported (i.e. `import * from '...'` and `await import('...')`) or required
// (i.e. `require('...')`).
//
// The ModuleRegistry is immutable once created. When the Worker::Isolate is
// created, it's ModuleRegistry is created to contain Module definitions
// declared by the worker bundle configuration and from within the runtime.
// Internally, the ModuleBundle, ModuleBundleBuilder, and ModuleRegistry
// classes are used to create the ModuleRegistry instance.
//
// When a v8::Isolate* is created for a particular worker, the ModuleRegistry
// will be bound to the isolate in the form of a new IsolateModuleRegistry
// instance. This is the interface that is actually used to resolved
// specifiers into modules.
//
// The relationship between ModuleRegistry and IsolateModuleRegistry is such
// that a ModuleRegistry can actually be shared across multiple isolates/contexts
// safely, while the IsolateModuleRegistry is bound to an individual isolate/context
// pair. The ModuleRegistry, ModuleBundle, and individual Module instances do
// not store any state that is specific to an isolate.
//
// Specifiers are always handled as URLs.
// Built-in modules are always identified using a prefixed-specifier that
// can be parsed as an absolute URL. For instance, `node:buffer` is a fully
// qualified, absolute URL whose protocol is `node:` and whose pathname is
// `buffer`
// Specifieers for modules that come from the worker bundle config are always
// relative to `file:///`.
//
// For ESM modules, the specifier is accessible using import.meta.url.
// All ESM modules support import.meta.resolve(...)
// The first module in the module worker bundle must be an ESM and will specify
// import.meta.main = true.
//
// All modules are evaluated lazily when they are first imported. That is,
// the ModuleRegistry will not actually generate the v8::Local<v8::Module>,
// compile scripts, or evaluate it until the module is actually imported.
// This means that any modules that are never actually imported by a worker
// will never actually be compiled or evaluated.
//
// The ModuleRegistry evaluates modules synchronously and all modules are
// evaluated outside of the current IoContext (if any). This means the
// evaluation of any module cannot perform any i/o and therefore is expected
// to resolve synchronously. This allows for both ESM and CommonJS style
// imports/requires.

constexpr auto DEFAULT_STR = "default"_kjc;
constexpr auto MODULE_NOT_FOUND = "Failed to evaluate module. Not found."_kjc;
constexpr auto FILE_ROOT = "file:///"_kjc;

v8::Local<v8::WasmModuleObject> compileWasmModule(Lock& js, kj::ArrayPtr<const uint8_t> code,
                                                  const CompilationObserver& observer);

class Module;

// ======================================================================================
class CommonJsModuleObject final: public Object {
public:
  CommonJsModuleObject(Lock& js);

  JsValue getExports(Lock& js);
  void setExports(Lock& js, JsValue value);

  JSG_RESOURCE_TYPE(CommonJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
  }
private:
  JsRef<JsValue> exports;
};

class CommonJsModuleContext final: public Object {
public:
  CommonJsModuleContext(Lock& js, Module& module);

  JsValue require(Lock& js, kj::String specifier);
  Ref<CommonJsModuleObject> getModule(Lock& js);
  JsValue getExports(Lock& js);
  void setExports(Lock& js, JsValue value);

  JSG_RESOURCE_TYPE(CommonJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
  }

private:
  Module& inner;
  Ref<CommonJsModuleObject> module;
};

// ======================================================================================
// TODO(cleanup): Ideally these would exist over with the rest of the Node.js
// compat related stuff in workerd/api/node but there's a dependency cycle issue
// to work through there. Specifically, these are needed in jsg but jsg cannot
// depend on workerd/api. We should revisit to see if we can get these moved over.

// The NodeJsModuleContext is used in support of the NodeJsCompatModule type.
// It adds additional extensions to the global context that would normally be
// expected within the global scope of a Node.js compatible module (such as
// Buffer and process).

class NodeJsModuleObject final: public Object {
public:
  NodeJsModuleObject(Lock& js, Module& inner);

  JsValue getExports(Lock& js);
  void setExports(Lock& js, JsValue value);
  kj::ArrayPtr<const char> getPath();

  // TODO(soon): Additional properties... We can likely get by without implementing most
  // of these (if any).
  // * children https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulechildren
  // * filename https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulefilename
  // * id https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleid
  // * isPreloading https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleispreloading
  // * loaded https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleloaded
  // * parent https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#moduleparent
  // * paths https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulepaths
  // * require https://nodejs.org/dist/latest-v20.x/docs/api/modules.html#modulerequireid

  JSG_RESOURCE_TYPE(NodeJsModuleObject) {
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_READONLY_INSTANCE_PROPERTY(path, getPath);
  }
private:
  Module& inner;
  JsRef<JsValue> exports;
};

// The NodeJsModuleContext is similar in structure to CommonJsModuleContext
// with the exception that:
// (a) Node.js-compat built-in modules can be required without the `node:` specifier-prefix
//     (meaning that worker-bundle modules whose names conflict with the Node.js built-ins
//     are ignored), and
// (b) The common Node.js globals that we implement are exposed. For instance, `process`
//     and `Buffer` will be found at the global scope.
class NodeJsModuleContext final: public Object {
public:
  NodeJsModuleContext(Lock& js, Module& module);

  JsValue require(Lock& js, kj::String specifier);
  JsValue getBuffer(Lock& js);
  JsValue getProcess(Lock& js);

  // TODO(soon): Implement setImmediate/clearImmediate

  Ref<NodeJsModuleObject> getModule(Lock& js);

  JsValue getExports(Lock& js);
  void setExports(Lock& js, JsValue value);

  kj::String getFilename();
  kj::String getDirname();

  JSG_RESOURCE_TYPE(NodeJsModuleContext) {
    JSG_METHOD(require);
    JSG_READONLY_INSTANCE_PROPERTY(module, getModule);
    JSG_INSTANCE_PROPERTY(exports, getExports, setExports);
    JSG_LAZY_INSTANCE_PROPERTY(Buffer, getBuffer);
    JSG_LAZY_INSTANCE_PROPERTY(process, getProcess);
    JSG_LAZY_INSTANCE_PROPERTY(__filename, getFilename);
    JSG_LAZY_INSTANCE_PROPERTY(__dirname, getDirname);
  }

private:
  Module& inner;
  Ref<NodeJsModuleObject> module;
  JsRef<JsValue> exports;
};

// ======================================================================================
// NonModuleScript wraps a v8::UnboundScript.

// The NonModuleScript is used solely for old-style "service worker syntax" workers and
// is not actually a part of the ModuleRegistry.
class NonModuleScript final {
public:
  NonModuleScript(NonModuleScript&&) = default;
  NonModuleScript& operator=(NonModuleScript&&) = default;

  // Running the script will create a v8::Script instance bound to the given
  // context then will run it to completion.
  void run(v8::Local<v8::Context> context) const;

  static NonModuleScript compile(Lock& js, kj::StringPtr code, kj::StringPtr name = "worker.js");

private:
  NonModuleScript(Lock& js, v8::Local<v8::UnboundScript> script)
      : unboundScript(js.v8Isolate, script) {}

  v8::Global<v8::UnboundScript> unboundScript;
};

// ======================================================================================
// Module, ModuleBundle, ModuleRegistry

// Specifies a module that can be imported/required by a worker.
class Module {
public:
  // Identifies the general category of module.
  // An INTERNAL type can only be imported/required by other INTERNAL or BUILTIN types,
  // and can only import INTERNAL modules.
  // A BUILTIN type can be imported by any module and can only import other BUILTIN or
  // INTERNAL types.
  // A BUNDLE type can be imported by any module and can only import BUILTIN or other
  // BUNDLE types.
  using Type = workerd::jsg::ModuleType;

  enum class Flags {
    NONE = 0,
    // A Module with the MAIN flag set would specify import.meta.main = true.
    // This is generally only suitable for worker-bundle entry point modules,
    // but could in theory be applied to any module.
    MAIN = 1 << 0,
    ESM = 1 << 1,
  };

  KJ_DISALLOW_COPY_AND_MOVE(Module);

  inline Type type() const { return type_; }
  inline Flags flags() const { return flags_; }
  inline const Url& specifier() const KJ_LIFETIMEBOUND { return specifier_; }

  // True only if the ESM flag is set.
  inline bool isEsm() const;

  // Returns a v8::Module representing this Module definition for the given isolate.
  virtual v8::Local<v8::Module> getDescriptor(Lock& js) = 0;

  virtual bool load(Lock& js, v8::Local<v8::Module> module) KJ_WARN_UNUSED_RESULT = 0;

  static void instantiate(Lock& js, v8::Local<v8::Module> module);
  static JsObject evaluate(Lock& js, v8::Local<v8::Module> module);

protected:
  inline explicit Module(Type type, Url specifier, Flags flags = Flags::NONE)
      : type_(type), specifier_(kj::mv(specifier)), flags_(flags) {}

  // We use an std::vector here because we're preparing the list for v8 and it
  // expects an std::vector.
  virtual void listExports(Lock& js, std::vector<v8::Local<v8::String>>& exports) {}

private:
  Type type_;
  Url specifier_;
  Flags flags_;
};

// A SyntheticModule is effectively any module that is not internally an ESM.
// These will have their exports explicitly defined. We use this, for instance,
// for any custom module type like CommonJs, NodeJs, Text, Data, Wasm, and Capnp.
class SyntheticModule : public Module {
public:
  using Module::Module;

  bool load(Lock& js, v8::Local<v8::Module> module) override;
  v8::Local<v8::Module> getDescriptor(Lock& js) override;

  virtual JsValue getValue(Lock& js) = 0;
};

inline constexpr Module::Flags operator|(Module::Flags a, Module::Flags b) {
  return static_cast<Module::Flags>(static_cast<int>(a) | static_cast<int>(b));
}
inline constexpr Module::Flags operator&(Module::Flags a, Module::Flags b) {
  return static_cast<Module::Flags>(static_cast<int>(a) & static_cast<int>(b));
}
inline constexpr Module::Flags operator~(Module::Flags a) {
  return static_cast<Module::Flags>(~static_cast<int>(a));
}

inline bool Module::isEsm() const {
  return (flags() & Flags::ESM) == Flags::ESM;
}

// A ModuleRegistry is a collection of individual ModuleBundles, which are collections
// of related Modules. The ModuleBundle is the core of the implementation of the module
// registry in that it is the specific implementation of the ModuleBundle that determines
// exactly how a Module is resolved. The only requirement is that resolution is always
// synchronous. Later we might introduce asynchronous evaluation of modules to support a
// broader set of use cases. In such cases, the synchronous CommonJS `require()` might
// not work, however, so we'll have to handle it carefully.
class ModuleBundle {
public:
  using Type = workerd::jsg::ModuleType;

  inline Type type() const { return type_; }

  virtual kj::Maybe<Module&> resolve(const Url& specifier)
      KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT = 0;

protected:
  inline explicit ModuleBundle(Type type) : type_(type) {}

private:
  Type type_;
};

using DynamicImportHandler = Function<Promise<Value>(Function<Value()>)>;

class IsolateModuleRegistry;

// The ModuleRegistry is initialized for a Worker. It holds the base definitions
// for all modules available to the worker.
class ModuleRegistry final {
public:
  inline ModuleRegistry(kj::Array<kj::Own<ModuleBundle>> bundles)
      : bundles(kj::mv(bundles)) {}
  KJ_DISALLOW_COPY_AND_MOVE(ModuleRegistry);

  enum class ResolveOption {
    // Default resolution. Check worker bundle's first, then built-ins, ignore internal bundles.
    DEFAULT,
    // Built-in resolution. Ignore worker and internal bundles.
    BUILTIN,
    // Internal resolution. Check only internal builtins
    INTERNAL_ONLY,
  };

  // Attaches the ModuleRegistry to the Isolate by creating a new IsolateModuleRegistry
  // and associating that with the current isolate Context.
  inline void attachToIsolate(Lock& js, DynamicImportHandler dynamicImportHandler) const;

private:
  kj::Array<kj::Own<ModuleBundle>> bundles;

  // Uses the configured bundles to resolve the module associated with the given
  // specifier. Bundles that do not match the option specifier are ignored.
  kj::Maybe<Module&> resolve(const Url& specifier,
                             ResolveOption option = ResolveOption::DEFAULT)
                             KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;

  friend class IsolateModuleRegistry;
};

// Wraps the ModuleRegistry for a specific isolate context.
class IsolateModuleRegistry final {
public:
  static IsolateModuleRegistry& from(v8::Isolate* isolate);

  IsolateModuleRegistry(const ModuleRegistry& inner,
                        v8::Isolate* isolate,
                        kj::Maybe<DynamicImportHandler> dynamicImportHandler);
  KJ_DISALLOW_COPY_AND_MOVE(IsolateModuleRegistry);

  kj::Maybe<v8::Local<v8::Module>> resolve(
      Lock& js,
      const Url& specifier,
      ModuleRegistry::ResolveOption option = ModuleRegistry::ResolveOption::DEFAULT)
          KJ_WARN_UNUSED_RESULT;

  kj::Maybe<Module&> resolve(Lock& js, v8::Local<v8::Module> module)
                             KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
private:
  const ModuleRegistry& inner;
  kj::Maybe<DynamicImportHandler> dynamicImportHandler;

  struct ModuleRef {
    Module* ref;

    inline ModuleRef(Module& module) : ref(&module) {}
    ModuleRef(ModuleRef&&) = default;
    ModuleRef& operator=(ModuleRef&&) = default;
    KJ_DISALLOW_COPY(ModuleRef);

    inline operator Module&() { return *ref; }
    inline operator const Module&() const { return *ref; }
  };

  struct Cached {
    V8Ref<v8::Module> ref;
    bool internal;
    Cached(Lock& js, v8::Local<v8::Module> module, bool internal);
  };

  kj::HashMap<Url, Cached> cache;
  kj::HashMap<HashableV8Ref<v8::Module>, ModuleRef> modules;

  static v8::MaybeLocal<v8::Promise> dynamicImport(
      v8::Local<v8::Context> context,
      v8::Local<v8::Data> host_defined_options,
      v8::Local<v8::Value> resource_name,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> import_assertions);

  static void importMeta(v8::Local<v8::Context> context,
                         v8::Local<v8::Module> module,
                         v8::Local<v8::Object> meta);
};

inline void ModuleRegistry::attachToIsolate(
    Lock& js, DynamicImportHandler dynamicImportHandler) const {
  // We use new here because it is required for the v8 integration.
  // Responsibility for cleaning up the type is passed to v8 when
  // the context is cleaned up.
  new IsolateModuleRegistry(*this, js.v8Isolate, kj::mv(dynamicImportHandler));
}

// A synthetic Module that is backed by a jsg::Object exported as its default export.
// These are always exclusively built-in/internal modules.
template <typename T, typename TypeWrapper>
class JsgObjectModule final: public SyntheticModule {
public:

  // TODO(soon): A JsgObjectModule currently does not support named exports other than
  // "default". We actually could support named exports here, hwoever, by allowing the
  // type T to explicitly define which methods to handle as named exports.

  inline explicit JsgObjectModule(Type type,
                                  Url specifier,
                                  Flags flags = Flags::NONE)
      : SyntheticModule(type, kj::mv(specifier), flags) {}
  using SyntheticModule::SyntheticModule;

  JsValue getValue(Lock& js) override {
    auto& wrapper = TypeWrapper::from(js.v8Isolate);
    return JsValue(wrapper.wrap(js.v8Context(), kj::none, alloc<T>()));
  }
};

// A synthetic Module that is backed by a CommonJS style script.
// These are always exclusively from the worker bundle configuration.
template <typename TypeWrapper>
class CjsModule final: public SyntheticModule {
public:
  enum class Mode {
    // The original CommonJsModule type.
    DEFAULT,
    // The NodeJsCompatModule type.
    NODEJS_COMPAT,
  };

  // TODO(soon): A CjsModule currently does not support named exports other than "default".
  // We actually could support named exports here, however, by moving the compile/run step
  // into the getDescriptor() method and extracting the list of own properties on the export.
  // We would then implement a custom override of Module::load here to set the named exports
  // explicitly from that object.

  CjsModule(Type type, Url url, kj::String code, Flags flags, Mode mode)
      : SyntheticModule(type, kj::mv(url), flags),
        code(kj::mv(code)),
        mode(mode) {}

  JsValue getValue(Lock& js) override {
    auto& wrapper = TypeWrapper::from(js.v8Isolate);
    v8::Local<v8::Object> contextHandle;

    kj::OneOf<Ref<CommonJsModuleContext>, Ref<NodeJsModuleContext>> contextInstance;

    switch (mode) {
      case Mode::DEFAULT: {
        auto context = alloc<CommonJsModuleContext>(js, *this);
        contextHandle = wrapper.wrap(js.v8Context(), kj::none, context.addRef())
            .template As<v8::Object>();
        contextInstance = kj::mv(context);
        break;
      }
      case Mode::NODEJS_COMPAT: {
        auto context = alloc<NodeJsModuleContext>(js, *this);
        contextHandle = wrapper.wrap(js.v8Context(), kj::none, kj::mv(context))
          .template As<v8::Object>();
        contextInstance = kj::mv(context);
        break;
      }
    }

    v8::ScriptOrigin origin(js.v8Isolate, js.strExtern(specifier().getHref()));

    auto str = js.str(code);

    v8::ScriptCompiler::Source source(str, origin);
    auto compiled = check(v8::ScriptCompiler::CompileFunction(js.v8Context(), &source, 0,
                                                              nullptr, 1, &contextHandle));

    auto eval = KJ_ASSERT_NONNULL(wrapper.tryUnwrap(js.v8Context(), compiled,
                                                    (Function<void()>*)nullptr,
                                                    kj::none));
    eval(js);

    // CommonJS/Node.js modules can completely replace the exported object,
    // so we have to wait until after eval returns to grab the value that is
    // to be used as the default export.
    v8::Local<v8::Value> namespaceHandle;
    KJ_SWITCH_ONEOF(contextInstance) {
      KJ_CASE_ONEOF(cjs, Ref<CommonJsModuleContext>) {
        namespaceHandle = cjs->getExports(js);
      }
      KJ_CASE_ONEOF(njs, Ref<NodeJsModuleContext>) {
        namespaceHandle = njs->getExports(js);
      }
    }

    return JsValue(namespaceHandle);
  }

private:
  kj::String code;
  Mode mode;
};

struct CapnpModuleInfo {
  capnp::Schema schema;

  struct Interface {
    kj::String name;
    capnp::Schema schema;
  };
  kj::Array<Interface> interfaces;
};

// A synthetic Module backed by a capnp capability.
// These are always exclusively from the worker bundle configuration.
template <typename TypeWrapper>
class CapnpModule final: public SyntheticModule {
public:
  CapnpModule(Type type, Url url, CapnpModuleInfo info, Flags flags)
      : SyntheticModule(type, kj::mv(url), flags),
        info(kj::mv(info)) {}

  bool load(Lock& js, v8::Local<v8::Module> module) override {
    auto& wrapper = TypeWrapper::from(js.v8Isolate);
    check(module->SetSyntheticModuleExport(js.v8Isolate,
        js.strIntern(DEFAULT_STR),
        wrapper.wrap(js.v8Context(), kj::none, info.schema)));
    for (auto& interface : info.interfaces) {
      check(module->SetSyntheticModuleExport(js.v8Isolate,
          js.strIntern(interface.name),
          wrapper.wrap(js.v8Context(), kj::none, interface.schema)));
    }
    return true;
  }

  void listExports(Lock& js, std::vector<v8::Local<v8::String>>& exports) override {
    for (auto& interface : info.interfaces) {
      exports.push_back(js.strIntern(interface.name));
    }
  }

  JsValue getValue(Lock& js) override {
    // getValue is not called in this implementation since we overload
    // load directly.
    KJ_UNREACHABLE;
  }

private:
  CapnpModuleInfo info;
};

// The ModuleBundleBuilder is the abstract base used internally to initialize the
// content of the ModuleRegistry.
class ModuleBundleBuilder {
public:
  inline ModuleBundleBuilder(ModuleBundle::Type type, CompilationObserver& observer)
      : type(type), observer_(observer) {}
  virtual ~ModuleBundleBuilder() = default;
  KJ_DISALLOW_COPY_AND_MOVE(ModuleBundleBuilder);

  // Register a Module backed by a factory function. The factory will be
  // called lazily when the module is resolved.
  using Factory = kj::Function<kj::Own<Module>(const Url& specifier)>;
  virtual void add(kj::StringPtr specifier, Factory factory) = 0;

  kj::Own<ModuleBundle> finish();

  struct Entry {
    Url specifier;
    Factory factory;
  };

  inline CompilationObserver& observer() { return observer_; }

protected:
  kj::Vector<Entry> entries;
  ModuleBundle::Type type;
  CompilationObserver& observer_;
};

// Used exclusively by internals to construct the collections of built-in/internal
// modules sourced by the runtime itself.
class BuiltinModuleBundleBuilder final : public ModuleBundleBuilder {
public:
  inline BuiltinModuleBundleBuilder(ModuleBundle::Type type, CompilationObserver& observer)
      : ModuleBundleBuilder(type, observer) {
    KJ_DASSERT(type != ModuleBundle::Type::BUNDLE);
  }

  // Register a built-in Module backed by a const buffer.
  // The specifier must be a fully qualified URL string.
  void add (kj::StringPtr specifier, kj::ArrayPtr<const char> source);

  // Register all built-in Modules defined in the capnp Bundle::Reader if they
  // match the given filter.
  void add(Bundle::Reader bundle);

  // Register a built-in Module backed by a Object resource type.
  // The specifier must be a fully qualified URL string.
  template <typename T, typename TypeWrapper>
  void add(kj::StringPtr specifier) {
    // Do *not* capture this in the lambda below. The builder instance will be destroyed
    // by the time the lambda is called.
    add(specifier, [type=type](const Url& specifier) -> kj::Own<Module> {
      return kj::heap<JsgObjectModule<T, TypeWrapper>>(type, specifier.clone());
    });
  }

  void add(kj::StringPtr specifier, Factory factory) override;
};

// Used exclusively to build a ModuleBundle representing the content of a worker bundle
// configuration.
class WorkerModuleBundleBuilder final : public ModuleBundleBuilder {
public:
  WorkerModuleBundleBuilder(CompilationObserver& observer)
      : ModuleBundleBuilder(ModuleBundle::Type::BUNDLE, observer) {}
  KJ_DISALLOW_COPY_AND_MOVE(WorkerModuleBundleBuilder);

  // If the specifier is not an absolute URL, it will be parsed relative to
  // the default base URL file:/// that is assumed for all worker bundles.

  void addTextModule(kj::StringPtr specifier, kj::String data,
                     Module::Flags flags = Module::Flags::NONE);
  void addDataModule(kj::StringPtr specifier, kj::Array<const kj::byte> data,
                     Module::Flags flags = Module::Flags::NONE);
  void addWasmModule(kj::StringPtr specifier, kj::Array<const uint8_t> code,
                     Module::Flags flags = Module::Flags::NONE);
  void addJsonModule(kj::StringPtr specifier, kj::String data,
                     Module::Flags flags = Module::Flags::NONE);
  void addEsmModule(kj::StringPtr specifier, kj::String code,
                    Module::Flags flags = Module::Flags::NONE);

  template <typename TypeWrapper>
  void addCjsModule(kj::StringPtr specifier, kj::String code,
                    Module::Flags flags = Module::Flags::NONE) {
    add(specifier, [code=kj::mv(code), flags](const Url& specifier) mutable {
      return kj::heap<CjsModule<TypeWrapper>>(
          ModuleBundle::Type::BUNDLE,
          specifier.clone(), kj::mv(code), flags,
          CjsModule<TypeWrapper>::Mode::DEFAULT);
    });
  }

  template <typename TypeWrapper>
  void addNodejsModule(kj::StringPtr specifier, kj::String code,
                       Module::Flags flags = Module::Flags::NONE) {
    add(specifier, [code=kj::mv(code), flags](const Url& specifier) mutable {
      return kj::heap<CjsModule<TypeWrapper>>(
          ModuleBundle::Type::BUNDLE,
          specifier.clone(), kj::mv(code), flags,
          CjsModule<TypeWrapper>::Mode::NODEJS_COMPAT);
    });
  }

  template <typename TypeWrapper>
  void addCapnpModule(kj::StringPtr specifier, CapnpModuleInfo info,
                      Module::Flags flags = Module::Flags::NONE) {
    add(specifier, [info=kj::mv(info), flags](const Url& specifier) mutable {
      return kj::heap<CapnpModule<TypeWrapper>>(
          ModuleBundle::Type::BUNDLE,
          specifier.clone(),
          kj::mv(info), flags);
    });
  }

  void add(kj::StringPtr specifier, Factory factory) override;
};

}  // namespace workerd::jsg::modules
