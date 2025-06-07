// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/container.capnp.h>
#include <workerd/io/docker-client.h>

#include <kj/async.h>
#include <kj/string.h>

namespace workerd::io {

// Docker-based implementation that implements the rpc::Container::Server interface
// so it can be used as a rpc::Container::Client via kj::heap<DockerContainerClient>().
// This allows the Container JSG class to use Docker directly without knowing
// it's talking to Docker instead of a real RPC service.
class DockerContainerClient final: public rpc::Container::Server {
 public:
  DockerContainerClient(kj::String containerId, kj::String imageTag, DockerClient& dockerClient);

  // Implement rpc::Container::Server interface
  kj::Promise<void> status(StatusContext context) override;
  kj::Promise<void> start(StartContext context) override;
  kj::Promise<void> monitor(MonitorContext context) override;
  kj::Promise<void> destroy(DestroyContext context) override;
  kj::Promise<void> signal(SignalContext context) override;
  kj::Promise<void> getTcpPort(GetTcpPortContext context) override;
  kj::Promise<void> listenTcp(ListenTcpContext context) override;

 private:
  kj::String containerId;
  kj::String imageTag;
  DockerClient& dockerClient;
  bool running = false;
  kj::HashMap<uint16_t, uint16_t> portMappings;  // container -> host port

  // Docker-specific Port implementation
  class DockerPort;
};

}  // namespace workerd::io
