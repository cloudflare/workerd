// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/container.capnp.h>
#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/list.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/map.h>
#include <kj/refcount.h>
#include <kj/string.h>

namespace workerd::server {

// Docker-based implementation that implements the rpc::Container::Server interface
// so it can be used as a rpc::Container::Client via kj::heap<ContainerClient>().
// This allows the Container JSG class to use Docker directly without knowing
// it's talking to Docker instead of a real RPC service.
//
// ContainerClient is reference-counted to support actor reconnection with inactivity timeouts.
// When setInactivityTimeout() is called, a timer holds a reference to prevent premature
// destruction. The ContainerClient can be shared across multiple actor lifetimes.
class ContainerClient final: public rpc::Container::Server, public kj::Refcounted {
 public:
  ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
      kj::Timer& timer,
      kj::Network& network,
      kj::String dockerPath,
      kj::String containerName,
      kj::String imageName,
      kj::TaskSet& waitUntilTasks,
      kj::Function<void()> cleanupCallback,
      ChannelTokenHandler& channelTokenHandler);

  ~ContainerClient() noexcept(false);

  // Implement rpc::Container::Server interface
  kj::Promise<void> status(StatusContext context) override;
  kj::Promise<void> start(StartContext context) override;
  kj::Promise<void> monitor(MonitorContext context) override;
  kj::Promise<void> destroy(DestroyContext context) override;
  kj::Promise<void> signal(SignalContext context) override;
  kj::Promise<void> getTcpPort(GetTcpPortContext context) override;
  kj::Promise<void> listenTcp(ListenTcpContext context) override;
  kj::Promise<void> setInactivityTimeout(SetInactivityTimeoutContext context) override;
  kj::Promise<void> setEgressTcp(SetEgressTcpContext context) override;

  kj::Own<ContainerClient> addRef();

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::Timer& timer;
  kj::Network& network;
  kj::String dockerPath;
  kj::String containerName;
  kj::String imageName;
  kj::TaskSet& waitUntilTasks;

  static constexpr kj::StringPtr defaultEnv[] = {"CLOUDFLARE_COUNTRY_A2=XX"_kj,
    "CLOUDFLARE_DEPLOYMENT_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_LOCATION=loc01"_kj, "CLOUDFLARE_REGION=REGN"_kj,
    "CLOUDFLARE_APPLICATION_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_DURABLE_OBJECT_ID=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"_kj};

  // Docker-specific Port implementation
  class DockerPort;

  struct Response {
    kj::uint statusCode;
    kj::String body;
  };

  struct InspectResponse {
    bool isRunning;
    kj::HashMap<uint16_t, uint16_t> ports;
  };

  // Docker API v1.50 helper methods
  static kj::Promise<Response> dockerApiRequest(kj::Network& network,
      kj::String dockerPath,
      kj::HttpMethod method,
      kj::String endpoint,
      kj::Maybe<kj::String> body = kj::none);
  kj::Promise<InspectResponse> inspectContainer();
  kj::Promise<void> createContainer(kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
      kj::Maybe<capnp::List<capnp::Text>::Reader> environment);
  kj::Promise<void> startContainer();
  kj::Promise<void> stopContainer();
  kj::Promise<void> killContainer(uint32_t signal);
  kj::Promise<void> destroyContainer();

  // Cleanup callback to remove from ActorNamespace map when destroyed
  kj::Function<void()> cleanupCallback;

  // For redeeming channel tokens received via setEgressTcp
  ChannelTokenHandler& channelTokenHandler;

  // Egress TCP mappings: address -> SubrequestChannel
  kj::HashMap<kj::String, kj::Own<workerd::IoChannelFactory::SubrequestChannel>> egressMappings;
};

}  // namespace workerd::server
