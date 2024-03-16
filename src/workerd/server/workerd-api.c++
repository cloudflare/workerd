// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-api.h"

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules.h>
#include <workerd/jsg/util.h>
#include <workerd/jsg/setup.h>
#include <workerd/api/actor.h>
#include <workerd/api/actor-state.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/cache.h>
#include <workerd/api/crypto-impl.h>
#include <workerd/api/encoding.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/modules.h>
#include <workerd/api/pyodide.h>
#include <workerd/api/queue.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/sql.h>
#include <workerd/api/r2.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/trace.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/node/node.h>
#include <workerd/io/promise-wrapper.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/use-perfetto-categories.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
#include <workerd/api/gpu/gpu.h>
#else
#define EW_WEBGPU_ISOLATE_TYPES_LIST
#endif

namespace workerd::server {

JSG_DECLARE_ISOLATE_TYPE(JsgWorkerdIsolate,
  // Declares the listing of host object types and structs that the jsg
  // automatic type mapping will understand. Each of the various
  // NNNN_ISOLATE_TYPES macros are defined in different header files
  // (e.g. GLOBAL_SCOPE_ISOLATE_TYPES is defined in api/global-scope.h).
  //
  // Global scope types are defined first just by convention, the rest
  // of the list is in alphabetical order for easier readability (the
  // actual order of the items is unimportant), followed by additional
  // types defined in worker.c++ or as part of jsg.
  //
  // When adding a new NNNN_ISOLATE_TYPES macro, remember to add it to
  // src/workerd/api/rtti.c++ too (and tools/api-encoder.c++ for the
  // time being), so it gets included in the TypeScript types.
  EW_GLOBAL_SCOPE_ISOLATE_TYPES,

  EW_ACTOR_ISOLATE_TYPES,
  EW_ACTOR_STATE_ISOLATE_TYPES,
  EW_ANALYTICS_ENGINE_ISOLATE_TYPES,
  EW_BASICS_ISOLATE_TYPES,
  EW_BLOB_ISOLATE_TYPES,
  EW_CACHE_ISOLATE_TYPES,
  EW_CRYPTO_ISOLATE_TYPES,
  EW_ENCODING_ISOLATE_TYPES,
  EW_FORMDATA_ISOLATE_TYPES,
  EW_HTML_REWRITER_ISOLATE_TYPES,
  EW_HTTP_ISOLATE_TYPES,
  EW_SOCKETS_ISOLATE_TYPES,
  EW_KV_ISOLATE_TYPES,
  EW_PYODIDE_ISOLATE_TYPES,
  EW_QUEUE_ISOLATE_TYPES,
  EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES,
  EW_R2_PUBLIC_BETA_ISOLATE_TYPES,
  EW_WORKER_RPC_ISOLATE_TYPES,
  EW_SCHEDULED_ISOLATE_TYPES,
  EW_STREAMS_ISOLATE_TYPES,
  EW_TRACE_ISOLATE_TYPES,
  EW_UNSAFE_ISOLATE_TYPES,
  EW_MEMORY_CACHE_ISOLATE_TYPES,
  EW_URL_ISOLATE_TYPES,
  EW_URL_STANDARD_ISOLATE_TYPES,
  EW_URLPATTERN_ISOLATE_TYPES,
  EW_WEBSOCKET_ISOLATE_TYPES,
  EW_SQL_ISOLATE_TYPES,
  EW_NODE_ISOLATE_TYPES,
  EW_RTTI_ISOLATE_TYPES,
  EW_HYPERDRIVE_ISOLATE_TYPES,
#ifdef WORKERD_EXPERIMENTAL_ENABLE_WEBGPU
  EW_WEBGPU_ISOLATE_TYPES,
#endif

  jsg::TypeWrapperExtension<PromiseWrapper>,
  jsg::InjectConfiguration<CompatibilityFlags::Reader>,
  Worker::Api::ErrorInterface,
  jsg::CommonJsModuleObject,
  jsg::CommonJsModuleContext,
  jsg::NodeJsModuleObject,
  jsg::NodeJsModuleContext);

struct WorkerdApi::Impl {
  kj::Own<CompatibilityFlags::Reader> features;
  JsgWorkerdIsolate jsgIsolate;
  api::MemoryCacheProvider& memoryCacheProvider;
  kj::Maybe<kj::Own<const kj::Directory>> pyodideCacheRoot;

  class Configuration {
  public:
    Configuration(Impl& impl)
        : features(*impl.features),
          jsgConfig(jsg::JsgConfig {
            .noSubstituteNull = features.getNoSubstituteNull(),
            .unwrapCustomThenables = features.getUnwrapCustomThenables(),
          }) {}
    operator const CompatibilityFlags::Reader() const { return features; }
    operator const jsg::JsgConfig&() const { return jsgConfig; }

  private:
    CompatibilityFlags::Reader& features;
    jsg::JsgConfig jsgConfig;
  };

  Impl(jsg::V8System& v8System,
       CompatibilityFlags::Reader featuresParam,
       IsolateLimitEnforcer& limitEnforcer,
       kj::Own<jsg::IsolateObserver> observer,
       api::MemoryCacheProvider& memoryCacheProvider,
       kj::Maybe<kj::Own<const kj::Directory>> pyodideCacheRoot)
      : features(capnp::clone(featuresParam)),
        jsgIsolate(v8System, Configuration(*this), kj::mv(observer), limitEnforcer.getCreateParams()),
        memoryCacheProvider(memoryCacheProvider), pyodideCacheRoot(kj::mv(pyodideCacheRoot)) {}

  static v8::Local<v8::String> compileTextGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Text::Reader reader) {
    return lock.wrapNoContext(reader);
  };

  static v8::Local<v8::ArrayBuffer> compileDataGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Data::Reader reader) {
    return lock.wrapNoContext(kj::heapArray(reader));
  };

  static v8::Local<v8::WasmModuleObject> compileWasmGlobal(
      JsgWorkerdIsolate::Lock& lock, capnp::Data::Reader reader,
      const jsg::CompilationObserver& observer) {
    lock.setAllowEval(true);
    KJ_DEFER(lock.setAllowEval(false));

    // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
    // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
    // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
    // compiles fast but runs slower.
    AllowV8BackgroundThreadsScope scope;

    return jsg::compileWasmModule(lock, reader, observer);
  };

  static v8::Local<v8::Value> compileJsonGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Text::Reader reader) {
    return jsg::check(v8::JSON::Parse(
        lock.v8Context(),
        lock.wrapNoContext(reader)));
  };

};

WorkerdApi::WorkerdApi(jsg::V8System& v8System,
    CompatibilityFlags::Reader features,
    IsolateLimitEnforcer& limitEnforcer,
    kj::Own<jsg::IsolateObserver> observer,
    api::MemoryCacheProvider& memoryCacheProvider,
    kj::Maybe<kj::Own<const kj::Directory>> pyodideCacheRoot)
    : impl(kj::heap<Impl>(v8System, features, limitEnforcer, kj::mv(observer),
                          memoryCacheProvider, kj::mv(pyodideCacheRoot))) {}
WorkerdApi::~WorkerdApi() noexcept(false) {}

kj::Own<jsg::Lock> WorkerdApi::lock(jsg::V8StackScope& stackScope) const {
  return kj::heap<JsgWorkerdIsolate::Lock>(impl->jsgIsolate, stackScope);
}
CompatibilityFlags::Reader WorkerdApi::getFeatureFlags() const {
  return *impl->features;
}
jsg::JsContext<api::ServiceWorkerGlobalScope>
    WorkerdApi::newContext(jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock)
      .newContext<api::ServiceWorkerGlobalScope>(lock.v8Isolate);
}
jsg::Dict<NamedExport> WorkerdApi::unwrapExports(
    jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock)
      .unwrap<jsg::Dict<NamedExport>>(lock.v8Context(), moduleNamespace);
}
WorkerdApi::EntrypointClasses WorkerdApi::getEntrypointClasses(jsg::Lock& lock) const {
  auto& typedLock = kj::downcast<JsgWorkerdIsolate::Lock>(lock);

  return {
    .workerEntrypoint = typedLock.getConstructor<api::WorkerEntrypoint>(lock.v8Context()),
    .durableObject = typedLock.getConstructor<api::DurableObjectBase>(lock.v8Context()),
  };
}
const jsg::TypeHandler<Worker::Api::ErrorInterface>&
    WorkerdApi::getErrorInterfaceTypeHandler(jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<ErrorInterface>();
}

const jsg::TypeHandler<api::QueueExportedHandler>&
    WorkerdApi::getQueueTypeHandler(jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<api::QueueExportedHandler>();
}

jsg::JsObject WorkerdApi::wrapExecutionContext(
    jsg::Lock& lock, jsg::Ref<api::ExecutionContext> ref) const {
  return jsg::JsObject(kj::downcast<JsgWorkerdIsolate::Lock>(lock)
      .wrap(lock.v8Context(), kj::mv(ref)));
}

Worker::Script::Source WorkerdApi::extractSource(kj::StringPtr name,
    config::Worker::Reader conf,
    Worker::ValidationErrorReporter& errorReporter,
    capnp::List<config::Extension>::Reader extensions) {
  TRACE_EVENT("workerd", "WorkerdApi::extractSource()");
  switch (conf.which()) {
    case config::Worker::MODULES: {
      auto modules = conf.getModules();
      if (modules.size() == 0) {
        errorReporter.addError(kj::str("Modules list cannot be empty."));
        goto invalid;
      }

      bool isPython = api::pyodide::hasPythonModules(modules);
      return Worker::Script::ModulesSource {
        modules[0].getName(),
        [conf,&errorReporter, extensions](jsg::Lock& lock, const Worker::Api& api) {
          return WorkerdApi::from(api).compileModules(lock, conf, errorReporter, extensions);
        },
        isPython
      };
    }
    case config::Worker::SERVICE_WORKER_SCRIPT:
      return Worker::Script::ScriptSource {
        conf.getServiceWorkerScript(),
        name,
        [conf,&errorReporter](jsg::Lock& lock, const Worker::Api& api, const jsg::CompilationObserver& observer) {
          return WorkerdApi::from(api).compileScriptGlobals(lock, conf, errorReporter, observer);
        }
      };
    case config::Worker::INHERIT:
      // TODO(beta): Support inherit.
      KJ_FAIL_ASSERT("inherit should have been handled earlier");
  }

  errorReporter.addError(kj::str("Encountered unknown Worker code type. Was the "
                                 "config compiled with a newer version of the schema?"));
invalid:
  return Worker::Script::ScriptSource {
    ""_kj,
    name,
    [](jsg::Lock& lock, const Worker::Api& api, const jsg::CompilationObserver& observer)
        -> kj::Array<Worker::Script::CompiledGlobal> {
      return nullptr;
    }
  };
}

kj::Array<Worker::Script::CompiledGlobal> WorkerdApi::compileScriptGlobals(
      jsg::Lock& lockParam, config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter,
      const jsg::CompilationObserver& observer) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileScriptGlobals()");
  // For Service Worker scripts, we support Wasm modules as globals, but they need to be loaded
  // at script load time.

  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);

  uint wasmCount = 0;
  for (auto binding: conf.getBindings()) {
    if (binding.isWasmModule()) ++wasmCount;
  }

  auto compiledGlobals = kj::heapArrayBuilder<Worker::Script::CompiledGlobal>(wasmCount);
  for (auto binding: conf.getBindings()) {
    if (binding.isWasmModule()) {
      auto name = lock.str(binding.getName());
      auto value = Impl::compileWasmGlobal(lock, binding.getWasmModule(), observer);

      compiledGlobals.add(Worker::Script::CompiledGlobal {
        { lock.v8Isolate, name },
        { lock.v8Isolate, value },
      });
    }
  }

  return compiledGlobals.finish();
}

kj::Maybe<jsg::ModuleRegistry::ModuleInfo> WorkerdApi::tryCompileModule(
    jsg::Lock& js,
    config::Worker::Module::Reader module,
    jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  TRACE_EVENT("workerd", "WorkerdApi::tryCompileModule()", "name", module.getName());
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(js);
  switch (module.which()) {
    case config::Worker::Module::TEXT: {
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          kj::none,
          jsg::ModuleRegistry::TextModuleInfo(lock,
              Impl::compileTextGlobal(lock, module.getText())));
    }
    case config::Worker::Module::DATA: {
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          kj::none,
          jsg::ModuleRegistry::DataModuleInfo(
              lock,
              Impl::compileDataGlobal(lock, module.getData()).As<v8::ArrayBuffer>()));
    }
    case config::Worker::Module::WASM: {
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          kj::none,
          jsg::ModuleRegistry::WasmModuleInfo(lock,
              Impl::compileWasmGlobal(lock, module.getWasm(), observer)));
    }
    case config::Worker::Module::JSON: {
        return jsg::ModuleRegistry::ModuleInfo(
            lock,
            module.getName(),
            kj::none,
            jsg::ModuleRegistry::JsonModuleInfo(lock,
                Impl::compileJsonGlobal(lock, module.getJson())));
    }
    case config::Worker::Module::ES_MODULE: {
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          module.getEsModule(),
          jsg::ModuleInfoCompileOption::BUNDLE,
          observer);
    }
    case config::Worker::Module::COMMON_JS_MODULE: {
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          kj::none,
          jsg::ModuleRegistry::CommonJsModuleInfo(
              lock,
              module.getName(),
              module.getCommonJsModule()));
    }
    case config::Worker::Module::NODE_JS_COMPAT_MODULE: {
      KJ_REQUIRE(featureFlags.getNodeJsCompat(),
          "The nodejs_compat compatibility flag is required to use the nodeJsCompatModule type.");
      return jsg::ModuleRegistry::ModuleInfo(
          lock,
          module.getName(),
          kj::none,
          jsg::ModuleRegistry::NodeJsModuleInfo(
              lock,
              module.getName(),
              module.getNodeJsCompatModule()));
    }
    case config::Worker::Module::PYTHON_MODULE: {
      // Nothing to do. Handled in compileModules.
      return kj::none;
    }
    case config::Worker::Module::PYTHON_REQUIREMENT: {
      // Nothing to do. Handled in compileModules.
      return kj::none;
    }
  }
  KJ_UNREACHABLE;
}

void WorkerdApi::compileModules(
    jsg::Lock& lockParam, config::Worker::Reader conf,
    Worker::ValidationErrorReporter& errorReporter,
    capnp::List<config::Extension>::Reader extensions) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileModules()");
  lockParam.withinHandleScope([&] {
    auto modules = jsg::ModuleRegistryImpl<JsgWorkerdIsolate_TypeWrapper>::from(lockParam);

    auto confModules = conf.getModules();
    using namespace workerd::api::pyodide;
    auto featureFlags = getFeatureFlags();
    if (hasPythonModules(confModules)) {
      KJ_REQUIRE(featureFlags.getPythonWorkers(),
          "The python_workers compatibility flag is required to use Python.");
      // Inject pyodide bootstrap module.
      {
        auto mainModule = confModules.begin();
        capnp::MallocMessageBuilder message;
        auto module = message.getRoot<config::Worker::Module>();
        module.setEsModule(PYTHON_ENTRYPOINT);
        auto info = tryCompileModule(lockParam, module, modules->getObserver(), featureFlags);
        auto path = kj::Path::parse(mainModule->getName());
        modules->add(path, kj::mv(KJ_REQUIRE_NONNULL(info)));
      }
      // Inject metadata that the entrypoint module will read.
      {
        using ModuleInfo = jsg::ModuleRegistry::ModuleInfo;
        using ObjectModuleInfo = jsg::ModuleRegistry::ObjectModuleInfo;
        using ResolveMethod = jsg::ModuleRegistry::ResolveMethod;
        auto specifier = "pyodide-internal:runtime-generated/metadata";
        auto metadataReader = makePyodideMetadataReader(conf);
        modules->addBuiltinModule(
            specifier,
            [specifier = kj::str(specifier), metadataReader = kj::mv(metadataReader)](
                jsg::Lock& js, ResolveMethod, kj::Maybe<const kj::Path&>&) mutable {
              auto& wrapper = JsgWorkerdIsolate_TypeWrapper::from(js.v8Isolate);
              auto wrap = wrapper.wrap(js.v8Context(), kj::none, kj::mv(metadataReader));
              return kj::Maybe(ModuleInfo(js, specifier, kj::none, ObjectModuleInfo(js, wrap)));
            },
            jsg::ModuleRegistry::Type::INTERNAL);
      }
      // Inject artifact bundler.
      {
        using ModuleInfo = jsg::ModuleRegistry::ModuleInfo;
        using ObjectModuleInfo = jsg::ModuleRegistry::ObjectModuleInfo;
        using ResolveMethod = jsg::ModuleRegistry::ResolveMethod;
        auto specifier = "pyodide-internal:artifacts";
        modules->addBuiltinModule(specifier,
            [specifier = kj::str(specifier)](
                jsg::Lock& js, ResolveMethod, kj::Maybe<const kj::Path&>&) mutable {
          auto& wrapper = JsgWorkerdIsolate_TypeWrapper::from(js.v8Isolate);
          auto wrap = wrapper.wrap(js.v8Context(), kj::none, ArtifactBundler::makeDisabledBundler());
          return kj::Maybe(ModuleInfo(js, specifier, kj::none, ObjectModuleInfo(js, wrap)));
        },
            jsg::ModuleRegistry::Type::INTERNAL);
      }

      // Inject jaeger internal tracer in a disabled state (we don't have a use for it in workerd)
      {
        using ModuleInfo = jsg::ModuleRegistry::ModuleInfo;
        using ObjectModuleInfo = jsg::ModuleRegistry::ObjectModuleInfo;
        using ResolveMethod = jsg::ModuleRegistry::ResolveMethod;
        auto specifier = "pyodide-internal:internalJaeger";
        modules->addBuiltinModule(
            specifier,
            [specifier = kj::str(specifier)](
                jsg::Lock& js, ResolveMethod, kj::Maybe<const kj::Path&>&) mutable {
              auto& wrapper = JsgWorkerdIsolate_TypeWrapper::from(js.v8Isolate);
              auto wrap = wrapper.wrap(js.v8Context(), kj::none, DisabledInternalJaeger::create());
              return kj::Maybe(ModuleInfo(js, specifier, kj::none, ObjectModuleInfo(js, wrap)));
            },
            jsg::ModuleRegistry::Type::INTERNAL);
      }

      // Inject disk cache module
      KJ_IF_SOME(pcr, impl->pyodideCacheRoot) {
        using ModuleInfo = jsg::ModuleRegistry::ModuleInfo;
        using ObjectModuleInfo = jsg::ModuleRegistry::ObjectModuleInfo;
        using ResolveMethod = jsg::ModuleRegistry::ResolveMethod;
        auto specifier = "pyodide-internal:disk_cache";
        auto diskCache = jsg::alloc<DiskCache>(pcr->clone());
        modules->addBuiltinModule(
          specifier,
          [specifier = kj::str(specifier), diskCache = kj::mv(diskCache)](
              jsg::Lock& js, ResolveMethod, kj::Maybe<const kj::Path&>&) mutable {
            auto& wrapper = JsgWorkerdIsolate_TypeWrapper::from(js.v8Isolate);
            auto wrap = wrapper.wrap(js.v8Context(), kj::none, kj::mv(diskCache));
            return kj::Maybe(ModuleInfo(js, specifier, kj::none, ObjectModuleInfo(js, wrap)));
          },
          jsg::ModuleRegistry::Type::INTERNAL);
      }
    }

    for (auto module: confModules) {
      auto path = kj::Path::parse(module.getName());
      auto maybeInfo = tryCompileModule(lockParam, module, modules->getObserver(), featureFlags);
      KJ_IF_SOME(info, maybeInfo) {
        modules->add(path, kj::mv(info));
      }
    }

    api::registerModules(*modules, featureFlags);

    // todo(perf): we'd like to find a way to precompile these on server startup and use isolate
    // cloning for faster worker creation.
    for (auto extension: extensions) {
      for (auto module: extension.getModules()) {
        modules->addBuiltinModule(module.getName(), module.getEsModule().asArray(),
            module.getInternal() ? jsg::ModuleRegistry::Type::INTERNAL : jsg::ModuleRegistry::Type::BUILTIN);
      }
    }
  });
}

class ActorIdFactoryImpl final: public ActorIdFactory {
public:
  ActorIdFactoryImpl(kj::StringPtr uniqueKey) {
    KJ_ASSERT(SHA256(uniqueKey.asBytes().begin(), uniqueKey.size(), key) == key);
  }

  class ActorIdImpl final: public ActorId {
  public:
    ActorIdImpl(const kj::byte idParam[SHA256_DIGEST_LENGTH], kj::Maybe<kj::String> name)
        : name(kj::mv(name)) {
      memcpy(id, idParam, sizeof(id));
    }

    kj::String toString() const override {
      return kj::encodeHex(kj::ArrayPtr<const kj::byte>(id));
    }
    kj::Maybe<kj::StringPtr> getName() const override {
      return name;
    }
    bool equals(const ActorId& other) const override {
      return memcmp(id, kj::downcast<const ActorIdImpl>(other).id, sizeof(id)) == 0;
    }
    kj::Own<ActorId> clone() const override {
      return kj::heap<ActorIdImpl>(id, name.map([](kj::StringPtr str) { return kj::str(str); }));
    }

  private:
    kj::byte id[SHA256_DIGEST_LENGTH];
    kj::Maybe<kj::String> name;
  };

  kj::Own<ActorId> newUniqueId(kj::Maybe<kj::StringPtr> jurisdiction) override {
    JSG_REQUIRE(jurisdiction == kj::none, Error,
        "Jurisdiction restrictions are not implemented in workerd.");

    // We want to randomly-generate the first 16 bytes, then HMAC those to produce the latter
    // 16 bytes. But the HMAC will produce 32 bytes, so we're only taking a prefix of it. We'll
    // allocate a single array big enough to output the HMAC as a suffix, which will then get
    // truncated.
    kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH];

    if (isPredictableModeForTest()) {
      memcpy(id, &counter, sizeof(counter));
      memset(id + sizeof(counter), 0, BASE_LENGTH - sizeof(counter));
      ++counter;
    } else {
      KJ_ASSERT(RAND_bytes(id, BASE_LENGTH) == 1);
    }

    computeMac(id);
    return kj::heap<ActorIdImpl>(id, kj::none);
  }

  kj::Own<ActorId> idFromName(kj::String name) override {
    kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH];

    // Compute the first half of the ID by HMACing the name itself. We're using HMAC as a keyed
    // hash here, not actually for authentication, but it works.
    uint len = SHA256_DIGEST_LENGTH;
    KJ_ASSERT(HMAC(EVP_sha256(), key, sizeof(key), name.asBytes().begin(), name.size(), id, &len)
                   == id);
    KJ_ASSERT(len == SHA256_DIGEST_LENGTH);

    computeMac(id);
    return kj::heap<ActorIdImpl>(id, kj::mv(name));
  }

  kj::Own<ActorId> idFromString(kj::String str) override {
    auto decoded = kj::decodeHex(str);
    JSG_REQUIRE(str.size() == SHA256_DIGEST_LENGTH * 2 && !decoded.hadErrors &&
                decoded.size() == SHA256_DIGEST_LENGTH,
                TypeError, "Invalid Durable Object ID: must be 64 hex digits");

    kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH];
    memcpy(id, decoded.begin(), BASE_LENGTH);
    computeMac(id);

    // Verify that the computed mac matches the input.
    JSG_REQUIRE(memcmp(id + BASE_LENGTH, decoded.begin() + BASE_LENGTH,
                decoded.size() - BASE_LENGTH) == 0,
                TypeError, "Durable Object ID is not valid for this namespace.");

    return kj::heap<ActorIdImpl>(id, kj::none);
  }

  kj::Own<ActorIdFactory> cloneWithJurisdiction(kj::StringPtr jurisdiction) override {
    JSG_FAIL_REQUIRE(Error, "Jurisdiction restrictions are not implemented in workerd.");
  }

  bool matchesJurisdiction(const ActorId& id) override {
    return true;
  }

private:
  kj::byte key[SHA256_DIGEST_LENGTH];

  uint64_t counter = 0;   // only used in predictable mode

  static constexpr size_t BASE_LENGTH = SHA256_DIGEST_LENGTH / 2;
  void computeMac(kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]) {
    // Given that the first `BASE_LENGTH` bytes of `id` are filled in, compute the second half
    // of the ID by HMACing the first half. The id must be in a buffer large enough to store the
    // first half of the ID plus a full HMAC, even though only a prefix of the HMAC becomes part
    // of the final ID.

    kj::byte* hmacOut = id + BASE_LENGTH;
    uint len = SHA256_DIGEST_LENGTH;
    KJ_ASSERT(HMAC(EVP_sha256(), key, sizeof(key), id, BASE_LENGTH, hmacOut, &len) == hmacOut);
    KJ_ASSERT(len == SHA256_DIGEST_LENGTH);
  }
};

static v8::Local<v8::Value> createBindingValue(
    JsgWorkerdIsolate::Lock& lock,
    const WorkerdApi::Global& global,
    CompatibilityFlags::Reader featureFlags,
    uint32_t ownerId,
    api::MemoryCacheProvider& memoryCacheProvider) {
  TRACE_EVENT("workerd", "WorkerdApi::createBindingValue()");
  using Global = WorkerdApi::Global;
  auto context = lock.v8Context();

  v8::Local<v8::Value> value;

  KJ_SWITCH_ONEOF(global.value) {
    KJ_CASE_ONEOF(json, Global::Json) {
      v8::Local<v8::String> string = lock.wrap(context, kj::mv(json.text));
      value = jsg::check(v8::JSON::Parse(context, string));
    }

    KJ_CASE_ONEOF(pipeline, Global::Fetcher) {
      value = lock.wrap(context, jsg::alloc<api::Fetcher>(
          pipeline.channel,
          pipeline.requiresHost ? api::Fetcher::RequiresHostAndProtocol::YES
                                : api::Fetcher::RequiresHostAndProtocol::NO,
          pipeline.isInHouse));
    }

    KJ_CASE_ONEOF(ns, Global::KvNamespace) {
      value = lock.wrap(context, jsg::alloc<api::KvNamespace>(
          kj::Array<api::KvNamespace::AdditionalHeader>{}, ns.subrequestChannel));
    }

    KJ_CASE_ONEOF(r2, Global::R2Bucket) {
      value = lock.wrap(context,
          jsg::alloc<api::public_beta::R2Bucket>(featureFlags, r2.subrequestChannel));
    }

    KJ_CASE_ONEOF(r2a, Global::R2Admin) {
      value = lock.wrap(context,
          jsg::alloc<api::public_beta::R2Admin>(featureFlags, r2a.subrequestChannel));
    }

    KJ_CASE_ONEOF(ns, Global::QueueBinding) {
      value = lock.wrap(context, jsg::alloc<api::WorkerQueue>(ns.subrequestChannel));
    }

    KJ_CASE_ONEOF(key, Global::CryptoKey) {
      api::SubtleCrypto::ImportKeyData keyData;
      KJ_SWITCH_ONEOF(key.keyData) {
        KJ_CASE_ONEOF(data, kj::Array<byte>) {
          keyData = kj::heapArray(data.asPtr());
        }
        KJ_CASE_ONEOF(json, Global::Json) {
          v8::Local<v8::String> str = lock.wrap(context, kj::mv(json.text));
          v8::Local<v8::Value> obj = jsg::check(v8::JSON::Parse(context, str));
          keyData = lock.unwrap<api::SubtleCrypto::ImportKeyData>(context, obj);
        }
      }

      v8::Local<v8::String> algoStr = lock.wrap(context, kj::mv(key.algorithm.text));
      v8::Local<v8::Value> algo = jsg::check(v8::JSON::Parse(context, algoStr));
      auto importKeyAlgo = lock.unwrap<
          kj::OneOf<kj::String, api::SubtleCrypto::ImportKeyAlgorithm>>(context, algo);

      jsg::Ref<api::CryptoKey> importedKey = api::SubtleCrypto().importKeySync(lock,
          key.format, kj::mv(keyData),
          api::interpretAlgorithmParam(kj::mv(importKeyAlgo)),
          key.extractable, key.usages);

      value = lock.wrap(context, kj::mv(importedKey));
    }

    KJ_CASE_ONEOF(cache, Global::MemoryCache) {
      value = lock.wrap(context, jsg::alloc<api::MemoryCache>(
          api::SharedMemoryCache::Use(
              memoryCacheProvider.getInstance(cache.cacheId),
              {
                .maxKeys = cache.maxKeys,
                .maxValueSize = cache.maxValueSize,
                .maxTotalValueSize = cache.maxTotalValueSize,
              })));
    }

    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      value = lock.wrap(context, jsg::alloc<api::ColoLocalActorNamespace>(ns.actorChannel));
    }

    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      value = lock.wrap(context, jsg::alloc<api::DurableObjectNamespace>(ns.actorChannel,
          kj::heap<ActorIdFactoryImpl>(ns.uniqueKey)));
    }

    KJ_CASE_ONEOF(ae, Global::AnalyticsEngine) {
        // Use subrequestChannel as logfwdrChannel
        value = lock.wrap(context, jsg::alloc<api::AnalyticsEngine>(ae.subrequestChannel,
                    kj::str(ae.dataset), ae.version, ownerId));
    }

    KJ_CASE_ONEOF(text, kj::String) {
      value = lock.wrap(context, kj::mv(text));
    }

    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      value = lock.wrap(context, kj::heapArray(data.asPtr()));
    }

    KJ_CASE_ONEOF(wrapped, Global::Wrapped) {
      auto moduleRegistry = jsg::ModuleRegistry::from(lock);
      auto moduleName = kj::Path::parse(wrapped.moduleName);

      // wrapped bindings can be produced by internal modules only
      KJ_IF_SOME(moduleInfo, moduleRegistry->resolve(lock, moduleName, kj::none,
           jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY)) {
        // obtain the module
        auto module = moduleInfo.module.getHandle(lock);
        jsg::instantiateModule(lock, module);

        // build env object with inner bindings
        auto env = v8::Object::New(lock.v8Isolate);
        for (const auto& innerBinding: wrapped.innerBindings) {
          lock.v8Set(env, innerBinding.name,
                     createBindingValue(lock, innerBinding, featureFlags, ownerId,
                                         memoryCacheProvider));
        }

        // obtain exported function to call
        auto moduleNs = jsg::check(module->GetModuleNamespace()->ToObject(context));
        auto fn = lock.v8Get(moduleNs, wrapped.entrypoint);
        KJ_ASSERT(fn->IsFunction(), "Entrypoint is not a function", wrapped.entrypoint);

        // invoke the function, its result will be binding value
        auto args = kj::arr(env.As<v8::Value>());
        value = jsg::check(v8::Function::Cast(*fn)-> Call(context, context->Global(),
            args.size(), args.begin()));
      } else {
        KJ_LOG(ERROR, "wrapped binding module can't be resolved (internal modules only)", moduleName);
      }
    }
    KJ_CASE_ONEOF(hyperdrive, Global::Hyperdrive) {
      value = lock.wrap(context, jsg::alloc<api::Hyperdrive>(
                                     hyperdrive.subrequestChannel, kj::str(hyperdrive.database),
                                     kj::str(hyperdrive.user), kj::str(hyperdrive.password),
                                     kj::str(hyperdrive.scheme)));
    }
    KJ_CASE_ONEOF(unsafe, Global::UnsafeEval) {
      value = lock.wrap(context, jsg::alloc<api::UnsafeEval>());
    }
  }

  return value;
}

void WorkerdApi::compileGlobals(
    jsg::Lock& lockParam, kj::ArrayPtr<const Global> globals,
    v8::Local<v8::Object> target,
    uint32_t ownerId) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileGlobals()");
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);
  lockParam.withinHandleScope([&] {
    auto& featureFlags = *impl->features;

    for (auto& global: globals) {
      lockParam.withinHandleScope([&] {
        // Don't use String's usual TypeHandler here because we want to intern the string.
        auto value = createBindingValue(lock, global, featureFlags, ownerId,
                                        impl->memoryCacheProvider);
        KJ_ASSERT(!value.IsEmpty(), "global did not produce v8::Value");
        lockParam.v8Set(target, global.name, value);
      });
    }
  });
}

void WorkerdApi::setModuleFallbackCallback(
    kj::Function<ModuleFallbackCallback>&& callback) const {
  auto& isolateBase = const_cast<JsgWorkerdIsolate&>(impl->jsgIsolate);
  isolateBase.setModuleFallbackCallback(kj::mv(callback));
}

// =======================================================================================

WorkerdApi::Global WorkerdApi::Global::clone() const {
  Global result;
  result.name = kj::str(name);

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(json, Global::Json) {
      result.value = json.clone();
    }
    KJ_CASE_ONEOF(fetcher, Global::Fetcher) {
      result.value = fetcher.clone();
    }
    KJ_CASE_ONEOF(kvNamespace, Global::KvNamespace) {
      result.value = kvNamespace.clone();
    }
    KJ_CASE_ONEOF(r2Bucket, Global::R2Bucket) {
      result.value = r2Bucket.clone();
    }
    KJ_CASE_ONEOF(r2Admin, Global::R2Admin) {
      result.value = r2Admin.clone();
    }
    KJ_CASE_ONEOF(queueBinding, Global::QueueBinding) {
      result.value = queueBinding.clone();
    }
    KJ_CASE_ONEOF(key, Global::CryptoKey) {
      result.value = key.clone();
    }
    KJ_CASE_ONEOF(cache, Global::MemoryCache) {
      result.value = cache.clone();
    }
    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ae, Global::AnalyticsEngine) {
      result.value = ae.clone();
    }
    KJ_CASE_ONEOF(text, kj::String) {
      result.value = kj::str(text);
    }
    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      result.value = kj::heapArray(data.asPtr());
    }
    KJ_CASE_ONEOF(wrapped, Global::Wrapped) {
      result.value = wrapped.clone();
    }
    KJ_CASE_ONEOF(hyperdrive, Global::Hyperdrive) {
      result.value = hyperdrive.clone();
    }
    KJ_CASE_ONEOF(unsafe, Global::UnsafeEval) {
      result.value = Global::UnsafeEval {};
    }
  }

  return result;
}

const WorkerdApi& WorkerdApi::from(const Worker::Api& api) {
  return kj::downcast<const WorkerdApi>(api);
}

}  // namespace workerd::server
