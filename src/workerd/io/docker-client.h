// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/map.h>
#include <kj/string.h>

namespace workerd::api {
class Container;
}

namespace workerd::io {

class DockerClient {
 public:
  explicit DockerClient(kj::HttpClient& httpClient);

  // Container lifecycle
  kj::Promise<bool> containerExists(kj::StringPtr containerId);
  kj::Promise<bool> isContainerRunning(kj::StringPtr containerId);
  kj::Promise<void> startContainer(kj::StringPtr imageTag,
      kj::StringPtr containerId,
      kj::ArrayPtr<const kj::StringPtr> entrypoint,
      kj::ArrayPtr<const kj::StringPtr> environmentVariables,
      kj::HashMap<uint16_t, uint16_t>& portMappings);
  kj::Promise<void> stopContainer(kj::StringPtr containerId);
  kj::Promise<void> killContainer(kj::StringPtr containerId, uint32_t signal);
  kj::Promise<void> removeContainer(kj::StringPtr containerId);

  // Image management
  kj::Promise<void> buildImage(kj::StringPtr tag, kj::StringPtr context);
  kj::Promise<void> pullImage(kj::StringPtr tag);
  kj::Promise<kj::Array<uint16_t>> getExposedPorts(kj::StringPtr imageTag);

  // Port management
  kj::Promise<uint16_t> allocateHostPort();
  void releaseHostPort(uint16_t port);

  // Monitoring
  kj::Promise<void> waitForContainerExit(kj::StringPtr containerId);

  // TCP Port connection
  kj::Promise<void> connectToContainerPort(
      kj::StringPtr containerId, uint16_t containerPort, kj::AsyncIoStream& connection);

 private:
  kj::HttpClient& httpClient;
  kj::String dockerSocketPath;
  kj::HashSet<uint16_t> usedPorts;

  kj::Promise<kj::String> makeDockerRequest(
      kj::HttpMethod method, kj::StringPtr path, kj::Maybe<kj::String> body = kj::none);

  kj::String buildPortBindingsJson(const kj::HashMap<uint16_t, uint16_t>& portMappings);
  kj::String escapeJsonString(kj::StringPtr str);
};

}  // namespace workerd::io