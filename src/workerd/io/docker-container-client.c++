// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "docker-container-client.h"
#include <workerd/jsg/jsg.h>
#include <kj/debug.h>

namespace workerd::io {

// Docker-specific Port implementation that implements rpc::Container::Port::Server
class DockerContainerClient::DockerPort final: public rpc::Container::Port::Server {
public:
  DockerPort(DockerClient& dockerClient, kj::String containerId, uint16_t containerPort)
      : dockerClient(dockerClient), containerId(kj::mv(containerId)), containerPort(containerPort) {}

  kj::Promise<void> connect(ConnectContext context) override {
    auto params = context.getParams();
    auto results = context.getResults();
    
    // Get the downstream ByteStream
    auto down = params.getDown();
    
    // TODO: Implement proper ByteStream to AsyncIoStream conversion
    // For now, this is a placeholder that will need proper implementation
    KJ_UNIMPLEMENTED("Docker port connection not yet fully implemented");
  }

private:
  DockerClient& dockerClient;
  kj::String containerId;
  uint16_t containerPort;
};

DockerContainerClient::DockerContainerClient(kj::String containerId, kj::String imageTag, DockerClient& dockerClient)
    : containerId(kj::mv(containerId)), imageTag(kj::mv(imageTag)), dockerClient(dockerClient) {}

kj::Promise<capnp::Response<rpc::Container::StatusResults>> DockerContainerClient::statusRequest(
    capnp::Request<rpc::Container::StatusParams, rpc::Container::StatusResults> request) {
  return dockerClient.isContainerRunning(containerId).then(
      [request = kj::mv(request)](bool isRunning) mutable {
    auto results = request.initResults();
    results.setRunning(isRunning);
    return kj::mv(request);
  });
}

kj::Promise<capnp::Response<rpc::Container::StartResults>> DockerContainerClient::startRequest(
    capnp::Request<rpc::Container::StartParams, rpc::Container::StartResults> request) {
  auto params = request.getParams();
  
  // Convert entrypoint
  kj::Array<kj::StringPtr> entrypointPtrs;
  if (params.hasEntrypoint()) {
    auto entrypoint = params.getEntrypoint();
    auto builder = kj::heapArrayBuilder<kj::StringPtr>(entrypoint.size());
    for (auto cmd : entrypoint) {
      builder.add(cmd);
    }
    entrypointPtrs = builder.finish();
  } else {
    entrypointPtrs = kj::heapArray<kj::StringPtr>(0);
  }
  
  // Convert environment variables
  kj::Array<kj::StringPtr> envPtrs;
  if (params.hasEnvironmentVariables()) {
    auto envVars = params.getEnvironmentVariables();
    auto builder = kj::heapArrayBuilder<kj::StringPtr>(envVars.size());
    for (auto envVar : envVars) {
      builder.add(envVar);
    }
    envPtrs = builder.finish();
  } else {
    envPtrs = kj::heapArray<kj::StringPtr>(0);
  }
  
  return dockerClient.startContainer(imageTag, containerId, entrypointPtrs, envPtrs, portMappings)
      .then([request = kj::mv(request), this]() mutable {
    running = true;
    auto results = request.initResults();
    return kj::mv(request);
  });
}

kj::Promise<capnp::Response<rpc::Container::MonitorResults>> DockerContainerClient::monitorRequest(
    capnp::Request<rpc::Container::MonitorParams, rpc::Container::MonitorResults> request) {
  return dockerClient.waitForContainerExit(containerId).then(
      [request = kj::mv(request), this]() mutable {
    running = false;
    auto results = request.initResults();
    return kj::mv(request);
  });
}

kj::Promise<capnp::Response<rpc::Container::DestroyResults>> DockerContainerClient::destroyRequest(
    capnp::Request<rpc::Container::DestroyParams, rpc::Container::DestroyResults> request) {
  if (!running) {
    auto results = request.initResults();
    return kj::mv(request);
  }
  
  return dockerClient.stopContainer(containerId).then(
      [request = kj::mv(request), this]() mutable {
    running = false;
    auto results = request.initResults();
    return kj::mv(request);
  });
}

kj::Promise<capnp::Response<rpc::Container::SignalResults>> DockerContainerClient::signalRequest(
    capnp::Request<rpc::Container::SignalParams, rpc::Container::SignalResults> request) {
  auto params = request.getParams();
  uint32_t signo = params.getSigno();
  
  return dockerClient.killContainer(containerId, signo).then(
      [request = kj::mv(request)]() mutable {
    auto results = request.initResults();
    return kj::mv(request);
  });
}

kj::Promise<capnp::Response<rpc::Container::GetTcpPortResults>> DockerContainerClient::getTcpPortRequest(
    capnp::Request<rpc::Container::GetTcpPortParams, rpc::Container::GetTcpPortResults> request) {
  auto params = request.getParams();
  uint16_t port = params.getPort();
  
  auto results = request.initResults();
  auto dockerPort = kj::heap<DockerPort>(dockerClient, kj::str(containerId), port);
  results.setPort(kj::mv(dockerPort));
  
  return kj::mv(request);
}

kj::Promise<capnp::Response<rpc::Container::ListenTcpResults>> DockerContainerClient::listenTcpRequest(
    capnp::Request<rpc::Container::ListenTcpParams, rpc::Container::ListenTcpResults> request) {
  // ListenTcp is not implemented for Docker mode yet
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers");
}

} // namespace workerd::io