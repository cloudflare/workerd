// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "docker-client.h"

#include <workerd/util/http-util.h>

#include <kj/debug.h>
#include <kj/encoding.h>

namespace workerd::io {

DockerClient::DockerClient(kj::HttpClient& httpClient)
    : httpClient(httpClient),
      dockerSocketPath("/var/run/docker.sock") {}

kj::Promise<kj::String> DockerClient::makeDockerRequest(
    kj::HttpMethod method, kj::StringPtr path, kj::Maybe<kj::String> body) {

  auto url = kj::str("http://localhost", path);
  auto headerTable = kj::heap<kj::HttpHeaderTable>();
  auto headers = kj::HttpHeaders(*headerTable);
  auto request = httpClient.request(method, url, headers);

  KJ_IF_SOME(b, body) {
    auto stream = kj::heap<kj::StringInputStream>(kj::mv(b));
    request.body = stream.attach(kj::mv(stream));
  }

  auto response = co_await request.response;
  auto responseBody = co_await response.body->readAllText();

  if (response.statusCode >= 400) {
    KJ_FAIL_REQUIRE("Docker API error", response.statusCode, responseBody);
  }

  co_return kj::mv(responseBody);
}

kj::Promise<bool> DockerClient::containerExists(kj::StringPtr containerId) {
  try {
    co_await makeDockerRequest(kj::HttpMethod::GET, kj::str("/containers/", containerId, "/json"));
    co_return true;
  } catch (kj::Exception& e) {
    co_return false;
  }
}

kj::Promise<bool> DockerClient::isContainerRunning(kj::StringPtr containerId) {
  try {
    auto response = co_await makeDockerRequest(
        kj::HttpMethod::GET, kj::str("/containers/", containerId, "/json"));

    // Parse JSON to check State.Running - simplified check for now
    co_return response.findSubstring("\"Running\":true"_kj) != kj::none;
  } catch (kj::Exception& e) {
    co_return false;
  }
}

kj::Promise<void> DockerClient::startContainer(kj::StringPtr imageTag,
    kj::StringPtr containerId,
    kj::ArrayPtr<const kj::StringPtr> entrypoint,
    kj::ArrayPtr<const kj::StringPtr> environmentVariables,
    kj::HashMap<uint16_t, uint16_t>& portMappings) {

  // Build entrypoint JSON array
  auto entrypointJson = kj::strArray(entrypoint, "\", \"");
  if (entrypointJson.size() > 0) {
    entrypointJson = kj::str("\"", entrypointJson, "\"");
  }

  // Build environment variables JSON array
  auto envJson = kj::strArray(environmentVariables, "\", \"");
  if (envJson.size() > 0) {
    envJson = kj::str("\"", envJson, "\"");
  }

  // Build port bindings
  auto portBindingsJson = buildPortBindingsJson(portMappings);

  // Build docker run JSON
  auto runConfig = kj::str(R"({
    "Image": ")",
      escapeJsonString(imageTag), R"(",
    "Cmd": [)",
      entrypointJson, R"(],
    "Env": [)",
      envJson, R"(],
    "Labels": {
      "MINIFLARE_CONTAINER_INSTANCE": "true",
      "DO_INSTANCE_ID": ")",
      escapeJsonString(containerId), R"("
    },
    "HostConfig": {
      "PortBindings": {)",
      portBindingsJson, R"(}
    }
  })");

  // Create container
  co_await makeDockerRequest(
      kj::HttpMethod::POST, kj::str("/containers/create?name=", containerId), runConfig);

  // Start container
  co_await makeDockerRequest(kj::HttpMethod::POST, kj::str("/containers/", containerId, "/start"));
}

kj::Promise<void> DockerClient::stopContainer(kj::StringPtr containerId) {
  co_await makeDockerRequest(kj::HttpMethod::POST, kj::str("/containers/", containerId, "/stop"));
}

kj::Promise<void> DockerClient::killContainer(kj::StringPtr containerId, uint32_t signal) {
  co_await makeDockerRequest(
      kj::HttpMethod::POST, kj::str("/containers/", containerId, "/kill?signal=", signal));
}

kj::Promise<void> DockerClient::removeContainer(kj::StringPtr containerId) {
  co_await makeDockerRequest(kj::HttpMethod::DELETE, kj::str("/containers/", containerId));
}

kj::Promise<void> DockerClient::buildImage(kj::StringPtr tag, kj::StringPtr context) {
  // Simplified - would need to handle tar upload for build context
  KJ_UNIMPLEMENTED("buildImage not yet implemented");
}

kj::Promise<void> DockerClient::pullImage(kj::StringPtr tag) {
  co_await makeDockerRequest(kj::HttpMethod::POST, kj::str("/images/create?fromImage=", tag));
}

kj::Promise<kj::Array<uint16_t>> DockerClient::getExposedPorts(kj::StringPtr imageTag) {
  try {
    auto response =
        co_await makeDockerRequest(kj::HttpMethod::GET, kj::str("/images/", imageTag, "/json"));

    // Simplified JSON parsing - would need proper JSON parser
    auto ports = kj::heapArrayBuilder<uint16_t>();

    // Look for "ExposedPorts" in response and extract port numbers
    // This is a simplified implementation
    if (response.findSubstring("\"80/tcp\""_kj) != kj::none) {
      ports.add(80);
    }
    if (response.findSubstring("\"443/tcp\""_kj) != kj::none) {
      ports.add(443);
    }
    if (response.findSubstring("\"8080/tcp\""_kj) != kj::none) {
      ports.add(8080);
    }

    co_return ports.finish();
  } catch (kj::Exception& e) {
    co_return kj::heapArray<uint16_t>(0);
  }
}

kj::Promise<uint16_t> DockerClient::allocateHostPort() {
  // Simple port allocation starting from 32768
  for (uint16_t port = 32768; port < 65535; port++) {
    if (usedPorts.find(port) == usedPorts.end()) {
      usedPorts.insert(port);
      co_return port;
    }
  }
  KJ_FAIL_REQUIRE("No available host ports");
}

void DockerClient::releaseHostPort(uint16_t port) {
  usedPorts.erase(port);
}

kj::Promise<void> DockerClient::waitForContainerExit(kj::StringPtr containerId) {
  co_await makeDockerRequest(kj::HttpMethod::POST, kj::str("/containers/", containerId, "/wait"));
}

kj::Promise<void> DockerClient::connectToContainerPort(
    kj::StringPtr containerId, uint16_t containerPort, kj::AsyncIoStream& connection) {
  // For local development, we need to connect to the host-mapped port
  // This is a simplified implementation - in reality we'd need to get the actual host port
  // from the port mappings established when the container was started
  KJ_UNIMPLEMENTED("connectToContainerPort not yet fully implemented");
}

kj::String DockerClient::buildPortBindingsJson(
    const kj::HashMap<uint16_t, uint16_t>& portMappings) {
  if (portMappings.size() == 0) {
    return kj::str("");
  }

  auto parts = kj::heapArrayBuilder<kj::String>();

  for (auto& mapping: portMappings) {
    parts.add(kj::str("\"", mapping.key, "/tcp\": [{\"HostPort\": \"", mapping.value, "\"}]"));
  }

  return kj::strArray(parts.finish(), ", ");
}

kj::String DockerClient::escapeJsonString(kj::StringPtr str) {
  // Simple JSON string escaping
  auto result = kj::str(str);
  // Would need to properly escape quotes, backslashes, etc.
  return kj::mv(result);
}

}  // namespace workerd::io