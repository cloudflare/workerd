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
      : containerId(kj::mv(containerId)) {}

  kj::Promise<void> connect(ConnectContext context) override {
    auto params = context.getParams();

    // Get the downstream ByteStream
    auto down = params.getDown();

    // TODO: Implement proper ByteStream to AsyncIoStream conversion
    // For now, this is a placeholder that will need proper implementation
    KJ_UNIMPLEMENTED("Docker port connection not yet fully implemented");
  }

 private:
  kj::String containerId;
};

DockerContainerClient::DockerContainerClient(
    kj::String containerId, kj::String imageTag, DockerClient& dockerClient)
    : containerId(kj::mv(containerId)),
      imageTag(kj::mv(imageTag)),
      dockerClient(dockerClient) {}

kj::Promise<void> DockerContainerClient::status(StatusContext context) {
  return dockerClient.isContainerRunning(containerId).then([context](bool isRunning) mutable {
    auto results = context.getResults();
    results.setRunning(isRunning);
  });
}

kj::Promise<void> DockerContainerClient::start(StartContext context) {
  auto params = context.getParams();

  // Convert entrypoint
  kj::Array<kj::StringPtr> entrypointPtrs;
  if (params.hasEntrypoint()) {
    auto entrypoint = params.getEntrypoint();
    auto builder = kj::heapArrayBuilder<kj::StringPtr>(entrypoint.size());
    for (auto cmd: entrypoint) {
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
    for (auto envVar: envVars) {
      builder.add(envVar);
    }
    envPtrs = builder.finish();
  } else {
    envPtrs = kj::heapArray<kj::StringPtr>(0);
  }

  return dockerClient.startContainer(imageTag, containerId, entrypointPtrs, envPtrs, portMappings)
      .then([this]() mutable { running = true; });
}

kj::Promise<void> DockerContainerClient::monitor(MonitorContext context) {
  return dockerClient.waitForContainerExit(containerId).then([this]() mutable { running = false; });
}

kj::Promise<void> DockerContainerClient::destroy(DestroyContext context) {
  if (!running) {
    return kj::READY_NOW;
  }

  return dockerClient.stopContainer(containerId).then([this]() mutable { running = false; });
}

kj::Promise<void> DockerContainerClient::signal(SignalContext context) {
  auto params = context.getParams();
  uint32_t signo = params.getSigno();

  return dockerClient.killContainer(containerId, signo);
}

kj::Promise<void> DockerContainerClient::getTcpPort(GetTcpPortContext context) {
  auto params = context.getParams();
  uint16_t port = params.getPort();

  auto results = context.getResults();
  auto dockerPort = kj::heap<DockerPort>(dockerClient, kj::str(containerId), port);
  results.setPort(kj::mv(dockerPort));

  return kj::READY_NOW;
}

kj::Promise<void> DockerContainerClient::listenTcp(ListenTcpContext context) {
  // ListenTcp is not implemented for Docker mode yet
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers");
}

}  // namespace workerd::io
