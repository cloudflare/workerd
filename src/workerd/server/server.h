// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/filesystem.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/async-io.h>
#include <workerd/server/workerd.capnp.h>
#include <kj/compat/http.h>

namespace kj {
  class TlsContext;
}

namespace workerd::jsg {
  class V8System;
}

namespace workerd::server {

using kj::uint;

class Server: private kj::TaskSet::ErrorHandler {
  // Implements the single-tenant Workers Runtime server / CLI.
  //
  // The purpose of this class is to implement the core logic independently of the CLI itself,
  // in such a way that it can be unit-tested. workerd.c++ implements the CLI wrapper around this.

public:
  Server(kj::Filesystem& fs, kj::Timer& timer, kj::Network& network,
         kj::EntropySource& entropySource, kj::Function<void(kj::String)> reportConfigError);
  ~Server() noexcept(false);

  void allowExperimental() { experimental = true; }
  // Permit experimental features to be used. These features may break backwards compatibility
  // in the future.

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

  kj::Promise<void> run(jsg::V8System& v8System, config::Config::Reader conf,
                        kj::Promise<void> drainWhen = kj::NEVER_DONE);
  // Runs the server using the given config.

  kj::Promise<bool> test(jsg::V8System& v8System, config::Config::Reader conf,
                         kj::StringPtr servicePattern = "*"_kj,
                         kj::StringPtr entrypointPattern = "*"_kj);
  // Executes one or more tests. By default, all exported test handlers from all entrypoints to
  // all services in the config are executed. Glob patterns can be specified to match specific
  // service and entrypoint names.
  //
  // The returned promise resolves true if at least one test ran and no tests failed.

  struct Durable { kj::String uniqueKey; };
  struct Ephemeral {};
  using ActorConfig = kj::OneOf<Durable, Ephemeral>;

private:
  kj::Filesystem& fs;
  kj::Timer& timer;
  kj::Network& network;
  kj::EntropySource& entropySource;
  kj::Function<void(kj::String)> reportConfigError;

  bool experimental = false;

  kj::HashMap<kj::String, kj::OneOf<kj::String, kj::Own<kj::ConnectionReceiver>>> socketOverrides;
  kj::HashMap<kj::String, kj::String> directoryOverrides;
  kj::HashMap<kj::String, kj::String> externalOverrides;
  // Overrides from the command line.
  //
  // String overrides are left as strings rather than parsed by the caller in order to reuse the
  // code that parses strings from the config file.

  kj::Maybe<kj::String> inspectorOverride;

  struct GlobalContext;
  kj::Own<GlobalContext> globalContext;
  // General context needed to construct workers. Initilaized early in run().

  class Service;
  kj::Own<Service> invalidConfigServiceSingleton;

  kj::HashMap<kj::String, kj::HashMap<kj::String, ActorConfig>> actorConfigs;
  // Information about all known actor namespaces. Maps serviceName -> className -> config.
  // This needs to be populated in advance of constructing any services, in order to be able to
  // correctly construct dependent services.

  kj::HashMap<kj::String, kj::Own<Service>> services;

  kj::Own<kj::PromiseFulfiller<void>> fatalFulfiller;

  struct ListedHttpServer {
    // An HttpServer object maintained in a linked list.

    Server& owner;
    kj::HttpServer httpServer;
    kj::ListLink<ListedHttpServer> link;

    template <typename... Params>
    ListedHttpServer(Server& owner, Params&&... params)
        : owner(owner), httpServer(kj::fwd<Params>(params)...) {
      owner.httpServers.add(*this);
    };
    ~ListedHttpServer() noexcept(false) {
      owner.httpServers.remove(*this);
    }
  };

  kj::List<ListedHttpServer, &ListedHttpServer::link> httpServers;
  // All active HttpServer objects -- used to implement drain().

  kj::TaskSet tasks;
  // Especially includes server loop tasks to listen on socokets. Any error is considered fatal.

  void taskFailed(kj::Exception&& exception) override;
  // Reports an exception thrown by a task in `tasks`.

  kj::Own<kj::TlsContext> makeTlsContext(config::TlsOptions::Reader conf);
  kj::Promise<kj::Own<kj::NetworkAddress>> makeTlsNetworkAddress(
      config::TlsOptions::Reader conf, kj::StringPtr addrStr,
      kj::Maybe<kj::StringPtr> certificateHost, uint defaultPort = 0);

  class HttpRewriter;

  kj::Own<Service> makeInvalidConfigService();
  kj::Own<Service> makeExternalService(
      kj::StringPtr name, config::ExternalServer::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder);
  kj::Own<Service> makeNetworkService(config::Network::Reader conf);
  kj::Own<Service> makeDiskDirectoryService(
      kj::StringPtr name, config::DiskDirectory::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder);
  kj::Own<Service> makeWorker(kj::StringPtr name, config::Worker::Reader conf);
  kj::Own<Service> makeService(
      config::Service::Reader conf,
      kj::HttpHeaderTable::Builder& headerTableBuilder);

  Service& lookupService(config::ServiceDesignator::Reader designator, kj::String errorContext);
  // Can only be called in the link stage.

  kj::Promise<void> listenHttp(kj::Own<kj::ConnectionReceiver> listener, Service& service,
                               kj::StringPtr physicalProtocol, kj::Own<HttpRewriter> rewriter);

  class InvalidConfigService;
  class ExternalHttpService;
  class NetworkService;
  class DiskDirectoryService;
  class WorkerService;
  class WorkerEntrypointService;
  class HttpListener;

  class InspectorService;

  kj::Maybe<kj::Own<InspectorService>> maybeInspectorService;
  kj::Own<InspectorService> makeInspectorService(kj::HttpHeaderTable::Builder& headerTableBuilder);

  void startServices(jsg::V8System& v8System, config::Config::Reader config,
                     kj::HttpHeaderTable::Builder& headerTableBuilder,
                     kj::ForkedPromise<void>& forkedDrainWhen);

  kj::Promise<void> listenOnSockets(config::Config::Reader config,
                                    kj::HttpHeaderTable::Builder& headerTableBuilder,
                                    kj::ForkedPromise<void>& forkedDrainWhen);
};

}  // namespace workerd::server
