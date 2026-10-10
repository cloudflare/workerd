// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/http.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/io/worker-fs.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/snapshot.h>
#include <workerd/server/compiled-bindings.capnp.h>
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
namespace modules {
class ModuleRegistry;
}
}  // namespace jsg
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
      kj::Array<Worker::Api::InboundListener> inboundListeners = nullptr,
      kj::Maybe<jsg::SnapshotConfig> snapshotConfig = kj::none);
  ~WorkerdApi() noexcept(false);

  static const WorkerdApi& from(const Worker::Api&);

  kj::Own<jsg::Lock> lock(jsg::V8StackScope& stackScope) const override;
  CompatibilityFlags::Reader getFeatureFlags() const override;
  kj::ArrayPtr<const Worker::Api::InboundListener> getInboundListeners() const override;
  jsg::JsContext<api::ServiceWorkerGlobalScope> newContext(
      jsg::Lock& lock, Worker::Api::NewContextOptions options = {}) const override;
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

  // Sets each of `globals`, a worker's bindings as the server interpreted them, on `target`.
  void compileGlobals(
      jsg::Lock& lock, capnp::List<Global>::Reader globals, v8::Local<v8::Object> target) const;

  // Part of the original module registry API.
  static kj::Maybe<jsg::ModuleRegistry::ModuleInfo> tryCompileModule(jsg::Lock& js,
      config::Worker::Module::Reader conf,
      const jsg::CompilationObserver& observer,
      CompatibilityFlags::Reader featureFlags);

  // Convert a module definition from workerd config to a Worker::Script::Module (which may contain
  // string pointers into the config).
  static Worker::Script::Module readModuleConf(config::Worker::Module::Reader conf,
      CompatibilityFlags::Reader featureFlags,
      kj::Maybe<Worker::ValidationErrorReporter&> errorReporter = kj::none);

  using ModuleFallbackCallback = Worker::Api::ModuleFallbackCallback;
  void setModuleFallbackCallback(kj::Function<ModuleFallbackCallback>&& callback) const override;

  // Create the ModuleRegistry instance for the worker.
  static kj::Arc<jsg::modules::ModuleRegistry> newWorkerdModuleRegistry(
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
  kj::Array<Worker::Api::InboundListener> inboundListeners;
};

// An ActorStorage implementation which will always respond to reads as if the state is empty,
// and will fail any writes. Defined here to be used by test-fixture and server.
kj::Own<rpc::ActorStorage::Stage::Server> newEmptyReadOnlyActorStorage();

}  // namespace workerd::server
