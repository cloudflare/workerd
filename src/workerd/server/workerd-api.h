// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker.h>
#include <workerd/api/analytics-engine.h>
#include <workerd/server/workerd.capnp.h>

namespace workerd::server {

class WorkerdApiIsolate final: public Worker::ApiIsolate {
  // An ApiIsolate implementation with support for all the APIs supported by the OSS runtime.
public:
  WorkerdApiIsolate(jsg::V8System& v8System,
      CompatibilityFlags::Reader features,
      IsolateLimitEnforcer& limitEnforcer);
  ~WorkerdApiIsolate() noexcept(false);

  kj::Own<jsg::Lock> lock() const override;
  CompatibilityFlags::Reader getFeatureFlags() const override;
  jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(jsg::Lock& lock) const override;
  jsg::Dict<NamedExport> unwrapExports(
      jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const override;
  const jsg::TypeHandler<ErrorInterface>&
      getErrorInterfaceTypeHandler(jsg::Lock& lock) const override;

  static Worker::Script::Source extractSource(kj::StringPtr name,
      config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter);

  struct Global {
    // A pipeline-level binding.
    //
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
    kj::String name;
    kj::OneOf<Json, Fetcher, KvNamespace, R2Bucket, R2Admin, CryptoKey, EphemeralActorNamespace,
              DurableActorNamespace, kj::String, kj::Array<byte>> value;

    Global clone() const;
  };

  void compileGlobals(jsg::Lock& lock,
                      kj::ArrayPtr<const Global> globals,
                      v8::Local<v8::Object> target,
                      uint32_t ownerId) const;

private:
  struct Impl;
  kj::Own<Impl> impl;

  kj::Array<Worker::Script::CompiledGlobal> compileScriptGlobals(
      jsg::Lock& lock, config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter) const;
  kj::Own<jsg::ModuleRegistry> compileModules(
      jsg::Lock& lock, config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter) const;
};

}  // namespace workerd::server
