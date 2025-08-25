// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "workerd/api/pyodide/pyodide.h"

#include <workerd/io/worker-fs.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd {
namespace api {
namespace pyodide {
struct PythonConfig;
}
}  // namespace api
}  // namespace workerd
namespace workerd {
namespace jsg {
class V8System;
}
}  // namespace workerd

namespace workerd::api {
class MemoryCacheProvider;
}

namespace workerd::server {

using api::pyodide::PythonConfig;

// A Worker::Api implementation with support for all the APIs supported by the OSS runtime.
class WorkerdApi final: public Worker::Api {
 public:
  WorkerdApi(jsg::V8System& v8System,
      CompatibilityFlags::Reader features,
      capnp::List<config::Extension>::Reader extensions,
      v8::Isolate::CreateParams createParams,
      v8::IsolateGroup group,
      kj::Own<JsgIsolateObserver> observer,
      api::MemoryCacheProvider& memoryCacheProvider,
      const PythonConfig& pythonConfig,
      kj::Maybe<kj::Own<jsg::modules::ModuleRegistry>> newModuleRegistry);
  ~WorkerdApi() noexcept(false);

  static const WorkerdApi& from(const Worker::Api&);

  kj::Own<jsg::Lock> lock(jsg::V8StackScope& stackScope) const override;
  CompatibilityFlags::Reader getFeatureFlags() const override;
  jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(jsg::Lock& lock) const override;
  jsg::Dict<NamedExport> unwrapExports(
      jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const override;
  NamedExport unwrapExport(jsg::Lock& lock, v8::Local<v8::Value> exportVal) const override;
  EntrypointClasses getEntrypointClasses(jsg::Lock& lock) const override;
  const jsg::TypeHandler<ErrorInterface>& getErrorInterfaceTypeHandler(
      jsg::Lock& lock) const override;
  const jsg::TypeHandler<api::QueueExportedHandler>& getQueueTypeHandler(
      jsg::Lock& lock) const override;
  jsg::JsObject wrapExecutionContext(
      jsg::Lock& lock, jsg::Ref<api::ExecutionContext> ref) const override;
  const jsg::IsolateObserver& getObserver() const override;
  void setIsolateObserver(IsolateObserver&) override;

  static Worker::Script::Source extractSource(kj::StringPtr name,
      config::Worker::Reader conf,
      CompatibilityFlags::Reader featureFlags,
      Worker::ValidationErrorReporter& errorReporter);

  void compileModules(jsg::Lock& lock,
      const Worker::Script::ModulesSource& source,
      const Worker::Isolate& isolate,
      kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts,
      SpanParent parentSpan) const override;

  kj::Array<Worker::Script::CompiledGlobal> compileServiceWorkerGlobals(jsg::Lock& lock,
      const Worker::Script::ScriptSource& source,
      const Worker::Isolate& isolate) const override;

  // A pipeline-level binding.
  struct Global {
    // TODO(cleanup): Get rid of this and just load from config.Worker.bindings capnp structure
    //   directly.

    struct Json {
      kj::String text;

      Json clone() const {
        return Json{.text = kj::str(text)};
      }
    };
    struct Fetcher {
      uint channel;
      bool requiresHost;
      bool isInHouse;

      Fetcher clone() const {
        return *this;
      }
    };
    struct LoopbackServiceStub {
      uint channel;

      LoopbackServiceStub clone() const {
        return *this;
      }
    };
    struct KvNamespace {
      uint subrequestChannel;

      KvNamespace clone() const {
        return *this;
      }
    };
    struct R2Bucket {
      uint subrequestChannel;

      R2Bucket clone() const {
        return *this;
      }
    };
    struct R2Admin {
      uint subrequestChannel;

      R2Admin clone() const {
        return *this;
      }
    };
    struct QueueBinding {
      uint subrequestChannel;

      QueueBinding clone() const {
        return *this;
      }
    };
    struct CryptoKey {
      kj::String format;
      kj::OneOf<kj::Array<byte>, Json> keyData;
      Json algorithm;
      bool extractable;
      kj::Array<kj::String> usages;

      CryptoKey clone() const {
        decltype(keyData) clonedKeyData;
        KJ_SWITCH_ONEOF(keyData) {
          KJ_CASE_ONEOF(bytes, kj::Array<byte>) {
            clonedKeyData = kj::heapArray(bytes.asPtr());
          }
          KJ_CASE_ONEOF(json, Json) {
            clonedKeyData = json.clone();
          }
        }
        return CryptoKey{
          .format = kj::str(format),
          .keyData = kj::mv(clonedKeyData),
          .algorithm = algorithm.clone(),
          .extractable = extractable,
          .usages = KJ_MAP(s, usages) { return kj::str(s); },
        };
      }
    };

    struct MemoryCache {
      kj::Maybe<kj::String> cacheId = kj::none;
      uint32_t maxKeys;
      uint32_t maxValueSize;
      uint64_t maxTotalValueSize;

      MemoryCache clone() const {
        return MemoryCache{
          .cacheId = cacheId.map([](auto& id) { return kj::str(id); }),
          .maxKeys = maxKeys,
          .maxValueSize = maxValueSize,
          .maxTotalValueSize = maxTotalValueSize,
        };
      }
    };

    struct EphemeralActorNamespace {
      uint actorChannel;

      EphemeralActorNamespace clone() const {
        return *this;
      }
    };
    struct LoopbackEphemeralActorNamespace {
      uint actorChannel;
      uint classChannel;

      LoopbackEphemeralActorNamespace clone() const {
        return *this;
      }
    };
    struct DurableActorNamespace {
      uint actorChannel;
      kj::StringPtr uniqueKey;

      DurableActorNamespace clone() const {
        return *this;
      }
    };
    struct LoopbackDurableActorNamespace {
      uint actorChannel;
      kj::StringPtr uniqueKey;
      uint classChannel;

      LoopbackDurableActorNamespace clone() const {
        return *this;
      }
    };
    struct Wrapped {
      // data carrier for configured WrappedBinding
      kj::String moduleName;
      kj::String entrypoint;
      kj::Array<Global> innerBindings;

      Wrapped clone() const {
        return Wrapped{.moduleName = kj::str(moduleName),
          .entrypoint = kj::str(entrypoint),
          .innerBindings = KJ_MAP(b, innerBindings) { return b.clone(); }};
      }
    };
    struct AnalyticsEngine {
      uint subrequestChannel;
      kj::String dataset;
      int64_t version;
      AnalyticsEngine clone() const {
        return AnalyticsEngine{
          .subrequestChannel = subrequestChannel, .dataset = kj::str(dataset), .version = version};
      }
    };
    struct Hyperdrive {
      uint subrequestChannel;
      kj::String database;
      kj::String user;
      kj::String password;
      kj::String scheme;

      Hyperdrive clone() const {
        return Hyperdrive{
          .subrequestChannel = subrequestChannel,
          .database = kj::str(database),
          .user = kj::str(user),
          .password = kj::str(password),
          .scheme = kj::str(scheme),
        };
      }
    };
    struct UnsafeEval {};

    struct ActorClass {
      uint channel;

      ActorClass clone() const {
        return *this;
      }
    };

    struct LoopbackActorClass {
      uint channel;

      LoopbackActorClass clone() const {
        return *this;
      }
    };

    struct WorkerLoader {
      uint channel;

      WorkerLoader clone() const {
        return *this;
      }
    };

    kj::String name;
    kj::OneOf<Json,
        Fetcher,
        LoopbackServiceStub,
        KvNamespace,
        R2Bucket,
        R2Admin,
        CryptoKey,
        EphemeralActorNamespace,
        LoopbackEphemeralActorNamespace,
        DurableActorNamespace,
        LoopbackDurableActorNamespace,
        QueueBinding,
        kj::String,
        kj::Array<byte>,
        Wrapped,
        AnalyticsEngine,
        Hyperdrive,
        UnsafeEval,
        MemoryCache,
        ActorClass,
        LoopbackActorClass,
        WorkerLoader>
        value;

    Global clone() const;
  };

  void compileGlobals(jsg::Lock& lock,
      kj::ArrayPtr<const Global> globals,
      v8::Local<v8::Object> target,
      uint32_t ownerId) const;

  // Part of the original module registry API.
  static kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileModule(jsg::Lock& js,
      config::Worker::Module::Reader conf,
      jsg::CompilationObserver& observer,
      CompatibilityFlags::Reader featureFlags);
  static kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileModule(jsg::Lock& js,
      const Worker::Script::Module& module,
      jsg::CompilationObserver& observer,
      CompatibilityFlags::Reader featureFlags);

  // Convert a module definition from workerd config to a Worker::Script::Module (which may contain
  // string pointers into the config).
  static Worker::Script::Module readModuleConf(config::Worker::Module::Reader conf,
      CompatibilityFlags::Reader featureFlags,
      kj::Maybe<Worker::ValidationErrorReporter&> errorReporter = kj::none);

  using ModuleFallbackCallback = Worker::Api::ModuleFallbackCallback;
  void setModuleFallbackCallback(kj::Function<ModuleFallbackCallback>&& callback) const override;

  // Create the ModuleRegistry instance for the worker.
  static kj::Own<jsg::modules::ModuleRegistry> initializeBundleModuleRegistry(
      const jsg::ResolveObserver& resolveObserver,
      kj::Maybe<const Worker::Script::ModulesSource&> source,
      const CompatibilityFlags::Reader& featureFlags,
      const PythonConfig& pythonConfig,
      const jsg::Url& bundleBase,
      capnp::List<config::Extension>::Reader extensions,
      kj::Maybe<kj::String> fallbackService = kj::none,
      kj::Maybe<kj::Own<api::pyodide::ArtifactBundler_State>> artifacts = kj::none);

 private:
  struct Impl;
  kj::Own<Impl> impl;
};

kj::Array<kj::String> getPythonRequirements(const Worker::Script::ModulesSource& source);

// Helper method for defining actor storage server treating all reads as empty, defined here to be
// used by test-fixture and server.
kj::Own<rpc::ActorStorage::Stage::Server> newEmptyReadOnlyActorStorage();

}  // namespace workerd::server
