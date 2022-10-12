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
#include <workerd/api/crypto-impl.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/kv.h>
#include <workerd/api/r2-admin.h>
#include <workerd/api/urlpattern.h>
#include <workerd/util/thread-scopes.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

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
  EW_KV_ISOLATE_TYPES,
  EW_R2_PUBLIC_BETA_ADMIN_ISOLATE_TYPES,
  EW_R2_PUBLIC_BETA_ISOLATE_TYPES,
  EW_SCHEDULED_ISOLATE_TYPES,
  EW_STREAMS_ISOLATE_TYPES,
  EW_TRACE_ISOLATE_TYPES,
  EW_URL_ISOLATE_TYPES,
  EW_URL_STANDARD_ISOLATE_TYPES,
  EW_URLPATTERN_ISOLATE_TYPES,
  EW_WEBSOCKET_ISOLATE_TYPES,

  jsg::TypeWrapperExtension<PromiseWrapper>,
  jsg::InjectConfiguration<CompatibilityFlags::Reader>,
  Worker::ApiIsolate::ErrorInterface,
  jsg::CommonJsModuleObject,
  jsg::CommonJsModuleContext);

struct WorkerdApiIsolate::Impl {
  kj::Own<CompatibilityFlags::Reader> features;
  JsgWorkerdIsolate jsgIsolate;

  class Configuration {
  public:
    Configuration(Impl& impl)
        : features(*impl.features),
          jsgConfig(jsg::JsgConfig {
            .noSubstituteNull = features.getNoSubstituteNull(),
          }) {}
    operator const CompatibilityFlags::Reader() const { return features; }
    operator const jsg::JsgConfig&() const { return jsgConfig; }

  private:
    CompatibilityFlags::Reader& features;
    jsg::JsgConfig jsgConfig;
  };

  Impl(jsg::V8System& v8System,
       CompatibilityFlags::Reader featuresParam,
       IsolateLimitEnforcer& limitEnforcer)
      : features(capnp::clone(featuresParam)),
        jsgIsolate(v8System, Configuration(*this), limitEnforcer.getCreateParams()) {}

  static v8::Local<v8::String> compileTextGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Text::Reader reader) {
    return lock.wrapNoContext(reader);
  };

  static v8::Local<v8::ArrayBuffer> compileDataGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Data::Reader reader) {
    return lock.wrapNoContext(kj::heapArray(reader));
  };

  static v8::Local<v8::WasmModuleObject> compileWasmGlobal(
      JsgWorkerdIsolate::Lock& lock, capnp::Data::Reader reader) {
    lock.setAllowEval(true);
    KJ_DEFER(lock.setAllowEval(false));

    // Allow Wasm compilation to spawn a background thread for tier-up, i.e. recompiling
    // Wasm with optimizations in the background. Otherwise Wasm startup is way too slow.
    // Until tier-up finishes, requests will be handled using Liftoff-generated code, which
    // compiles fast but runs slower.
    AllowV8BackgroundThreadsScope scope;

    return jsg::compileWasmModule(lock, reader);
  };

  static v8::Local<v8::Value> compileJsonGlobal(JsgWorkerdIsolate::Lock& lock,
      capnp::Text::Reader reader) {
    return jsg::check(v8::JSON::Parse(
        lock.v8Isolate->GetCurrentContext(),
        lock.wrapNoContext(reader)));
  };

};

WorkerdApiIsolate::WorkerdApiIsolate(jsg::V8System& v8System,
    CompatibilityFlags::Reader features,
    IsolateLimitEnforcer& limitEnforcer)
    : impl(kj::heap<Impl>(v8System, features, limitEnforcer)) {}
WorkerdApiIsolate::~WorkerdApiIsolate() noexcept(false) {}

kj::Own<jsg::Lock> WorkerdApiIsolate::lock() const {
  return kj::heap<JsgWorkerdIsolate::Lock>(impl->jsgIsolate);
}
CompatibilityFlags::Reader WorkerdApiIsolate::getFeatureFlags() const {
  return *impl->features;
}
jsg::JsContext<api::ServiceWorkerGlobalScope>
    WorkerdApiIsolate::newContext(jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock)
      .newContext<api::ServiceWorkerGlobalScope>(lock.v8Isolate);
}
jsg::Dict<NamedExport> WorkerdApiIsolate::unwrapExports(
    jsg::Lock& lock, v8::Local<v8::Value> moduleNamespace) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock)
      .unwrap<jsg::Dict<NamedExport>>(lock.v8Isolate->GetCurrentContext(), moduleNamespace);
}
const jsg::TypeHandler<Worker::ApiIsolate::ErrorInterface>&
    WorkerdApiIsolate::getErrorInterfaceTypeHandler(jsg::Lock& lock) const {
  return kj::downcast<JsgWorkerdIsolate::Lock>(lock).getTypeHandler<ErrorInterface>();
}

Worker::Script::Source WorkerdApiIsolate::extractSource(config::Worker::Reader conf,
    Worker::ValidationErrorReporter& errorReporter) {
  switch (conf.which()) {
    case config::Worker::MODULES: {
      auto modules = conf.getModules();
      if (modules.size() == 0) {
        errorReporter.addError(kj::str("Modules list cannot be empty."));
        goto invalid;
      }

      return Worker::Script::ModulesSource {
        modules[0].getName(),
        [conf,&errorReporter](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate) {
          return kj::downcast<const WorkerdApiIsolate>(apiIsolate)
              .compileModules(lock, conf, errorReporter);
        }
      };
    }
    case config::Worker::SERVICE_WORKER_SCRIPT:
      return Worker::Script::ScriptSource {
        conf.getServiceWorkerScript(),
        [conf,&errorReporter](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate) {
          return kj::downcast<const WorkerdApiIsolate>(apiIsolate)
              .compileScriptGlobals(lock, conf, errorReporter);
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
    [](jsg::Lock& lock, const Worker::ApiIsolate& apiIsolate)
        -> kj::Array<Worker::Script::CompiledGlobal> {
      return nullptr;
    }
  };
}

kj::Array<Worker::Script::CompiledGlobal> WorkerdApiIsolate::compileScriptGlobals(
      jsg::Lock& lockParam, config::Worker::Reader conf,
      Worker::ValidationErrorReporter& errorReporter) const {
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
      auto name = jsg::v8StrIntern(lock.v8Isolate, binding.getName());
      auto value = Impl::compileWasmGlobal(lock, binding.getWasmModule());

      compiledGlobals.add(Worker::Script::CompiledGlobal {
        { lock.v8Isolate, name },
        { lock.v8Isolate, value },
      });
    }
  }

  return compiledGlobals.finish();
}

kj::Own<jsg::ModuleRegistry> WorkerdApiIsolate::compileModules(
    jsg::Lock& lockParam, config::Worker::Reader conf,
    Worker::ValidationErrorReporter& errorReporter) const {
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);
  v8::HandleScope scope(lock.v8Isolate);

  auto modules = kj::heap<jsg::ModuleRegistryImpl<JsgWorkerdIsolate_TypeWrapper>>();

  for (auto module: conf.getModules()) {
    auto path = kj::Path::parse(module.getName());

    switch (module.which()) {
      case config::Worker::Module::TEXT: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                nullptr,
                jsg::ModuleRegistry::TextModuleInfo(lock,
                    Impl::compileTextGlobal(lock, module.getText()))));
        break;
      }
      case config::Worker::Module::DATA: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                nullptr,
                jsg::ModuleRegistry::DataModuleInfo(
                    lock,
                    Impl::compileDataGlobal(lock, module.getData()).As<v8::ArrayBuffer>())));
        break;
      }
      case config::Worker::Module::WASM: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                nullptr,
                jsg::ModuleRegistry::WasmModuleInfo(lock,
                    Impl::compileWasmGlobal(lock, module.getWasm()))));
        break;
      }
      case config::Worker::Module::JSON: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                nullptr,
                jsg::ModuleRegistry::JsonModuleInfo(lock,
                    Impl::compileJsonGlobal(lock, module.getJson()))));
        break;
      }
      case config::Worker::Module::ES_MODULE: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                module.getEsModule()));
        break;
      }
      case config::Worker::Module::COMMON_JS_MODULE: {
        modules->add(
            path,
            jsg::ModuleRegistry::ModuleInfo(
                lock,
                module.getName(),
                nullptr,
                jsg::ModuleRegistry::CommonJsModuleInfo(
                    lock,
                    module.getName(),
                    module.getCommonJsModule())));
        break;
      }
      default: {
        KJ_UNREACHABLE;
      }
    }
  }

  jsg::setModulesForResolveCallback<JsgWorkerdIsolate_TypeWrapper>(lock, modules);

  return modules;
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
    JSG_REQUIRE(jurisdiction == nullptr, Error,
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
    return kj::heap<ActorIdImpl>(id, nullptr);
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

    return kj::heap<ActorIdImpl>(id, nullptr);
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

void WorkerdApiIsolate::compileGlobals(
    jsg::Lock& lockParam, kj::ArrayPtr<const Global> globals,
    v8::Local<v8::Object> target,
    uint32_t ownerId) const {
  auto& lock = kj::downcast<JsgWorkerdIsolate::Lock>(lockParam);
  v8::HandleScope scope(lock.v8Isolate);
  auto context = lock.v8Isolate->GetCurrentContext();
  auto& featureFlags = *impl->features;

  for (auto& global: globals) {
    v8::HandleScope scope(lock.v8Isolate);

    // Don't use String's usual TypeHandler here because we want to intern the string.
    auto name = jsg::v8StrIntern(lock.v8Isolate, global.name);

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
        value = lock.wrap(context, jsg::alloc<api::KvNamespace>(ns.subrequestChannel));
      }

      KJ_CASE_ONEOF(r2, Global::R2Bucket) {
        value = lock.wrap(context,
            jsg::alloc<api::public_beta::R2Bucket>(featureFlags, r2.subrequestChannel));
      }

      KJ_CASE_ONEOF(r2a, Global::R2Admin) {
        value = lock.wrap(context,
            jsg::alloc<api::public_beta::R2Admin>(featureFlags, r2a.subrequestChannel));
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

      KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
        value = lock.wrap(context, jsg::alloc<api::ColoLocalActorNamespace>(ns.actorChannel));
      }

      KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
        value = lock.wrap(context, jsg::alloc<api::DurableObjectNamespace>(ns.actorChannel,
            kj::heap<ActorIdFactoryImpl>(ns.uniqueKey)));
      }

      KJ_CASE_ONEOF(text, kj::String) {
        value = lock.wrap(context, kj::mv(text));
      }

      KJ_CASE_ONEOF(data, kj::Array<byte>) {
        value = lock.wrap(context, kj::heapArray(data.asPtr()));
      }
    }

    KJ_ASSERT(!value.IsEmpty(), "global did not produce v8::Value");
    bool setResult = jsg::check(target->Set(context, name, value));

    if (!setResult) {
      // Can this actually happen? What does it mean?
      KJ_LOG(ERROR, "Set() returned false?", global.name);
    }
  }
}

// =======================================================================================

WorkerdApiIsolate::Global WorkerdApiIsolate::Global::clone() const {
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
    KJ_CASE_ONEOF(key, Global::CryptoKey) {
      result.value = key.clone();
    }
    KJ_CASE_ONEOF(ns, Global::EphemeralActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(ns, Global::DurableActorNamespace) {
      result.value = ns.clone();
    }
    KJ_CASE_ONEOF(text, kj::String) {
      result.value = kj::str(text);
    }
    KJ_CASE_ONEOF(data, kj::Array<byte>) {
      result.value = kj::heapArray(data.asPtr());
    }
  }

  return result;
}

}  // namespace workerd::server
