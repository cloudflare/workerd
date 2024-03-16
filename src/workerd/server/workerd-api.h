// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/jsg/setup.h>

namespace workerd::api {
class MemoryCacheProvider;
}

namespace workerd::server {

// A Worker::Api implementation with support for all the APIs supported by the OSS runtime.
class WorkerdApi final: public Worker::Api {
public:
  WorkerdApi(jsg::V8System& v8System,
      CompatibilityFlags::Reader features,
      IsolateLimitEnforcer& limitEnforcer,
      kj::Own<jsg::IsolateObserver> observer,
      api::MemoryCacheProvider& memoryCacheProvider,
      kj::Maybe<kj::Own<const kj::Directory>> pyodideCacheRoot);
  ~WorkerdApi() noexcept(false);

  static const WorkerdApi& from(const Worker::Api&);

  kj::Own<jsg::Lock> lock(jsg::V8StackScope& stackScope) const override;
  CompatibilityFlags::Reader getFeatureFlags() const override;
  jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(jsg::Lock& lock) const override;
  jsg::Dict<NamedExport> unwrapExports(
      jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const override;
  EntrypointClasses getEntrypointClasses(jsg::Lock& lock) const override;
  const jsg::TypeHandler<ErrorInterface>&
      getErrorInterfaceTypeHandler(jsg::Lock& lock) const override;
  const jsg::TypeHandler<api::QueueExportedHandler>& getQueueTypeHandler(
      jsg::Lock& lock) const override;
  jsg::JsObject wrapExecutionContext(
      jsg::Lock& lock, jsg::Ref<api::ExecutionContext> ref) const override;

  static Worker::Script::Source extractSource(kj::StringPtr name,
      config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter,
      capnp::List<config::Extension>::Reader extensions);

  // A pipeline-level binding.
  struct Global {
    // TODO(cleanup): Get rid of this and just load from config.Worker.bindings capnp structure
    //   directly.

    struct Json {
      kj::String text;

      Json clone() const {
        return Json { .text = kj::str(text) };
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
        return CryptoKey {
          .format = kj::str(format),
          .keyData = kj::mv(clonedKeyData),
          .algorithm = algorithm.clone(),
          .extractable = extractable,
          .usages = KJ_MAP(s, usages) {
            return kj::str(s);
          },
        };
      }
    };

    struct MemoryCache {
      kj::Maybe<kj::String> cacheId = kj::none;
      uint32_t maxKeys;
      uint32_t maxValueSize;
      uint64_t maxTotalValueSize;

      MemoryCache clone() const {
        return MemoryCache {
          .cacheId = cacheId.map([](auto& id) { return kj::str(id);}),
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
    struct DurableActorNamespace {
      uint actorChannel;
      kj::StringPtr uniqueKey;

      DurableActorNamespace clone() const {
        return *this;
      }
    };
    struct Wrapped {
      // data carrier for configured WrappedBinding
      kj::String moduleName;
      kj::String entrypoint;
      kj::Array<Global> innerBindings;

      Wrapped clone() const {
        return Wrapped {
          .moduleName = kj::str(moduleName),
          .entrypoint = kj::str(entrypoint),
          .innerBindings = KJ_MAP(b, innerBindings) { return b.clone(); }
        };
      }
    };
    struct AnalyticsEngine {
      uint subrequestChannel;
      kj::String dataset;
      int64_t version;
      AnalyticsEngine clone() const {
        return AnalyticsEngine {
          .subrequestChannel = subrequestChannel,
          .dataset = kj::str(dataset),
          .version = version
        };
      }
    };
    struct Hyperdrive {
      uint subrequestChannel;
      kj::String database;
      kj::String user;
      kj::String password;
      kj::String scheme;

      Hyperdrive clone() const {
        return Hyperdrive {
          .subrequestChannel = subrequestChannel,
          .database = kj::str(database),
          .user = kj::str(user),
          .password = kj::str(password),
          .scheme = kj::str(scheme),
        };
      }
    };
    struct UnsafeEval {};
    kj::String name;
    kj::OneOf<Json, Fetcher, KvNamespace, R2Bucket, R2Admin, CryptoKey, EphemeralActorNamespace,
              DurableActorNamespace, QueueBinding, kj::String, kj::Array<byte>, Wrapped,
              AnalyticsEngine, Hyperdrive, UnsafeEval, MemoryCache> value;

    Global clone() const;
  };

  void compileGlobals(jsg::Lock& lock,
                      kj::ArrayPtr<const Global> globals,
                      v8::Local<v8::Object> target,
                      uint32_t ownerId) const;

  static kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileModule(
      jsg::Lock& js,
      config::Worker::Module::Reader conf,
      jsg::CompilationObserver& observer,
      CompatibilityFlags::Reader featureFlags);

  using ModuleFallbackCallback = Worker::Api::ModuleFallbackCallback;
  void setModuleFallbackCallback(
       kj::Function<ModuleFallbackCallback>&& callback) const override;

private:
  struct Impl;
  kj::Own<Impl> impl;

  kj::Array<Worker::Script::CompiledGlobal> compileScriptGlobals(
      jsg::Lock& lock,
      config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter,
      const jsg::CompilationObserver& observer) const;

  void compileModules(
      jsg::Lock& lock,
      config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter,
      capnp::List<config::Extension>::Reader extensions) const;
};

}  // namespace workerd::server
