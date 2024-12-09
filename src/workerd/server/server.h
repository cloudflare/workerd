// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/memory-cache.h>
#include <workerd/api/pyodide/pyodide.h>
#include <workerd/io/worker.h>
#include <workerd/server/alarm-scheduler.h>
#include <workerd/server/workerd.capnp.h>
#include <workerd/util/sqlite.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <kj/one-of.h>

namespace kj {
class TlsContext;
}

namespace workerd::jsg {
class V8System;
}

namespace workerd::server {

using api::pyodide::PythonConfig;

// Implements the single-tenant Workers Runtime server / CLI.
//
// The purpose of this class is to implement the core logic independently of the CLI itself,
// in such a way that it can be unit-tested. workerd.c++ implements the CLI wrapper around this.
class Server final: private kj::TaskSet::ErrorHandler {
 public:
  Server(kj::Filesystem& fs,
      kj::Timer& timer,
      kj::Network& network,
      kj::EntropySource& entropySource,
      Worker::ConsoleMode consoleMode,
      kj::Function<void(kj::String)> reportConfigError);
  ~Server() noexcept;

  // Permit experimental features to be used. These features may break backwards compatibility
  // in the future.
  void allowExperimental() {
    experimental = true;
  }

  void overrideSocket(kj::String name, kj::Own<kj::ConnectionReceiver> port) {
    socketOverrides.upsert(kj::mv(name), kj::mv(port));
  }
  void overrideSocket(kj::String name, kj::String addr) {
    socketOverrides.upsert(kj::mv(name), kj::mv(addr));
  }
  void overrideDirectory(kj::String name, kj::String path) {
    directoryOverrides.upsert(kj::mv(name), kj::mv(path));
  }
  void overrideExternal(kj::String name, kj::String addr) {
    externalOverrides.upsert(kj::mv(name), kj::mv(addr));
  }
  void enableInspector(kj::String addr) {
    inspectorOverride = kj::mv(addr);
  }
  void enableControl(uint fd) {
    controlOverride = kj::heap<kj::FdOutputStream>(fd);
  }
  void setPackageDiskCacheRoot(kj::Maybe<kj::Own<const kj::Directory>>&& dkr) {
    pythonConfig.packageDiskCacheRoot = kj::mv(dkr);
  }
  void setPyodideDiskCacheRoot(kj::Maybe<kj::Own<const kj::Directory>>&& dkr) {
    pythonConfig.pyodideDiskCacheRoot = kj::mv(dkr);
  }
  void setPythonCreateSnapshot() {
    pythonConfig.createSnapshot = true;
  }
  void setPythonCreateBaselineSnapshot() {
    pythonConfig.createBaselineSnapshot = true;
  }

  // Runs the server using the given config.
  kj::Promise<void> run(jsg::V8System& v8System,
      config::Config::Reader conf,
      kj::Promise<void> drainWhen = kj::NEVER_DONE);

  // Executes one or more tests. By default, all exported test handlers from all entrypoints to
  // all services in the config are executed. Glob patterns can be specified to match specific
  // service and entrypoint names.
  //
  // The returned promise resolves true if at least one test ran and no tests failed.
  kj::Promise<bool> test(jsg::V8System& v8System,
      config::Config::Reader conf,
      kj::StringPtr servicePattern = "*"_kj,
      kj::StringPtr entrypointPattern = "*"_kj);

  struct Durable {
    kj::String uniqueKey;
    bool isEvictable;
    bool enableSql;
  };
  struct Ephemeral {
    bool isEvictable;
    bool enableSql;
  };
  using ActorConfig = kj::OneOf<Durable, Ephemeral>;

  class InspectorService;
  class InspectorServiceIsolateRegistrar;

 private:
  kj::Filesystem& fs;
  kj::Timer& timer;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Function<void(kj::String)> reportConfigError;
  PythonConfig pythonConfig = PythonConfig{.packageDiskCacheRoot = kj::none,
    .pyodideDiskCacheRoot = kj::none,
    .createSnapshot = false,
    .createBaselineSnapshot = false};

  bool experimental = false;

  Worker::ConsoleMode consoleMode;

  kj::Own<api::MemoryCacheProvider> memoryCacheProvider;

  kj::HashMap<kj::String, kj::OneOf<kj::String, kj::Own<kj::ConnectionReceiver>>> socketOverrides;
  kj::HashMap<kj::String, kj::String> directoryOverrides;

  // Overrides from the command line.
  //
  // String overrides are left as strings rather than parsed by the caller in order to reuse the
  // code that parses strings from the config file.
  kj::HashMap<kj::String, kj::String> externalOverrides;

  kj::Maybe<kj::String> inspectorOverride;
  kj::Maybe<kj::Own<InspectorServiceIsolateRegistrar>> inspectorIsolateRegistrar;
  kj::Maybe<kj::Own<kj::FdOutputStream>> controlOverride;

  struct GlobalContext;
  // General context needed to construct workers. Initilaized early in run().
  kj::Own<GlobalContext> globalContext;

  class Service;
  kj::Own<Service> invalidConfigServiceSingleton;

  // Information about all known actor namespaces. Maps serviceName -> className -> config.
  // This needs to be populated in advance of constructing any services, in order to be able to
  // correctly construct dependent services.
  kj::HashMap<kj::String, kj::HashMap<kj::String, ActorConfig>> actorConfigs;

  kj::HashMap<kj::String, kj::Own<Service>> services;

  kj::Own<kj::PromiseFulfiller<void>> fatalFulfiller;

  // Initialized in startAlarmScheduler().
  kj::Own<AlarmScheduler> alarmScheduler;

  // An HttpServer object maintained in a linked list.
  struct ListedHttpServer {
    Server& owner;
    kj::HttpServer httpServer;
    kj::ListLink<ListedHttpServer> link;

    template <typename... Params>
    ListedHttpServer(Server& owner, Params&&... params)
        : owner(owner),
          httpServer(kj::fwd<Params>(params)...) {
      owner.httpServers.add(*this);
    };
    ~ListedHttpServer() noexcept(false) {
      owner.httpServers.remove(*this);
    }
  };

  // All active HttpServer objects -- used to implement drain().
  kj::List<ListedHttpServer, &ListedHttpServer::link> httpServers;

  // Especially includes server loop tasks to listen on sockets. Any error is considered fatal.
  kj::TaskSet tasks;

  // Reports an exception thrown by a task in `tasks`.
  void taskFailed(kj::Exception&& exception) override;

  // Tell all HttpServers to drain once the drainWhen promise resolves.
  // This causes them to disconnect any connections that do not have a
  // request in flight.
  kj::Promise<void> handleDrain(kj::Promise<void> drainWhen);

  kj::Own<kj::TlsContext> makeTlsContext(config::TlsOptions::Reader conf);
  kj::Promise<kj::Own<kj::NetworkAddress>> makeTlsNetworkAddress(config::TlsOptions::Reader conf,
      kj::StringPtr addrStr,
      kj::Maybe<kj::StringPtr> certificateHost,
      uint defaultPort = 0);

  class HttpRewriter;

  kj::Own<Service> makeInvalidConfigService();
  kj::Own<Service> makeExternalService(kj::StringPtr name,
      config::ExternalServer::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder);
  kj::Own<Service> makeNetworkService(config::Network::Reader conf);
  kj::Own<Service> makeDiskDirectoryService(kj::StringPtr name,
      config::DiskDirectory::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder);
  kj::Own<Service> makeWorker(kj::StringPtr name,
      config::Worker::Reader conf,
      capnp::List<config::Extension>::Reader extensions);
  kj::Own<Service> makeService(config::Service::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      capnp::List<config::Extension>::Reader extensions);

  // Aborts all actors in this server except those in namespaces marked with `preventEviction`.
  void abortAllActors();

  // Can only be called in the link stage.
  //
  // May return a new object or may return a fake-own around a long-lived object.
  kj::Own<Service> lookupService(
      config::ServiceDesignator::Reader designator, kj::String errorContext);

  kj::Promise<void> listenHttp(kj::Own<kj::ConnectionReceiver> listener,
      kj::Own<Service> service,
      kj::StringPtr physicalProtocol,
      kj::Own<HttpRewriter> rewriter);

  class InvalidConfigService;
  class ExternalHttpService;
  class ExternalTcpService;
  class NetworkService;
  class DiskDirectoryService;
  class WorkerService;
  class WorkerEntrypointService;
  class HttpListener;

  void startServices(jsg::V8System& v8System,
      config::Config::Reader config,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      kj::ForkedPromise<void>& forkedDrainWhen);

  // Must be called after startServices!
  void startAlarmScheduler(config::Config::Reader config);

  kj::Promise<void> listenOnSockets(config::Config::Reader config,
      kj::HttpHeaderTable::Builder& headerTableBuilder,
      kj::ForkedPromise<void>& forkedDrainWhen,
      bool forTest = false);
};

// An ActorStorage implementation which will always respond to reads as if the state is empty,
// and will fail any writes.
kj::Own<rpc::ActorStorage::Stage::Server> newEmptyReadOnlyActorStorage();

}  // namespace workerd::server
