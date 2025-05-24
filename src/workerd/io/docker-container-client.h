// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/container.capnp.h>
#include <workerd/io/docker-client.h>

#include <kj/async.h>
#include <kj/string.h>

namespace workerd::io {

// Docker-based implementation of the rpc::Container::Client interface.
// This allows the Container JSG class to use Docker directly without knowing
// it's talking to Docker instead of a real RPC service.
class DockerContainerClient final: public rpc::Container::Client {
 public:
  DockerContainerClient(kj::String containerId, kj::String imageTag, DockerClient& dockerClient);

  // Implement rpc::Container::Client interface
  kj::Promise<capnp::Response<rpc::Container::StatusResults>> statusRequest(
      capnp::Request<rpc::Container::StatusParams, rpc::Container::StatusResults> request) override;

  kj::Promise<capnp::Response<rpc::Container::StartResults>> startRequest(
      capnp::Request<rpc::Container::StartParams, rpc::Container::StartResults> request) override;

  kj::Promise<capnp::Response<rpc::Container::MonitorResults>> monitorRequest(
      capnp::Request<rpc::Container::MonitorParams, rpc::Container::MonitorResults> request)
      override;

  kj::Promise<capnp::Response<rpc::Container::DestroyResults>> destroyRequest(
      capnp::Request<rpc::Container::DestroyParams, rpc::Container::DestroyResults> request)
      override;

  kj::Promise<capnp::Response<rpc::Container::SignalResults>> signalRequest(
      capnp::Request<rpc::Container::SignalParams, rpc::Container::SignalResults> request) override;

  kj::Promise<capnp::Response<rpc::Container::GetTcpPortResults>> getTcpPortRequest(
      capnp::Request<rpc::Container::GetTcpPortParams, rpc::Container::GetTcpPortResults> request)
      override;

  kj::Promise<capnp::Response<rpc::Container::ListenTcpResults>> listenTcpRequest(
      capnp::Request<rpc::Container::ListenTcpParams, rpc::Container::ListenTcpResults> request)
      override;

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