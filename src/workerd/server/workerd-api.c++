// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "workerd-api.h"

#include <workerd/api/actor-state.h>
#include <workerd/api/actor.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/api/base64.h>
#include <workerd/api/cache.h>
#include <workerd/api/capnp.h>
#include <workerd/api/commonjs.h>
#include <workerd/api/container.h>
#include <workerd/api/crypto/impl.h>
#include <workerd/api/encoding.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#include <workerd/api/export-loopback.h>
#include <workerd/api/filesystem.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/html-rewriter.h>
#include <workerd/api/hyperdrive.h>
#include <workerd/api/kv.h>
#include <workerd/api/memory-cache.h>
#include <workerd/api/modules.h>
#include <workerd/api/node/node.h>
#include <workerd/api/performance.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/api/pyodide/requirements.h>
#include <workerd/api/pyodide/setup-emscripten.h>
#include <workerd/api/queue.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/r2.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/sql.h>
#include <workerd/api/streams.h>
#include <workerd/api/streams/standard.h>
#include <workerd/api/sync-kv.h>
#include <workerd/api/trace.h>
#include <workerd/api/tracing-module.h>
#include <workerd/api/unsafe.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/urlpattern-standard.h>
#include <workerd/api/urlpattern.h>
#include <workerd/api/worker-loader.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/api/workers-module.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/promise-wrapper.h>
#include <workerd/io/worker-modules.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/url.h>
#include <workerd/jsg/util.h>
#ifdef WORKERD_USE_TRANSPILER
#include <workerd/rust/transpiler/lib.rs.h>
#endif  // defined(WORKERD_USE_TRANSPILER)
#include <workerd/server/actor-id-impl.h>
#include <workerd/server/fallback-service.h>
#include <workerd/server/workerd-debug-port-client.h>
#include <workerd/util/autogate.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/use-perfetto-categories.h>

#include <kj-rs/kj-rs.h>
#include <pyodide/generated/pyodide_extra.capnp.h>
#include <pyodide/python-entrypoint.embed.h>

#include <kj/compat/gzip.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/compat/url.h>

using namespace kj_rs;

namespace workerd::server {

using api::pyodide::PythonConfig;

namespace {
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
    EW_BASE64_ISOLATE_TYPES,
    EW_BASICS_ISOLATE_TYPES,
    EW_BLOB_ISOLATE_TYPES,
    EW_CACHE_ISOLATE_TYPES,
    EW_CAPNP_TYPES,
    EW_CONTAINER_ISOLATE_TYPES,
    EW_CJS_ISOLATE_TYPES,
    EW_CRYPTO_ISOLATE_TYPES,
    EW_ENCODING_ISOLATE_TYPES,
    EW_EVENTS_ISOLATE_TYPES,
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
    EW_URLPATTERN_STANDARD_ISOLATE_TYPES,
    EW_WEB_FILESYSTEM_ISOLATE_TYPE,
    EW_FILESYSTEM_ISOLATE_TYPES,
    EW_WEBSOCKET_ISOLATE_TYPES,
    EW_SQL_ISOLATE_TYPES,
    EW_SYNC_KV_ISOLATE_TYPES,
    EW_NODE_ISOLATE_TYPES,
    EW_RTTI_ISOLATE_TYPES,
    EW_HYPERDRIVE_ISOLATE_TYPES,
    EW_EVENTSOURCE_ISOLATE_TYPES,
    EW_WORKER_LOADER_ISOLATE_TYPES,
    EW_MESSAGECHANNEL_ISOLATE_TYPES,
    EW_WORKERS_MODULE_ISOLATE_TYPES,
    EW_EXPORT_LOOPBACK_ISOLATE_TYPES,
    EW_PERFORMANCE_ISOLATE_TYPES,
    EW_TRACING_MODULE_ISOLATE_TYPES,
    EW_WORKERD_DEBUG_PORT_CLIENT_ISOLATE_TYPES,
    workerd::api::EnvModule,
    workerd::api::PythonPatchedEnv,

    jsg::TypeWrapperExtension<PromiseWrapper>,
    jsg::InjectConfiguration<CompatibilityFlags::Reader>,
    Worker::Api::ErrorInterface);

static const PythonConfig defaultConfig{
  .packageDiskCacheRoot = kj::none,
  .pyodideDiskCacheRoot = kj::none,
  .createSnapshot = false,
  .createBaselineSnapshot = false,
};

// An ActorStorage implementation which will always respond to reads as if the state is empty,
// and will fail any writes.
class EmptyReadOnlyActorStorageImpl final: public rpc::ActorStorage::Stage::Server {
 public:
  kj::Promise<void> get(GetContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> getMultiple(GetMultipleContext context) override {
    return context.getParams()
        .getStream()
        .endRequest(capnp::MessageSize{2, 0})
        .sendIgnoringResult();
  }
  kj::Promise<void> list(ListContext context) override {
    return context.getParams()
        .getStream()
        .endRequest(capnp::MessageSize{2, 0})
        .sendIgnoringResult();
  }
  kj::Promise<void> getAlarm(GetAlarmContext context) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> txn(TxnContext context) override {
    auto results = context.getResults(capnp::MessageSize{2, 1});
    results.setTransaction(kj::heap<TransactionImpl>());
    return kj::READY_NOW;
  }

 private:
  class TransactionImpl final: public rpc::ActorStorage::Stage::Transaction::Server {
   protected:
    kj::Promise<void> get(GetContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> getMultiple(GetMultipleContext context) override {
      return context.getParams()
          .getStream()
          .endRequest(capnp::MessageSize{2, 0})
          .sendIgnoringResult();
    }
    kj::Promise<void> list(ListContext context) override {
      return context.getParams()
          .getStream()
          .endRequest(capnp::MessageSize{2, 0})
          .sendIgnoringResult();
    }
    kj::Promise<void> getAlarm(GetAlarmContext context) override {
      return kj::READY_NOW;
    }
    kj::Promise<void> commit(CommitContext context) override {
      return kj::READY_NOW;
    }
  };
};

}  // namespace

/**
 * This function matches the implementation of `getPythonRequirements` in the internal repo. But it
 * works on the workerd ModulesSource definition rather than the WorkerBundle.
 */
kj::Array<kj::String> getPythonRequirements(const Worker::Script::ModulesSource& source) {
  kj::Vector<kj::String> requirements;

  for (auto& def: source.modules) {
    KJ_SWITCH_ONEOF(def.content) {
      KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
        requirements.add(api::pyodide::canonicalizePythonPackageName(def.name));
      }
      KJ_CASE_ONEOF_DEFAULT {
        break;
      }
    }
  }

  return requirements.releaseAsArray();
}

struct WorkerdApi::Impl final {
  kj::Own<CompatibilityFlags::Reader> features;
  capnp::List<config::Extension>::Reader extensions;
  kj::Own<JsgIsolateObserver> observer;
  JsgWorkerdIsolate jsgIsolate;
  api::MemoryCacheProvider& memoryCacheProvider;
  const PythonConfig& pythonConfig;

  class Configuration {
   public:
    Configuration(Impl& impl)
        : features(*impl.features),
          jsgConfig(jsg::JsgConfig{
            .noSubstituteNull = features.getNoSubstituteNull(),
            .unwrapCustomThenables = features.getUnwrapCustomThenables(),
            .fetchIterableTypeSupport = features.getFetchIterableTypeSupport(),
            .fetchIterableTypeSupportOverrideAdjustment =
                features.getFetchIterableTypeSupportOverrideAdjustment(),
            .fastApiEnabled = util::Autogate::isEnabled(util::AutogateKey::V8_FAST_API),
          }) {}
    operator const CompatibilityFlags::Reader() const {
      return features;
    }
    operator const jsg::JsgConfig&() const {
      return jsgConfig;
    }

   private:
    CompatibilityFlags::Reader& features;
    jsg::JsgConfig jsgConfig;
  };

  Impl(jsg::V8System& v8System,
      CompatibilityFlags::Reader featuresParam,
      capnp::List<config::Extension>::Reader extensionsParam,
      v8::Isolate::CreateParams createParams,
      v8::IsolateGroup group,
      kj::Own<JsgIsolateObserver> observerParam,
      api::MemoryCacheProvider& memoryCacheProvider,
      const PythonConfig& pythonConfig = defaultConfig)
      : features(capnp::clone(featuresParam)),
        extensions(extensionsParam),
        observer(kj::atomicAddRef(*observerParam)),
        jsgIsolate(
            v8System, group, Configuration(*this), kj::mv(observerParam), kj::mv(createParams)),
        memoryCacheProvider(memoryCacheProvider),
        pythonConfig(pythonConfig) {
    jsgIsolate.runInLockScope([&](JsgWorkerdIsolate::Lock& lock) {
      if (featuresParam.getNewModuleRegistry()) {
        jsgIsolate.setUsingNewModuleRegistry();
      }

      // Allows us to begin experimenting with eval/new fuction enabled in
      // preparation for *possibly* enabling it by default in the future
      // once v8 sandbox is fully enabled and rolled out.
      if (featuresParam.getExperimentalAllowEvalAlways()) {
        jsgIsolate.setAllowsAllowEval();
      }
    });
  }
};

WorkerdApi::WorkerdApi(jsg::V8System& v8System,
    CompatibilityFlags::Reader features,
    capnp::List<config::Extension>::Reader extensions,
    v8::Isolate::CreateParams createParams,
    v8::IsolateGroup group,
    kj::Own<JsgIsolateObserver> observer,
    api::MemoryCacheProvider& memoryCacheProvider,
    const PythonConfig& pythonConfig)
    : impl(kj::heap<Impl>(v8System,
          features,
          extensions,
          kj::mv(createParams),
          group,
          kj::mv(observer),
          memoryCacheProvider,
          pythonConfig)) {}
WorkerdApi::~WorkerdApi() noexcept(false) {}

kj::Own<jsg::Lock> WorkerdApi::lock(jsg::V8StackScope& stackScope) const {
  return kj::heap<JsgWorkerdIsolate::Lock>(impl->jsgIsolate, stackScope);
}
CompatibilityFlags::Reader WorkerdApi::getFeatureFlags() const {
  return *impl->features;
}
jsg::JsContext<api::ServiceWorkerGlobalScope> WorkerdApi::newContext(
    jsg::Lock& lock, Worker::Api::NewContextOptions options) const {
  jsg::NewContextOptions opts{
    .newModuleRegistry = options.newModuleRegistry,
    .schemaLoader = options.schemaLoader,
    .enableWeakRef = getFeatureFlags().getJsWeakRef(),
  };
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).newContext<api::ServiceWorkerGlobalScope>(
      kj::mv(opts));
}
jsg::Dict<NamedExport> WorkerdApi::unwrapExports(
    jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).unwrap<jsg::Dict<NamedExport>>(
      lock.v8Context(), moduleNamespace);
}
NamedExport WorkerdApi::unwrapExport(jsg::Lock& lock, v8::Local<v8::Value> exportVal) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).unwrap<NamedExport>(
      lock.v8Context(), exportVal);
}
EntrypointClasses WorkerdApi::getEntrypointClasses(jsg::Lock& lock) const {
  auto& typedLock = kj::downcast<JsgWorkerdIsolate::Lock>(lock);

  return {
    .workerEntrypoint = typedLock.getConstructor<api::WorkerEntrypoint>(lock.v8Context()),
    .durableObject = typedLock.getConstructor<api::DurableObjectBase>(lock.v8Context()),
    .workflowEntrypoint = typedLock.getConstructor<api::WorkflowEntrypoint>(lock.v8Context()),
  };
}
const jsg::TypeHandler<Worker::Api::ErrorInterface>& WorkerdApi::getErrorInterfaceTypeHandler(
    jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<ErrorInterface>();
}

const jsg::TypeHandler<api::QueueExportedHandler>& WorkerdApi::getQueueTypeHandler(
    jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<api::QueueExportedHandler>();
}

jsg::JsObject WorkerdApi::wrapExecutionContext(
    jsg::Lock& lock, jsg::Ref<api::ExecutionContext> ref) const {
  return jsg::JsObject(
      kj::downcast<JsgWorkerdIsolate::Lock>(lock).wrap(lock.v8Context(), kj::mv(ref)));
}

const jsg::IsolateObserver& WorkerdApi::getObserver() const {
  return *impl->observer;
}

void WorkerdApi::setIsolateObserver(IsolateObserver&) {};

Worker::Script::Source WorkerdApi::extractSource(kj::StringPtr name,
    config::Worker::Reader conf,
    CompatibilityFlags::Reader featureFlags,
    Worker::ValidationErrorReporter& errorReporter) {
  TRACE_EVENT("workerd", "WorkerdApi::extractSource()");
  switch (conf.which()) {
    case config::Worker::MODULES: {
      auto modules = conf.getModules();
      if (modules.size() == 0) {
        errorReporter.addError(kj::str("Modules list cannot be empty."));
        goto invalid;
      }

      bool isPython = false;
      auto moduleArray = KJ_MAP(module, modules) -> Worker::Script::Module {
        if (module.isPythonModule()) {
          isPython = true;
        }
        return readModuleConf(module, featureFlags, errorReporter);
      };

      Worker::Script::ModulesSource result{
        .mainModule = modules[0].getName(), .modules = kj::mv(moduleArray), .isPython = isPython};

      return result;
    }
    case config::Worker::SERVICE_WORKER_SCRIPT: {
      uint wasmCount = 0;
      for (auto binding: conf.getBindings()) {
        if (binding.isWasmModule()) ++wasmCount;
      }

      auto globals = kj::heapArrayBuilder<Worker::Script::Module>(wasmCount);
      for (auto binding: conf.getBindings()) {
        if (binding.isWasmModule()) {
          globals.add(Worker::Script::Module{.name = binding.getName(),
            .content = Worker::Script::WasmModule{.body = binding.getWasmModule()}});
        }
      }

      return Worker::Script::ScriptSource{
        .mainScript = conf.getServiceWorkerScript(),
        .mainScriptName = name,
        .globals = globals.finish(),
      };
    }
    case config::Worker::INHERIT:
      // TODO(beta): Support inherit.
      KJ_FAIL_ASSERT("inherit should have been handled earlier");
  }

  errorReporter.addError(kj::str("Encountered unknown Worker code type. Was the "
                                 "config compiled with a newer version of the schema?"));
invalid:
  return Worker::Script::ScriptSource{""_kj, name, nullptr};
}

kj::Array<Worker::Script::CompiledGlobal> WorkerdApi::compileServiceWorkerGlobals(jsg::Lock& js,
    const Worker::Script::ScriptSource& source,
    const Worker::Isolate& isolate) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileScriptGlobals()");
  const jsg::CompilationObserver& observer = *impl->observer;
  return workerd::modules::legacy::compileServiceWorkerGlobals<JsgWorkerdIsolate>(
      js, source, isolate, observer);
}

namespace {
kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileLegacyModule(jsg::Lock& js,
    kj::StringPtr name,
    const Worker::Script::ModuleContent& content,
    const jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  return modules::legacy::tryCompileLegacyModule<JsgWorkerdIsolate>(
      js, name, content, observer, featureFlags);
}
}  // namespace

// Part of the original module registry implementation.
kj::Maybe<jsg::ModuleRegistry::ModuleInfo> WorkerdApi::tryCompileModule(jsg::Lock& js,
    config::Worker::Module::Reader conf,
    const jsg::CompilationObserver& observer,
    CompatibilityFlags::Reader featureFlags) {
  auto module = readModuleConf(conf, featureFlags);
  return tryCompileLegacyModule(js, module.name, module.content, observer, featureFlags);
}

Worker::Script::Module WorkerdApi::readModuleConf(config::Worker::Module::Reader conf,
    CompatibilityFlags::Reader featureFlags,
    kj::Maybe<Worker::ValidationErrorReporter&> errorReporter) {
  return {.name = conf.getName(), .content = [&]() -> Worker::Script::ModuleContent {
    switch (conf.which()) {
      case config::Worker::Module::TEXT:
        return Worker::Script::TextModule{conf.getText()};
      case config::Worker::Module::DATA:
        return Worker::Script::DataModule{conf.getData()};
      case config::Worker::Module::WASM:
        return Worker::Script::WasmModule{conf.getWasm()};
      case config::Worker::Module::JSON:
        return Worker::Script::JsonModule{conf.getJson()};
      case config::Worker::Module::ES_MODULE:
        // TODO(soon): Update this to also support full TS transform
        // with a separate compat flag.
#ifdef WORKERD_USE_TRANSPILER
        if (featureFlags.getTypescriptStripTypes()) {
          auto output = rust::transpiler::ts_strip(
              // value comes from capnp so it is a valid utf-8
              conf.getName().as<RustUncheckedUtf8>(), conf.getEsModule().asBytes().as<Rust>());

          if (output.success) {
            return Worker::Script::EsModule{
              .body = ::kj::from<Rust>(output.code), .ownBody = kj::mv(output.code)};
          }

          auto description = kj::str("Error transpiling ", conf.getName(), " : ", output.error);
          for (auto& diag: output.diagnostics) {
            description = kj::str(description, "\n    ", diag.message);
          }
          KJ_IF_SOME(reporter, errorReporter) {
            reporter.addError(kj::mv(description));
            return Worker::Script::TextModule{""};
          } else {
            KJ_FAIL_REQUIRE(description);
          }
        }
#endif  // defined(WORKERD_USE_TRANSPILER)
        return Worker::Script::EsModule{static_cast<kj::StringPtr>(conf.getEsModule())};
      case config::Worker::Module::COMMON_JS_MODULE: {
        Worker::Script::CommonJsModule result{.body = conf.getCommonJsModule()};
        if (conf.hasNamedExports()) {
          result.namedExports = KJ_MAP(name, conf.getNamedExports()) -> kj::StringPtr { return name; };
        }
        return result;
      }
      case config::Worker::Module::PYTHON_MODULE:
        return Worker::Script::PythonModule{conf.getPythonModule()};
      case config::Worker::Module::PYTHON_REQUIREMENT:
        return Worker::Script::PythonRequirement{};
      case config::Worker::Module::OBSOLETE: {
        // A non-supported or obsolete module type was configured
        KJ_FAIL_REQUIRE("Worker bundle specified an unsupported module type");
      }
    }

    KJ_IF_SOME(e, errorReporter) {
      e.addError(kj::str("Encountered unknown Worker.Module type. Was the "
                         "config compiled with a newer version of the schema?"));
      return Worker::Script::TextModule{""};
    } else {
      KJ_FAIL_REQUIRE("unknown module type", (uint)conf.which());
    }
  }()};
}

// Part of the original module registry implementation.
void WorkerdApi::compileModules(jsg::Lock& lockParam,
    const Worker::Script::ModulesSource& source,
    const Worker::Isolate& isolate,
    kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts,
    SpanParent parentSpan) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileModules()");
  lockParam.withinHandleScope([&] {
    auto modules = jsg::ModuleRegistryImpl<JsgWorkerdIsolate_TypeWrapper>::from(lockParam);

    using namespace workerd::api::pyodide;
    auto featureFlags = getFeatureFlags();

    for (auto& module: source.modules) {
      auto path = kj::Path::parse(module.name);
      auto maybeInfo = tryCompileLegacyModule(
          lockParam, module.name, module.content, modules->getObserver(), featureFlags);
      KJ_IF_SOME(info, maybeInfo) {
        modules->add(path, kj::mv(info));
      }
    }

    api::registerModules(*modules, featureFlags);

    if (source.isPython) {
      modules::python::registerPythonWorkerdModules<JsgWorkerdIsolate>(
          lockParam, *modules, featureFlags, kj::mv(artifacts), impl->pythonConfig, source);
    }

    for (auto extension: impl->extensions) {
      for (auto module: extension.getModules()) {
        modules->addBuiltinModule(module.getName(), module.getEsModule().asArray(),
            module.getInternal() ? jsg::ModuleRegistry::Type::INTERNAL
                                 : jsg::ModuleRegistry::Type::BUILTIN);
      }
    }
  });
}

static v8::Local<v8::Value> createBindingValue(JsgWorkerdIsolate::Lock& lock,
    const WorkerdApi::Global& global,
    CompatibilityFlags::Reader featureFlags,
    uint32_t ownerId,
    api::MemoryCacheProvider& memoryCacheProvider) {
  TRACE_EVENT("workerd", "WorkerdApi::createBindingValue()");
  using Global = WorkerdApi::Global;
  auto context = lock.v8Context();

  v8::Local<v8::Value> value;

  // When new binding types are created. If their value resolves to be a string
  // or a JSON stringified/stringifiable value, then it should be added to
  // process.env here as well, just like with Global::Json and kj::String
  // entries.
  //
  // It is important to understand the process.env is fundamentally different
  // from the existing bag of bindings. The keys and values on process.env are
  // fundamentally a Record<string, string>, where any value set on process.env
  // is coerced to a string. Having a separate object for process.env is the
  // easiest approach as opposed to wrapping the bindings/env with a proxy that
  // tries to abstract the details. If this ends up needing to change later then
  // as long as the observable behavior remains the same we can do so without
  // Yet Another Compat Flag.

  KJ_SWITCH_ONEOF(global.value) {
    KJ_CASE_ONEOF(json, Global::Json) {
      value = jsg::check(v8::JSON::Parse(context, lock.str(json.text)));
    }

    KJ_CASE_ONEOF(pipeline, Global::Fetcher) {
      value = lock.wrap(context,
          lock.alloc<api::Fetcher>(pipeline.channel,
              pipeline.requiresHost ? api::Fetcher::RequiresHostAndProtocol::YES
                                    : api::Fetcher::RequiresHostAndProtocol::NO,
              pipeline.isInHouse));
    }

    KJ_CASE_ONEOF(loopback, Global::LoopbackServiceStub) {
      value = lock.wrap(context, lock.alloc<api::LoopbackServiceStub>(loopback.channel));
    }

    KJ_CASE_ONEOF(ns, Global::KvNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::KvNamespace>(kj::str(ns.bindingName),
              kj::Array<api::KvNamespace::AdditionalHeader>{}, ns.subrequestChannel));
    }

    KJ_CASE_ONEOF(r2, Global::R2Bucket) {
      value = lock.wrap(context,
          lock.alloc<api::public_beta::R2Bucket>(
              featureFlags, r2.subrequestChannel, kj::str(r2.bucket), kj::str(r2.bindingName)));
    }

    KJ_CASE_ONEOF(r2a, Global::R2Admin) {
      value = lock.wrap(
          context, lock.alloc<api::public_beta::R2Admin>(featureFlags, r2a.subrequestChannel));
    }

    KJ_CASE_ONEOF(ns, Global::QueueBinding) {
      value = lock.wrap(context, lock.alloc<api::WorkerQueue>(ns.subrequestChannel));
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
      auto importKeyAlgo =
          lock.unwrap<kj::OneOf<kj::String, api::SubtleCrypto::ImportKeyAlgorithm>>(context, algo);

      jsg::Ref<api::CryptoKey> importedKey =
          api::SubtleCrypto().importKeySync(lock, key.format, kj::mv(keyData),
              api::interpretAlgorithmParam(kj::mv(importKeyAlgo)), key.extractable, key.usages);

      value = lock.wrap(context, kj::mv(importedKey));
    }

    KJ_CASE_ONEOF(cache, Global::MemoryCache) {
      value = lock.wrap(context,
          lock.alloc<api::MemoryCache>(
              api::SharedMemoryCache::Use(memoryCacheProvider.getInstance(cache.cacheId),
                  {
                    .maxKeys = cache.maxKeys,
                    .maxValueSize = cache.maxValueSize,
                    .maxTotalValueSize = cache.maxTotalValueSize,
                  })));
    }

    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      value = lock.wrap(context, lock.alloc<api::ColoLocalActorNamespace>(ns.actorChannel));
    }
    KJ_CASE_ONEOF(ns, Global::LoopbackEphemeralActorNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::LoopbackColoLocalActorNamespace>(
              ns.actorChannel, lock.alloc<api::LoopbackDurableObjectClass>(ns.classChannel)));
    }

    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::DurableObjectNamespace>(
              ns.actorChannel, kj::heap<ActorIdFactoryImpl>(ns.uniqueKey)));
    }
    KJ_CASE_ONEOF(ns, Global::LoopbackDurableActorNamespace) {
      value = lock.wrap(context,
          lock.alloc<api::LoopbackDurableObjectNamespace>(ns.actorChannel,
              kj::heap<ActorIdFactoryImpl>(ns.uniqueKey),
              lock.alloc<api::LoopbackDurableObjectClass>(ns.classChannel)));
    }

    KJ_CASE_ONEOF(ae, Global::AnalyticsEngine) {
      // Use subrequestChannel as logfwdrChannel
      value = lock.wrap(context,
          lock.alloc<api::AnalyticsEngine>(
              ae.subrequestChannel, kj::str(ae.dataset), ae.version, ownerId));
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
      KJ_IF_SOME(moduleInfo,
          moduleRegistry->resolve(
              lock, moduleName, kj::none, jsg::ModuleRegistry::ResolveOption::INTERNAL_ONLY)) {
        // obtain the module
        auto module = moduleInfo.module.getHandle(lock);
        jsg::instantiateModule(lock, module);

        // build env object with inner bindings
        auto env = v8::Object::New(lock.v8Isolate);
        for (const auto& innerBinding: wrapped.innerBindings) {
          lock.v8Set(env, innerBinding.name,
              createBindingValue(lock, innerBinding, featureFlags, ownerId, memoryCacheProvider));
        }

        // obtain exported function to call
        auto moduleNs = jsg::check(module->GetModuleNamespace()->ToObject(context));
        auto fn = lock.v8Get(moduleNs, wrapped.entrypoint);
        KJ_ASSERT(fn->IsFunction(), "Entrypoint is not a function", wrapped.entrypoint);

        // invoke the function, its result will be binding value
        v8::Local<v8::Value> arg = env.As<v8::Value>();
        value = jsg::check(v8::Function::Cast(*fn)->Call(context, context->Global(), 1, &arg));
      } else {
        KJ_LOG(
            ERROR, "wrapped binding module can't be resolved (internal modules only)", moduleName);
      }
    }
    KJ_CASE_ONEOF(hyperdrive, Global::Hyperdrive) {
      value = lock.wrap(context,
          lock.alloc<api::Hyperdrive>(hyperdrive.subrequestChannel, kj::str(hyperdrive.database),
              kj::str(hyperdrive.user), kj::str(hyperdrive.password), kj::str(hyperdrive.scheme)));
    }
    KJ_CASE_ONEOF(unsafe, Global::UnsafeEval) {
      value = lock.wrap(context, lock.alloc<api::UnsafeEval>());
    }

    KJ_CASE_ONEOF(actorClass, Global::ActorClass) {
      value = lock.wrap(context, lock.alloc<api::DurableObjectClass>(actorClass.channel));
    }

    KJ_CASE_ONEOF(actorClass, Global::LoopbackActorClass) {
      value = lock.wrap(context, lock.alloc<api::LoopbackDurableObjectClass>(actorClass.channel));
    }

    KJ_CASE_ONEOF(workerLoader, Global::WorkerLoader) {
      value = lock.wrap(context,
          lock.alloc<api::WorkerLoader>(
              workerLoader.channel, CompatibilityDateValidation::CODE_VERSION));
    }

    KJ_CASE_ONEOF(_, Global::WorkerdDebugPort) {
      value = lock.wrap(context, lock.alloc<WorkerdDebugPortConnector>());
    }
  }

  return value;
}

void WorkerdApi::compileGlobals(jsg::Lock& lockParam,
    kj::ArrayPtr<const Global> globals,
    v8::Local<v8::Object> target,
    uint32_t ownerId) const {
  TRACE_EVENT("workerd", "WorkerdApi::compileGlobals()");
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);
  lockParam.withinHandleScope([&] {
    auto& featureFlags = *impl->features;

    for (auto& global: globals) {
      lockParam.withinHandleScope([&] {
        // Don't use String's usual TypeHandler here because we want to intern the string.
        auto value =
            createBindingValue(lock, global, featureFlags, ownerId, impl->memoryCacheProvider);
        KJ_ASSERT(!value.IsEmpty(), "global did not produce v8::Value");
        lockParam.v8Set(target, global.name, value);
      });
    }
  });
}

void WorkerdApi::setModuleFallbackCallback(kj::Function<ModuleFallbackCallback>&& callback) const {
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
    KJ_CASE_ONEOF(loopback, Global::LoopbackServiceStub) {
      result.value = loopback.clone();
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
    KJ_CASE_ONEOF(ns, Global::LoopbackEphemeralActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ns, Global::LoopbackDurableActorNamespace) {
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
      result.value = Global::UnsafeEval{};
    }

    KJ_CASE_ONEOF(actorClass, Global::ActorClass) {
      result.value = actorClass.clone();
    }
    KJ_CASE_ONEOF(actorClass, Global::LoopbackActorClass) {
      result.value = actorClass.clone();
    }
    KJ_CASE_ONEOF(workerLoader, Global::WorkerLoader) {
      result.value = workerLoader.clone();
    }
    KJ_CASE_ONEOF(workerdDebugPort, Global::WorkerdDebugPort) {
      result.value = workerdDebugPort.clone();
    }
  }

  return result;
}

const WorkerdApi& WorkerdApi::from(const Worker::Api& api) {
  return kj::downcast<const WorkerdApi>(api);
}

// =======================================================================================

// TODO(soon): These are required for python workers but we don't support those yet
// with the new module registry. Uncomment these when we do.
// namespace {
// static constexpr auto PYTHON_TAR_READER = "export default { }"_kj;

// static const auto bootrapSpecifier = "internal:setup-emscripten"_url;
// static const auto metadataSpecifier = "pyodide-internal:runtime-generated/metadata"_url;
// static const auto artifactsSpecifier = "pyodide-internal:artifacts"_url;
// static const auto internalJaegerSpecifier = "pyodide-internal:internalJaeger"_url;
// static const auto diskCacheSpecifier = "pyodide-internal:disk_cache"_url;
// static const auto limiterSpecifier = "pyodide-internal:limiter"_url;
// static const auto tarReaderSpecifier = "pyodide-internal:packages_tar_reader"_url;
// }  // namespace

kj::Arc<jsg::modules::ModuleRegistry> WorkerdApi::newWorkerdModuleRegistry(
    const jsg::ResolveObserver& observer,
    kj::Maybe<const Worker::Script::ModulesSource&> maybeSource,
    const CompatibilityFlags::Reader& featureFlags,
    const PythonConfig& pythonConfig,
    const jsg::Url& bundleBase,
    capnp::List<config::Extension>::Reader extensions,
    kj::Maybe<kj::String> maybeFallbackService,
    kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts) {

  return newWorkerModuleRegistry<JsgWorkerdIsolate_TypeWrapper>(observer, maybeSource, featureFlags,
      bundleBase,
      [&](jsg::modules::ModuleRegistry::Builder& builder, IsPythonWorker isPythonWorker) {
    // TODO(later): The new module registry should eventually support python workers
    // as well, but for now we forbid it. There are a number of nuances to python workers
    // and modules that need to be worked out.
    KJ_REQUIRE(!isPythonWorker, "Python workers are not supported with the new module registry");
    // if (isPythonWorker) {
    //   using namespace api::pyodide;

    //   // It's not possible to have a python worker without a source bundle.
    //   auto& source = KJ_ASSERT_NONNULL(maybeSource);

    //   // To support python workers we create two modules bundles, one BUILTIN
    //   // and the other BUILTIN_ONLY. The BUILTIN bundle contains support modules
    //   // that need to be importable by the python worker bootstrap module (which
    //   // is added to the BUNDLE modules). The BUILTIN_ONLY bundle contains support
    //   // modules that are used by the BUILTIN modules and are not intended to be
    //   // accessible from the worker itself.

    //   // Inject metadata that the entrypoint module will read.
    //   auto pythonRelease = KJ_ASSERT_NONNULL(getPythonSnapshotRelease(featureFlags));
    //   auto version = getPythonBundleName(pythonRelease);
    //   auto bundle = retrievePyodideBundle(pythonConfig, version);

    //   // We end up adding modules from the bundle twice, once to get BUILTIN modules
    //   // and again to get the BUILTIN_ONLY modules. These end up in two different
    //   // module bundles.
    //   jsg::modules::ModuleBundle::BuiltinBuilder pyodideSdkBuilder;

    //   // There are two bundles that are relevant here, PYODIDE_BUNDLE, which is
    //   // fixed and contains compiled-in modules, and the bundle that is fetched
    //   // that contains the more dynamic implementation details. We have to process
    //   // both.
    //   jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideSdkBuilder, PYODIDE_BUNDLE);
    //   jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideSdkBuilder, bundle);
    //   builder.add(pyodideSdkBuilder.finish());

    //   jsg::modules::ModuleBundle::BuiltinBuilder pyodideBundleBuilder(
    //       jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

    //   jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideBundleBuilder, PYODIDE_BUNDLE);
    //   jsg::modules::ModuleBundle::getBuiltInBundleFromCapnp(pyodideBundleBuilder, bundle);

    //   pyodideBundleBuilder.addSynthetic(bootrapSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<api::pyodide::SetupEmscripten,
    //           JsgWorkerdIsolate_TypeWrapper>(
    //           [bundle = capnp::clone(bundle)](
    //               jsg::Lock& js) mutable -> jsg::Ref<api::pyodide::SetupEmscripten> {
    //     auto emscriptenRuntime = api::pyodide::EmscriptenRuntime::initialize(js, true, *bundle);
    //     return js.alloc<api::pyodide::SetupEmscripten>(kj::mv(emscriptenRuntime));
    //   }));

    //   pyodideBundleBuilder.addEsm(tarReaderSpecifier, PYTHON_TAR_READER);

    //   api::pyodide::CreateBaselineSnapshot createBaselineSnapshot(
    //       pythonConfig.createBaselineSnapshot);
    //   api::pyodide::SnapshotToDisk snapshotToDisk(
    //       pythonConfig.createSnapshot || createBaselineSnapshot);
    //   auto maybeSnapshot = tryGetMetadataSnapshot(pythonConfig, snapshotToDisk);
    //   auto state = workerd::modules::python::createPyodideMetadataState(source,
    //       api::pyodide::IsWorkerd::YES, api::pyodide::IsTracing::NO, snapshotToDisk,
    //       createBaselineSnapshot, pythonRelease, kj::mv(maybeSnapshot), featureFlags);

    //   pyodideBundleBuilder.addSynthetic(metadataSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<api::pyodide::PyodideMetadataReader,
    //           JsgWorkerdIsolate_TypeWrapper>(
    //           [state = kj::mv(state)](
    //               jsg::Lock& js) mutable -> jsg::Ref<api::pyodide::PyodideMetadataReader> {
    //     // The ModuleRegistry may be shared across multiple isolates and workers.
    //     // We need to clone the PyodideMetadataReader::State for each instance
    //     // that is evaluated. Typically this is only once per python worker
    //     // but could be more in the future.
    //     return js.alloc<PyodideMetadataReader>(state->clone());
    //   }));
    //   // Inject artifact bundler.
    //   pyodideBundleBuilder.addSynthetic(artifactsSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<ArtifactBundler,
    //           JsgWorkerdIsolate_TypeWrapper>(
    //           [](jsg::Lock& js) mutable -> jsg::Ref<ArtifactBundler> {
    //     return js.alloc<ArtifactBundler>(ArtifactBundler::makeDisabledBundler());
    //   }));
    //   // Inject jaeger internal tracer in a disabled state (we don't have a use for it in workerd)
    //   pyodideBundleBuilder.addSynthetic(internalJaegerSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<DisabledInternalJaeger,
    //           JsgWorkerdIsolate_TypeWrapper>(
    //           [](jsg::Lock& js) mutable -> jsg::Ref<DisabledInternalJaeger> {
    //     return DisabledInternalJaeger::create(js);
    //   }));
    //   // Inject disk cache module
    //   pyodideBundleBuilder.addSynthetic(diskCacheSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<DiskCache, JsgWorkerdIsolate_TypeWrapper>(
    //           [&packageDiskCacheRoot = pythonConfig.packageDiskCacheRoot](jsg::Lock& js) mutable
    //           -> jsg::Ref<DiskCache> { return js.alloc<DiskCache>(packageDiskCacheRoot); }));
    //   // Inject a (disabled) SimplePythonLimiter
    //   pyodideBundleBuilder.addSynthetic(limiterSpecifier,
    //       jsg::modules::Module::newJsgObjectModuleHandler<SimplePythonLimiter,
    //           JsgWorkerdIsolate_TypeWrapper>(
    //           [](jsg::Lock& js) mutable -> jsg::Ref<SimplePythonLimiter> {
    //     return SimplePythonLimiter::makeDisabled(js);
    //   }));

    //   builder.add(pyodideBundleBuilder.finish());
    // }

    // Handle extensions (extensions are a workerd-specific concept)
    jsg::modules::ModuleBundle::BuiltinBuilder publicExtensionsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN);
    jsg::modules::ModuleBundle::BuiltinBuilder privateExtensionsBuilder(
        jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);

    for (auto extension: extensions) {
      for (auto module: extension.getModules()) {
        KJ_IF_SOME(url, jsg::Url::tryParse(module.getName())) {
          if (module.getInternal()) {
            privateExtensionsBuilder.addEsm(url, module.getEsModule().asArray());
          } else {
            publicExtensionsBuilder.addEsm(url, module.getEsModule().asArray());
          }
        } else {
          KJ_LOG(WARNING, "Ignoring extension module with invalid name", module.getName());
        }
      }
    }

    builder.add(publicExtensionsBuilder.finish());
    builder.add(privateExtensionsBuilder.finish());

    // If we have a fallback service configured, add the fallback bundle.
    // The fallback bundle is used only in workerd local development mode.
    // If a module is not found in the static bundles, a registry that is
    // configured to use the fallback will send a request to the fallback
    // service to try resolving.
    KJ_IF_SOME(fallbackService, maybeFallbackService) {
      auto fallbackClient =
          kj::heap<workerd::fallback::FallbackServiceClient>(kj::str(fallbackService));
      builder.add(jsg::modules::ModuleBundle::newFallbackBundle(
          [client = kj::mv(fallbackClient), featureFlags](
              const jsg::modules::ResolveContext& context) mutable
          -> kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>> {
        auto normalizedSpecifier = kj::str(context.normalizedSpecifier.getHref());
        auto referrer = kj::str(context.referrerNormalizedSpecifier.getHref());
        KJ_IF_SOME(resolved,
            client->tryResolve(workerd::fallback::Version::V2,
                workerd::fallback::ImportType::IMPORT, normalizedSpecifier,
                context.rawSpecifier.orDefault(nullptr), referrer, context.attributes)) {
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(str, kj::String) {
              // The fallback service returned an alternative specifier.
              // The resolution must start over with the new specifier.
              return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(kj::mv(str));
            }
            KJ_CASE_ONEOF(def, kj::Own<server::config::Worker::Module::Reader>) {
              // The fallback service returned a module definition.
              // We need to convert that into a Module instance.
              auto mod = readModuleConf(*def, featureFlags, kj::none);
              KJ_IF_SOME(id, jsg::Url::tryParse(mod.name)) {
                // Note that unlike the regular case, the module content returned
                // by the fallback service is not guaranteed to be memory-resident.
                // We need to copy the content into a heap-allocated arrays and
                // make sure those stay alive while the Module is alive.
                KJ_SWITCH_ONEOF(mod.content) {
                  KJ_CASE_ONEOF(content, Worker::Script::EsModule) {
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newEsm(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            kj::heapArray<const char>(content.body)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::TextModule) {
                    auto ownedData = kj::str(content.body);
                    auto ptr = ownedData.asPtr();
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newSynthetic(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            jsg::modules::Module::newTextModuleHandler(ptr))
                            .attach(kj::mv(ownedData)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::DataModule) {
                    auto ownedData = kj::heapArray<uint8_t>(content.body);
                    auto ptr = ownedData.asPtr();
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newSynthetic(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            jsg::modules::Module::newDataModuleHandler(ptr))
                            .attach(kj::mv(ownedData)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::WasmModule) {
                    auto ownedData = kj::heapArray<uint8_t>(content.body);
                    auto ptr = ownedData.asPtr();
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newSynthetic(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            jsg::modules::Module::newWasmModuleHandler(ptr))
                            .attach(kj::mv(ownedData)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::JsonModule) {
                    auto ownedData = kj::heapArray<const char>(content.body);
                    auto ptr = ownedData.asPtr();
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newSynthetic(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            jsg::modules::Module::newJsonModuleHandler(ptr))
                            .attach(kj::mv(ownedData)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::CommonJsModule) {
                    auto ownedData = kj::str(content.body);
                    auto ptr = ownedData.asPtr();
                    kj::ArrayPtr<const kj::StringPtr> named;
                    KJ_IF_SOME(n, content.namedExports) {
                      named = n;
                    }
                    return kj::Maybe<kj::OneOf<kj::String, kj::Own<jsg::modules::Module>>>(
                        jsg::modules::Module::newSynthetic(kj::mv(id),
                            jsg::modules::Module::Type::FALLBACK,
                            jsg::modules::Module::newCjsStyleModuleHandler<
                                api::CommonJsModuleContext, JsgWorkerdIsolate_TypeWrapper>(
                                ptr, mod.name),
              KJ_MAP(name, named) {
                      return kj::str(name);
                    }).attach(kj::mv(ownedData)));
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::PythonModule) {
                    // Python modules are not supported.in fallback
                    KJ_LOG(WARNING, "Fallback service returned a Python module");
                    return kj::none;
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::PythonRequirement) {
                    // Python requirement modules are not supported.in fallback
                    KJ_LOG(WARNING, "Fallback service returned a Python requirement");
                    return kj::none;
                  }
                  KJ_CASE_ONEOF(content, Worker::Script::CapnpModule) {
                    // Capnp modules are not supported.in fallback
                    KJ_LOG(WARNING, "Fallback service returned a Capnp module");
                    return kj::none;
                  }
                }
                KJ_UNREACHABLE;
              }
              KJ_LOG(WARNING, "Fallback service returned an invalid id");
              return kj::none;
            }
          }
        }
        return kj::none;
      }));
    }
  }, jsg::modules::ModuleRegistry::Builder::Options::ALLOW_FALLBACK);
}

kj::Own<rpc::ActorStorage::Stage::Server> newEmptyReadOnlyActorStorage() {
  return kj::heap<EmptyReadOnlyActorStorageImpl>();
}

}  // namespace workerd::server
