// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/container.capnp.h>
#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token.h>
#include <workerd/server/docker-api.capnp.h>

#include <capnp/compat/byte-stream.h>
#include <capnp/compat/json.h>
#include <capnp/list.h>
#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/filesystem.h>
#include <kj/map.h>
#include <kj/refcount.h>
#include <kj/string.h>

#include <atomic>

namespace workerd::server {

// Distinguishes how an egress mapping should proxy matched connections.
enum class EgressProtocol : uint8_t {
  HTTP,   // Parse HTTP inside the CONNECT tunnel and forward via worker request().
  HTTPS,  // Same as HTTP but with TLS interception (CA-cert injected).
  TCP,    // Forward raw bytes via worker connect().
};

// Decode a JSON string into a Cap'n Proto message of type T. The MallocMessageBuilder is
// heap-allocated and returned as an owned pointer so that the decoded data outlives this
// call. Callers must keep the returned message alive while accessing the root via
// message->getRoot<T>(). A previous version allocated the builder on the stack and returned
// a Builder (which is just a pointer into the message's arena); that caused every caller to
// dereference freed memory after the function returned.
template <typename T>
kj::Own<capnp::MallocMessageBuilder> decodeJsonResponse(kj::StringPtr response) {
  auto message = kj::heap<capnp::MallocMessageBuilder>();
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  auto jsonRoot = message->initRoot<T>();
  codec.decode(response, jsonRoot);
  return message;
}

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
      kj::String containerEgressInterceptorImage,
      kj::TaskSet& waitUntilTasks,
      kj::Promise<void> pendingCleanup,
      kj::Function<void(kj::Promise<void>)> cleanupCallback,
      ChannelTokenHandler& channelTokenHandler);

  ~ContainerClient() noexcept(false);

  // Implement rpc::Container::Server interface
  kj::Promise<void> status(StatusContext context) override;
  kj::Promise<void> start(StartContext context) override;
  kj::Promise<void> monitor(MonitorContext context) override;
  kj::Promise<void> destroy(DestroyContext context) override;
  kj::Promise<void> signal(SignalContext context) override;
  kj::Promise<void> exec(ExecContext context) override;
  kj::Promise<void> getTcpPort(GetTcpPortContext context) override;
  kj::Promise<void> listenTcp(ListenTcpContext context) override;
  kj::Promise<void> setInactivityTimeout(SetInactivityTimeoutContext context) override;
  kj::Promise<void> setEgressHttp(SetEgressHttpContext context) override;
  kj::Promise<void> setEgressHttps(SetEgressHttpsContext context) override;
  kj::Promise<void> setEgressTcp(SetEgressTcpContext context) override;
  kj::Promise<void> snapshotDirectory(SnapshotDirectoryContext context) override;
  kj::Promise<void> snapshotContainer(SnapshotContainerContext context) override;
  kj::Promise<void> inspect(InspectContext context) override;

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
  kj::String containerEgressInterceptorImage;

  kj::TaskSet& waitUntilTasks;

  // Forked promise representing pending cleanup from a previous ContainerClient for the same
  // container ID. status() co_awaits a branch so that Docker inspect only runs after any
  // in-flight DELETE from the previous client has settled (either completed or been cancelled
  // via containerCleanupCanceler, in which case the .catch_() resolves it immediately).
  kj::ForkedPromise<void> pendingCleanup;

  static constexpr kj::StringPtr defaultEnv[] = {"CLOUDFLARE_COUNTRY_A2=XX"_kj,
    "CLOUDFLARE_DEPLOYMENT_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_LOCATION=loc01"_kj, "CLOUDFLARE_REGION=REGN"_kj,
    "CLOUDFLARE_APPLICATION_ID=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"_kj,
    "CLOUDFLARE_DURABLE_OBJECT_ID=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"_kj};

  // Docker-specific Port implementation
  class DockerPort;
  class DockerProcessHandle;

  // EgressHttpService handles CONNECT requests from proxy-anything sidecar
  friend class EgressHttpService;

  struct Label {
    kj::String name;
    kj::String value;
  };

  struct InspectResponse {
    bool isRunning;
    kj::Array<Label> labels;
  };

  struct IPAMConfigResult {
    kj::String gateway;
    kj::String subnet;
  };

  struct SidecarInspectResponse {
    uint16_t ingressHostPort;
  };

  struct SnapshotRestoreMount {
    kj::Path restorePath;
    kj::String sourceVolume;
    kj::String cloneVolume;
  };

  struct ImageInspectResponse {
    kj::String id;
    uint64_t size;
  };

  struct ExecInspectResponse {
    int32_t exitCode;
    bool running;
    uint32_t pid;
  };

  kj::Promise<kj::Maybe<InspectResponse>> inspectContainer();

  kj::Promise<void> updateSidecarEgressPort(uint16_t ingressHostPort, uint16_t egressPort);
  kj::Promise<void> updateSidecarEgressConfig(uint16_t ingressHostPort, uint16_t egressPort);
  kj::Promise<void> createContainer(kj::StringPtr effectiveImage,
      kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
      kj::Maybe<capnp::List<capnp::Text>::Reader> environment,
      kj::ArrayPtr<const SnapshotRestoreMount> restoreMounts,
      rpc::Container::StartParams::Reader params);
  kj::Promise<kj::String> createExec(capnp::List<capnp::Text>::Reader cmd,
      rpc::Container::ExecOptions::Reader params,
      bool attachStdout,
      bool attachStderr);
  kj::Promise<kj::Own<kj::AsyncIoStream>> startExec(kj::String execId);
  kj::Promise<ExecInspectResponse> inspectExec(kj::StringPtr execId);
  kj::Promise<void> runSimpleExec(kj::ArrayPtr<const kj::String> cmd);
  kj::Promise<void> startContainer();
  kj::Promise<void> stopContainer();
  kj::Promise<void> killContainer(uint32_t signal);
  kj::Promise<void> destroyContainer();

  // Docker volume management for snapshots
  kj::Promise<void> createVolume(kj::StringPtr volumeName);
  kj::Promise<void> deleteVolume(kj::String volumeName);
  kj::Promise<void> commitContainer(kj::StringPtr imageRef);
  kj::Promise<ImageInspectResponse> inspectImage(kj::StringPtr imageRef);
  kj::Promise<void> deleteImage(kj::String imageRef);
  kj::Promise<kj::String> createTempContainerWithVolume(
      kj::StringPtr volumeName, kj::StringPtr mountPath);
  // Creates a writable clone volume by copying an existing snapshot volume through a
  // short-lived helper container. The caller mounts the returned clone into the app
  // container with NoCopy=true so the restored path masks any image contents there.
  kj::Promise<void> cloneSnapshot(SnapshotRestoreMount& snapshot);
  kj::Promise<void> deleteTempContainer(kj::String tempContainerId);

  // Sidecar container management (for egress proxy)
  // Inspect the sidecar container to retrieve the port to ingress to
  kj::Promise<kj::Maybe<SidecarInspectResponse>> inspectSidecar();
  kj::Promise<void> createSidecarContainer(uint16_t egressPort, kj::String networkCidr);
  kj::Promise<void> startSidecarContainer();
  kj::Promise<void> destroySidecarContainer();
  kj::Promise<void> monitorSidecarContainer();

  // Cleanup callback invoked from the destructor. Receives the joined cleanup promise so
  // ActorNamespace can wrap it with the canceler, store it for the next ContainerClient
  // to await, and add a branch to waitUntilTasks to keep the cleanup tasks alive.
  kj::Function<void(kj::Promise<void>)> cleanupCallback;

  // For redeeming channel tokens received via setEgressHttp / setEgressHttps.
  ChannelTokenHandler& channelTokenHandler;

  // Opaque implementation struct holding egress mappings. Defined in container-client.c++ to
  // avoid pulling heavy types (kj::OneOf, kj::CidrRange, kj::Vector) into server.c++ which
  // includes this header.
  struct EgressState;
  kj::Own<EgressState> egressState;

  // Insert or replace an egress mapping.
  struct EgressMapping;
  void upsertEgressMapping(EgressMapping mapping);
  kj::Vector<kj::String> getDnsAllowHostnames() const;

  // Find a matching egress mapping for the given destination address (host:port format).
  // Returns an addRef'd Own so the channel stays alive even if the mapping is later replaced.
  kj::Maybe<kj::Own<workerd::IoChannelFactory::SubrequestChannel>> findEgressMapping(
      kj::StringPtr destAddr,
      uint16_t defaultPort,
      kj::Maybe<kj::StringPtr> hostname,
      EgressProtocol protocol);

  kj::Promise<void> writeFileToContainer(kj::StringPtr container,
      kj::StringPtr dir,
      kj::StringPtr filename,
      kj::ArrayPtr<const kj::byte> content);
  kj::Promise<void> readCACert();
  kj::Promise<void> injectCACert();

  // Whether general internet access is enabled for this container, when known.
  kj::Maybe<bool> internetEnabled = kj::none;

  std::atomic_bool containerStarted = false;
  std::atomic_bool containerSidecarStarted = false;
  std::atomic_bool egressListenerStarted = false;
  std::atomic_bool caCertInjected = false;

  // Writable clone volumes currently owned by the app container, or by an in-flight start()
  // that still needs failure cleanup.
  kj::Vector<kj::String> snapshotClones;

  // CA cert read from the sidecar after it starts.
  kj::Maybe<kj::String> caCert;

  kj::Maybe<kj::Own<kj::HttpServer>> egressHttpServer;
  kj::Maybe<kj::Promise<void>> egressListenerTask;

  uint16_t egressListenerPort = 0;
  kj::Maybe<uint16_t> sidecarIngressHostPort;

  // All mutating RPCs need to ask and wait on an RpcTurn before doing any mutations.
  // monitor() is an exception. It waits for all pending mutating RPCs without joining
  // the queue itself.
  kj::ForkedPromise<void> mutationQueue = kj::Promise<void>(kj::READY_NOW).fork();

  struct RpcTurn {
    kj::Promise<void> ready;
    kj::Own<kj::PromiseFulfiller<void>> done;
  };
  // Get a turn to run mutating RPC.
  // Callers will receive a RpcTurn where they can wait and then resolve
  // when they finish through a KJ defer.
  RpcTurn getRpcTurn();

  // Get the Docker bridge network gateway IP and subnet.
  kj::Promise<IPAMConfigResult> getDockerBridgeIPAMConfig();
  // Check if the Docker daemon has IPv6 enabled by inspecting the default bridge network's
  // IPAM config for IPv6 subnets.
  kj::Promise<bool> isDaemonIpv6Enabled();
  // Start the egress listener on the given address. If port is 0, an OS-chosen port is used.
  kj::Promise<uint16_t> startEgressListener(kj::String listenAddress, uint16_t port = 0);
  void stopEgressListener();
  // Ensure the egress listener is started exactly once.
  // Uses egressListenerStarted as a guard. Called from setEgressHttp() and status().
  // If port is non-zero, binds to that specific port (for reconnecting to an existing sidecar).
  kj::Promise<void> ensureEgressListenerStarted(uint16_t port = 0);
  // Ensure the egress listener and sidecar container are started exactly once.
  // Uses containerSidecarStarted as a guard. Called from both start() and setEgressHttp().
  kj::Promise<void> ensureSidecarStarted();
};

}  // namespace workerd::server
