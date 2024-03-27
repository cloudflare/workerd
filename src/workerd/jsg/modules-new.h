// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/observer.h>
#include <workerd/jsg/modules.capnp.h>
#include <workerd/jsg/url.h>
#include <workerd/jsg/util.h>
#include <kj/common.h>
#include <kj/function.h>
#include <kj/refcount.h>
#include <kj/map.h>
#include <kj/table.h>
#include <v8.h>

namespace workerd::jsg::modules {

// Defines and implements workerd's (new) module loader subsystem.
//
// Every Worker has exactly one ModuleRegistry associated with composed of
// of or more ModuleBundles (e.g. a ModuleBundle with modules from the worker
// bundle, a ModuleBundle representing built-in modules, a ModuleBundle that is
// a fallback service, etc)
//
// The ModuleRegistry is the collection of individual Modules that can be
// imported (i.e. `import * from '...'` and `await import('...')`) or required
// (i.e. `require('...')`).
//
// The ModuleRegistry is conceptually immutable once created (individual
// instances may support dynamic resolution using, for instance, a fallback
// service but there is no API to manipulate the ModuleRegistry once it has
// been created).
//
// When a Worker::Isolate is created, a ModuleBundle is created to contain
// Module definitions declared by the worker bundle configuration. One or more
// are also provided that provides modules from within the runtime.
//
// When a v8::Isolate* is created for a particular worker, the ModuleRegistry
// instance will be bound to the isolate in the form of a new IsolateModuleRegistry
// instance. This is the interface that is actually used to resolve specifiers into
// imported modules.
//
// The relationship between ModuleRegistry and IsolateModuleRegistry is such
// that a ModuleRegistry can be shared across multiple isolates/contexts safely,
// while the IsolateModuleRegistry is bound to an individual isolate/context
// pair. The ModuleRegistry, ModuleBundle, and individual Module instances do
// not store any state that is specific to an isolate.
//
// Specifiers are always handled as URLs.
//
// Built-in modules are always identified using a prefixed-specifier that
// can be parsed as an absolute URL. For instance, `node:buffer` is a fully
// qualified, absolute URL whose protocol is `node:` and whose pathname is
// `buffer`.
//
// Specifiers for modules that come from the worker bundle config are always
// relative to `file:///`.
//
// For ESM modules, the specifier is accessible using import.meta.url.
// All ESM modules support import.meta.resolve(...)
//
// If the first module in the module worker bundle is an ESM it will specify
// import.meta.main = true.
//
// All modules are evaluated lazily when they are first imported. That is,
// the ModuleRegistry will not actually generate the v8::Local<v8::Module>,
// compile scripts, or evaluate it until the module is actually imported.
// This means that any modules that are never actually imported by a worker
// will never actually be compiled or evaluated.
//
// A ModuleBundle will have one of three basic types: Bundle, Builtin,
// or Builtin-Only. A Bundle ModuleBundle provides access to modules
// that are defined in the worker bundle configuration. A Builtin ModuleBundle
// provides access to modules that are compiled into the runtime and are
// importable by bundle scripts. A Builtin-Only ModuleBundle provides access
// to built-in modules that can only be imported by other built-ins.
//
// A special fourth type of ModuleBundle that is available only for local
// dev in workerd is the Fallback type. This will use dynamic resolution
// using a configurable fallback service. This will not be available in
// production.
//
// ModuleBundle instances will also have a resolution priority. That is,
// when a ModuleRegistry is using multiple ModuleBundle instances, the order
// in which they are searched is significant. The search order will vary based
// on resolve context.
//
// * If import is called from a bundle script, the search order is:
//
//   1. Bundle ModuleBundle
//   2. Builtin ModuleBundle
//   3. Fallback ModuleBundle (if available)
//
// * If import is called from a builtin script, the search order is:
//
//   1. Builtin ModuleBundle
//   2. Builtin-Only ModuleBundle
//
// * If import is called from a builtin-only script, the search order is:
//
//   1. Builtin-Only ModuleBundle
//
// When the Fallback ModuleBundle is used, modules loaded from the fallback
// are handled as if they are bundle scripts. That key difference, however, is
// that the fallback service is not limited to a specific URL root.
//
// Notice that for built-ins, the Bundle ModuleBundle is not used. This
// means it will not be possible to import bundle modules from a built-in.
//
// The ModuleRegistry evaluates modules synchronously and all modules are
// evaluated outside of the current IoContext (if any). This means the
// evaluation of any module cannot perform any i/o and therefore is expected
// to resolve synchronously. This allows for both ESM and CommonJS style
// imports/requires. However, this also means that modules that do need to
// be resolved, loaded, and evaluated asynchronously must make appropriate
// arrangements to be able to do so.
//
// Module resolution occurs in two phases: Resolve and Instantiate.
// The Resolve phase is respondsible for determining if a module is available
// and does not require the isolate lock to be held. The Instantiate phase
// creates the v8::Module instance and does require the isolate lock to be
// held.
//
// All modules are Instantiated lazily. That is, the ModuleRegistry will not
// instantiate a module until after it is resolved (implying an import or
// require).
//
// Metrics can be collected for module resolution and evaluation.

// The ResolveContext identifies the module that is being resolved along with
// other key bits of information that may be used to resolve the module.
struct ResolveContext {
  using Source = ResolveObserver::Source;
  using Type = ResolveObserver::Context;

  Type type;
  Source source;
  const Url& specifier;
  const Url& referrer;

  // The raw specifier is the original specifier passed in, if any,
  // before it was normalized into the specifier URL.
  kj::Maybe<kj::StringPtr> rawSpecifier = kj::none;

  // TODO(soon): Support import attributes
  // kj::HashMap<kj::StringPtr, kj::StringPtr> attributes;
};

// The abstraction of a module within the ModuleRegistry.
// Importantly, a Module is immutable once created and must be thread-safe.
class Module {
public:
  enum class Type : uint8_t {
    BUNDLE,
    BUILTIN,
    BUILTIN_ONLY,
    FALLBACK,
  };

  enum class Flags: uint8_t {
    NONE = 0,
    // A Module with the MAIN flag set would specify import.meta.main = true.
    // This is generally only suitable for worker-bundle entry point modules,
    // but could in theory be applied to any module.
    MAIN = 1 << 0,
    ESM = 1 << 1,
  };

  KJ_DISALLOW_COPY_AND_MOVE(Module);
  virtual ~Module() = default;

  inline const Url& specifier() const KJ_LIFETIMEBOUND { return specifier_; }
  inline Type type() const { return type_; }

  // If isEsm() returns false the implication is that the module is a synthetic module
  bool isEsm() const;

  // If isMain() returns true, then import.meta.main will be true for this module
  bool isMain() const;

  // Returns a v8::Module representing this Module definition for the given isolate.
  // The return value follows the established v8 rules for Maybe. If the returned
  // maybe is empty, then an exception should have been scheduled on the isolate
  // via the lock. Do not throw C++ exceptions from this method unless they are fatal.
  virtual v8::MaybeLocal<v8::Module> getDescriptor(
      Lock& js,
      const CompilationObserver& observer) const KJ_WARN_UNUSED_RESULT = 0;

  // Determines if this module can be resolved in the given context.
  virtual bool evaluateContext(const ResolveContext& context) const
      KJ_WARN_UNUSED_RESULT;

  // Instantiates the given module. The return value follows the established v8
  // rules for Maybe. If the returned maybe is empty, then an exception should
  // have been scheduled on the isolate via the lock. Do not throw C++ exceptions
  // from this method unless they are fatal.
  v8::Maybe<bool> instantiate(
      Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer) const KJ_WARN_UNUSED_RESULT;

  // Evaluates the given module, returning the result of the evaluation in the form
  // of a JS value. This is the value that is actually returned by the import or
  // require. The return value follows the established v8 rules for Maybe. If the
  // returned maybe is empty, then an exception should have been scheduled on the
  // isolate via the lock. If the module has not yet been instantiated, it will
  // be instantiated first. Do not throw C++ exceptions from this method unless they
  // are fatal.
  virtual v8::MaybeLocal<v8::Value> evaluate(
      Lock& js,
      v8::Local<v8::Module> module,
      const CompilationObserver& observer) const KJ_WARN_UNUSED_RESULT = 0;

  // A helper interface that is used to make it easier for a synthetic module
  // evaluation callback to set the exports of the module.
  class ModuleNamespace final {
  public:
    explicit ModuleNamespace(v8::Local<v8::Module> inner,
                             kj::ArrayPtr<const kj::String> namedExports);
    KJ_DISALLOW_COPY_AND_MOVE(ModuleNamespace);

    bool set(Lock& js, kj::StringPtr name, JsValue value) const;
    bool setDefault(Lock& js, JsValue value) const;

  private:
    v8::Local<v8::Module> inner;
    kj::HashSet<kj::StringPtr> namedExports;
  };

  // The EvaluateCallback is used to evaluate a synthetic module. The callback
  // is called after the module is resolved and instantiated.
  using EvaluateCallback =
      kj::Function<bool(Lock&, const Url& specifier,
                        const ModuleNamespace&,
                        const CompilationObserver&)>;

  static kj::Own<Module> newSynthetic(
      Url specifier,
      Type type,
      EvaluateCallback callback,
      kj::Array<kj::String> namedExports = nullptr);

  // Creates a new ESM module that takes ownership of the given code array.
  // This is generally used to construct ESM modules from a worker bundle.
  static kj::Own<Module> newEsm(
      Url specifier,
      Type type,
      kj::Array<const char> code,
      Flags flags = Flags::ESM);

  // Creates a new ESM module that does not take ownership of the given code
  // array. This is used to construct ESM modules from compiled-in built-in
  // modules.
  static kj::Own<Module> newEsm(
      Url specifier,
      Type type,
      kj::ArrayPtr<const char> code);

  // The following methods are used to create the evaluation callbacks for various
  // kinds of common simple synthetic module types. The module registry is not
  // limited to just these kinds of modules, however. These are just the most
  // common.

  static EvaluateCallback newTextModuleHandler(kj::Array<const char> data)
      KJ_WARN_UNUSED_RESULT;
  static EvaluateCallback newDataModuleHandler(kj::Array<kj::byte> data)
      KJ_WARN_UNUSED_RESULT;
  static EvaluateCallback newJsonModuleHandler(kj::Array<const char> data)
      KJ_WARN_UNUSED_RESULT;
  static EvaluateCallback newWasmModuleHandler(kj::Array<kj::byte> data)
      KJ_WARN_UNUSED_RESULT;

  // An eval function is used for CommonJS style modules (including Node.js compat
  // modules. The expectation is that this method will be called when the CommonJS
  // style module is evaluated (e.g. within the EvaluationCallback).
  static Function<void()> compileEvalFunction(
      Lock& js,
      kj::StringPtr code,
      kj::StringPtr name,
      kj::Maybe<JsObject> compileExtensions,
      const CompilationObserver& observer) KJ_WARN_UNUSED_RESULT;

  // Some modules may need to protect against being evaluated recursively. The
  // EvaluatingScope class makes it possible to guard against that, throwing an
  // error if there's already an active evaluation happening.
  class EvaluatingScope {
  public:
    EvaluatingScope() = default;
    KJ_DISALLOW_COPY_AND_MOVE(EvaluatingScope);
    ~EvaluatingScope() noexcept(false);
    kj::Own<void> enterEvaluationScope(const Url& specifier) KJ_WARN_UNUSED_RESULT;
  private:
    struct Impl;
    KJ_DECLARE_NON_POLYMORPHIC(Impl);
    kj::Maybe<Impl&> maybeEvaluating;
    friend struct Impl;
  };

  // A CjsStyleModuleHandler is used for CommonJS style modules (including
  // The template type T must be a jsg::Object that implements a getExports(Lock&)
  // method returning a JsValue. This is set as the default export of the
  // synthetic module. All methods and properties exposed by the template
  // type T are exposed as additional globals within the executed scope.
  template <typename T, typename TypeWrapper>
  static EvaluateCallback newCjsStyleModuleHandler(kj::String source, kj::String name)
      KJ_WARN_UNUSED_RESULT {
    return [source=kj::mv(source), name=kj::mv(name),
            evaluatingScope=kj::heap<EvaluatingScope>()](
        Lock& js,
        const Url& specifier,
        const Module::ModuleNamespace& ns,
        const CompilationObserver& observer) mutable -> bool {
      return js.tryCatch([&] {
        auto evaluating = evaluatingScope->enterEvaluationScope(specifier);
        auto& wrapper = TypeWrapper::from(js.v8Isolate);
        auto ext = alloc<T>(js, specifier);
        auto fn = Module::compileEvalFunction(js, source, name,
            JsObject(wrapper.wrap(js.v8Context(), kj::none, ext.addRef())),
            observer);
        fn(js);
        return ns.setDefault(js, ext->getExports(js));
      }, [&](Value exception) {
        js.v8Isolate->ThrowException(exception.getHandle(js));
        return false;
      });
    };
  }

protected:
  Module(Url specifier, Type type, Flags flags = Flags::NONE);

private:
  Url specifier_;
  Type type_;
  Flags flags_;
};

constexpr Module::Flags operator&(const Module::Flags& a, const Module::Flags& b) {
  return static_cast<Module::Flags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr Module::Flags operator|(const Module::Flags& a, const Module::Flags& b) {
  return static_cast<Module::Flags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

// A ModuleBundle is a source of modules that can be imported or required.
// A ModuleRegistry is a collection of ModuleBundles.
// Importantly, a ModuleBundle is immutable once created with exception to
// any internal caching it may use to optimize resolution. Accesses to the
// bundle must be thread-safe.
class ModuleBundle {
public:
  using Type = Module::Type;

  // A Builder is used to construct a ModuleBundle.
  class Builder {
  public:
    KJ_DISALLOW_COPY_AND_MOVE(Builder);

    using ResolveCallback =
        kj::Function<kj::Maybe<kj::Own<Module>>(const ResolveContext&)>;

    Builder& add(const Url& specifier, ResolveCallback callback)
      KJ_LIFETIMEBOUND;

    Builder& alias(const Url& alias, const Url& specifier) KJ_LIFETIMEBOUND;

    kj::Own<ModuleBundle> finish() KJ_WARN_UNUSED_RESULT;

    inline Type type() const { return type_; }

  protected:
    Builder(Type type);

    void ensureIsNotBundleSpecifier(const Url& specifier);

    Type type_;
    kj::HashMap<Url, ResolveCallback> modules_;
    kj::HashMap<Url, Url> aliases_;
  };

  // Used to build a ModuleBundle representing modules sourced from a worker bundle.
  class BundleBuilder final: public Builder {
  public:
    static const Url BASE;

    BundleBuilder();
    KJ_DISALLOW_COPY_AND_MOVE(BundleBuilder);

    using EvaluateCallback = Module::EvaluateCallback;

    BundleBuilder& addSyntheticModule(
        kj::StringPtr specifier,
        EvaluateCallback callback,
        kj::Array<kj::String> namedExports = nullptr) KJ_LIFETIMEBOUND;

    BundleBuilder& addEsmModule(
        kj::StringPtr specifier,
        kj::Array<const char> code,
        Module::Flags flags = Module::Flags::ESM) KJ_LIFETIMEBOUND;

    BundleBuilder& alias(kj::StringPtr alias, kj::StringPtr specifier) KJ_LIFETIMEBOUND;
  };

  // Used to builde a ModuleBundle representing modules sources from the runtime.
  class BuiltinBuilder final: public Builder {
  public:
    enum class Type {
      BUILTIN,
      BUILTIN_ONLY,
    };
    BuiltinBuilder(Type type = Type::BUILTIN);
    KJ_DISALLOW_COPY_AND_MOVE(BuiltinBuilder);

    BuiltinBuilder& addSynthetic(
        const Url& specifier,
        BundleBuilder::EvaluateCallback callback) KJ_LIFETIMEBOUND;

    BuiltinBuilder& addEsm(const Url& specifier, kj::ArrayPtr<const char> source)
        KJ_LIFETIMEBOUND;

    // Adds a module that is implemented in C++ as a jsg::Object
    template <typename T, typename TypeWrapper>
    BuiltinBuilder& addObject(const Url& specifier) KJ_LIFETIMEBOUND {
      ensureIsNotBundleSpecifier(specifier);
      add(specifier, [specifier=specifier.clone(), type = type()](
          const ResolveContext& context) mutable -> kj::Maybe<kj::Own<Module>> {
        if (context.specifier != specifier) return kj::none;
        return Module::newSynthetic(kj::mv(specifier), type,
            [](Lock& js, const Url& specifier, const Module::ModuleNamespace& ns,
               const CompilationObserver&) {
          auto value = TypeWrapper::from(js.v8Isolate).wrap(js.v8Context(), kj::none,
              alloc<T>(js, specifier));
          ns.setDefault(js, JsValue(value));
          return true;
        });
      });
      return *this;
    }
  };

  static kj::Own<ModuleBundle> newFallbackBundle(Builder::ResolveCallback callback)
      KJ_WARN_UNUSED_RESULT;

  enum class BuiltInBundleOptions {
    NONE = 0,
    ALLOW_DATA_MODULES = 1 << 0,
  };

  static void getBuiltInBundleFromCapnp(
      BuiltinBuilder& builder,
      Bundle::Reader bundle,
      BuiltInBundleOptions options);

  KJ_DISALLOW_COPY_AND_MOVE(ModuleBundle);

  inline Type type() const { return type_; }

  virtual ~ModuleBundle() noexcept(false) = default;

  virtual kj::Maybe<Module&> resolve(const ResolveContext& context)
      KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT = 0;

protected:
  ModuleBundle(Type type);

private:
  Type type_;
};

constexpr ModuleBundle::BuiltInBundleOptions operator|(
    const ModuleBundle::BuiltInBundleOptions& a,
    const ModuleBundle::BuiltInBundleOptions& b) {
  return static_cast<ModuleBundle::BuiltInBundleOptions>(
      static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr ModuleBundle::BuiltInBundleOptions operator&(
    const ModuleBundle::BuiltInBundleOptions& a,
    const ModuleBundle::BuiltInBundleOptions& b) {
  return static_cast<ModuleBundle::BuiltInBundleOptions>(
      static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

// A ModuleRegistry is a collection of zero or more ModuleBundles.
// Importantly, the ModuleRegistry is immutable once created and
// must be thread-safe.
class ModuleRegistry final : public ModuleRegistryBase {
public:

  class Builder final {
  public:
    enum class Options {
      NONE = 0,
      // When set, allows the ModuleRegistry to use a fallback ModuleBundle to
      // dynamically resolve a module that cannot be resolved by any other
      // registered bundles. The fallback service is only used when using the
      // ResolveContext::Type::BUNDLE context and is always the last bundle
      // checked. The fallback service should only be used for local dev.
      ALLOW_FALLBACK = 1 << 0,
    };
    Builder(const ResolveObserver& observer, Options options = Options::NONE);
    KJ_DISALLOW_COPY_AND_MOVE(Builder);

    // A ModuleRegistry may have exactly one parent registry. When set, if this
    // registry cannot resolve a module, it will attempt to resolve the module
    // from the parent registry.
    Builder& setParent(ModuleRegistry& parent) KJ_LIFETIMEBOUND;

    Builder& add(kj::Own<ModuleBundle> bundle) KJ_LIFETIMEBOUND;

    kj::Own<ModuleRegistry> finish() KJ_WARN_UNUSED_RESULT;

  private:
    bool allowsFallback() const;

    // One slot for each of ModuleBundle::Type
    const ResolveObserver& observer;
    kj::Maybe<ModuleRegistry&> maybeParent;
    const Options options;
    kj::Vector<kj::Own<ModuleBundle>> bundles_[4];
    friend class ModuleRegistry;
  };

  kj::Maybe<Module&> resolve(const ResolveContext& context)
      KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;

  // Attaches the ModuleRegistry to the given isolate by creating an IsolateModuleRegistry
  // and linking that to the isolate.
  void attachToIsolate(Lock& js, const CompilationObserver& observer) override;

  // Synchronously resolve the specified module from the registry bound to the given lock.
  // This will throw a JsExceptionThrown exception if the module cannot be found or an
  // error occurs while the module is being evaluated. Modules resolved with this method
  // must be capable of fully evaluating within one drain of the microtask queue.
  static JsValue resolve(Lock& js, kj::StringPtr specifier,
      kj::StringPtr exportName = "default"_kjc,
      ResolveContext::Type type = ResolveContext::Type::BUNDLE,
      ResolveContext::Source source = ResolveContext::Source::INTERNAL,
      kj::Maybe<const Url&> maybeReferrer = kj::none);

  // The constructor is public because kj::heap requires is to be. Do not
  // use the constructor directly. Use the ModuleRegistry::Builder
  ModuleRegistry(ModuleRegistry::Builder* builder);
  KJ_DISALLOW_COPY_AND_MOVE(ModuleRegistry);

private:
  const ResolveObserver& observer;
  kj::Maybe<ModuleRegistry&> maybeParent;
  enum BundleIndices {
    kBundle,
    kBuiltin,
    kBuiltinOnly,
    kFallback,
    kBundleCount
  };
  // One slot for each of ModuleBundle::Type
  kj::Array<kj::Own<ModuleBundle>> bundles_[kBundleCount];
};

constexpr ModuleRegistry::Builder::Options operator|(
    const ModuleRegistry::Builder::Options& a,
    const ModuleRegistry::Builder::Options& b) {
  return static_cast<ModuleRegistry::Builder::Options>(
      static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr ModuleRegistry::Builder::Options operator&(
    const ModuleRegistry::Builder::Options& a,
    const ModuleRegistry::Builder::Options& b) {
  return static_cast<ModuleRegistry::Builder::Options>(
      static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

}  // namespace workerd::jsg::modules
