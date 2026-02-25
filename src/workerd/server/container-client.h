// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/container.capnp.h>
#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token.h>
#include <workerd/server/docker-api.capnp.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/list.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/cidr.h>
#include <kj/compat/http.h>
#include <kj/map.h>
#include <kj/refcount.h>
#include <kj/string.h>

#include <atomic>

namespace workerd::server {

// Docker-based implementation that implements the rpc::Container::Server interface
// so it can be used as a rpc::Container::Client via kj::heap<ContainerClient>().
// This allows the Container JSG class to use Docker directly without knowing
// it's talking to Docker instead of a real RPC service.
//
// ContainerClient is reference-counted to support actor reconnection with inactivity timeouts.
// When setInactivityTimeout() is called, a timer holds a reference to prevent premature
// destruction. The ContainerClient can be shared across multiple actor lifetimes
class ContainerClient final: public rpc::Container::Server, public kj::Refcounted {
 public:
  ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
      kj::Timer& timer,
      kj::Network& network,
      kj::String dockerPath,
      kj::String containerName,
      kj::String imageName,
      kj::Maybe<kj::String> containerEgressInterceptorImage,
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
  kj::Promise<void> setEgressHttp(SetEgressHttpContext context) override;

  kj::Own<ContainerClient> addRef();

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::HttpHeaderTable headerTable;
  kj::Timer& timer;
  kj::Network& network;
  kj::String dockerPath;
  kj::String containerName;
  kj::String sidecarContainerName;
  kj::String imageName;

  // Container egress interceptor image name (sidecar for egress proxy)
  kj::Maybe<kj::String> containerEgressInterceptorImage;

  kj::TaskSet& waitUntilTasks;

  static constexpr kj::StringPtr defaultEnv[] = {"CLOUDFLARE_COUNTRY_A2=XX"_kj,
    "CLOUDFLARE_DEPLOYMENT_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_LOCATION=loc01"_kj, "CLOUDFLARE_REGION=REGN"_kj,
    "CLOUDFLARE_APPLICATION_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_DURABLE_OBJECT_ID=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"_kj};

  // Docker-specific Port implementation
  class DockerPort;

  // EgressHttpService handles CONNECT requests from proxy-anything sidecar
  friend class EgressHttpService;

  struct Response {
    kj::uint statusCode;
    kj::String body;
  };

  struct InspectResponse {
    bool isRunning;
    kj::HashMap<uint16_t, uint16_t> ports;
  };

  struct IPAMConfigResult {
    kj::String gateway;
    kj::String subnet;
  };

  // Docker API v1.50 helper methods
  static kj::Promise<Response> dockerApiRequest(kj::Network& network,
      kj::String dockerPath,
      kj::HttpMethod method,
      kj::String endpoint,
      kj::Maybe<kj::String> body = kj::none);
  kj::Promise<InspectResponse> inspectContainer();
  kj::Promise<void> createContainer(kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
      kj::Maybe<capnp::List<capnp::Text>::Reader> environment,
      rpc::Container::StartParams::Reader params);
  kj::Promise<void> startContainer();
  kj::Promise<void> stopContainer();
  kj::Promise<void> killContainer(uint32_t signal);
  kj::Promise<void> destroyContainer();

  // Sidecar container management (for egress proxy)
  kj::Promise<void> createSidecarContainer(uint16_t egressPort, kj::String networkCidr);
  kj::Promise<void> startSidecarContainer();
  kj::Promise<void> destroySidecarContainer();
  kj::Promise<void> monitorSidecarContainer();

  // Cleanup callback to remove from ActorNamespace map when destroyed
  kj::Function<void()> cleanupCallback;

  // For redeeming channel tokens received via setEgressHttp
  ChannelTokenHandler& channelTokenHandler;

  // Represents a parsed egress mapping with CIDR and port matching
  struct EgressMapping {
    kj::CidrRange cidr;
    uint16_t port;  // 0 means match all ports
    kj::Own<workerd::IoChannelFactory::SubrequestChannel> channel;
  };

  kj::Vector<EgressMapping> egressMappings;

  // Find a matching egress mapping for the given destination address (host:port format)
  kj::Maybe<workerd::IoChannelFactory::SubrequestChannel*> findEgressMapping(
      kj::StringPtr destAddr, uint16_t defaultPort);

  // Whether general internet access is enabled for this container
  bool internetEnabled = false;

  std::atomic_bool containerStarted = false;
  std::atomic_bool containerSidecarStarted = false;
  std::atomic_bool workerdNetworkCreated = false;

  kj::Maybe<kj::Own<kj::HttpServer>> egressHttpServer;
  kj::Maybe<kj::Promise<void>> egressListenerTask;

  uint16_t egressListenerPort = 0;
  // Set to the gateway IP when the egress listener successfully binds to it.
  // When none, the listener fell back to 127.0.0.1 and the sidecar must use host-gateway.
  kj::Maybe<kj::String> egressGatewayIp;

  // Get the Docker bridge network gateway IP and subnet.
  // Prefers the "workerd-network" bridge, creating it if needed
  kj::Promise<IPAMConfigResult> getDockerBridgeIPAMConfig();
  // Create the workerd-network Docker bridge network with IPv6 support
  kj::Promise<void> createWorkerdNetwork();
  // Check if the Docker daemon has IPv6 enabled by inspecting the default bridge network's
  // IPAM config for IPv6 subnets.
  kj::Promise<bool> isDaemonIpv6Enabled();
  // Start the egress listener on the specified address, returns the chosen port
  kj::Promise<uint16_t> startEgressListener(kj::StringPtr listenAddress);
  void stopEgressListener();
  // Ensure the egress listener and sidecar container are started exactly once.
  // Uses containerSidecarStarted as a guard. Called from both start() and setEgressHttp().
  kj::Promise<void> ensureSidecarStarted();
};

}  // namespace workerd::server
