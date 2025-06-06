// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/docker-api.capnp.h>
#include <workerd/jsg/jsg.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/exception.h>

namespace workerd::io {

ContainerClient::ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
    kj::Own<kj::HttpClient> network,
    kj::Own<kj::HttpClient> httpClient,
    kj::String containerName,
    kj::String imageTag)
    : byteStreamFactory(byteStreamFactory),
      network(kj::mv(network)),
      httpClient(kj::mv(httpClient)),
      containerName(kj::mv(containerName)),
      imageTag(kj::mv(imageTag)) {}

// Docker-specific Port implementation that implements rpc::Container::Port::Server
class ContainerClient::DockerPort final: public rpc::Container::Port::Server {
 public:
  DockerPort(capnp::ByteStreamFactory& byteStreamFactory,
      kj::HttpClient& network,
      kj::String containerName,
      kj::String containerHost,
      uint16_t containerPort)
      : byteStreamFactory(byteStreamFactory),
        network(network),
        containerName(kj::mv(containerName)),
        containerHost(kj::mv(containerHost)),
        containerPort(containerPort) {}

  kj::Promise<void> connect(ConnectContext context) override {
    kj::HttpHeaderTable headerTable;
    kj::HttpHeaders headers(headerTable);
    auto request = network.connect(
        kj::str(containerHost, ":", containerPort), headers, kj::HttpConnectSettings());
    auto upPipe = kj::newOneWayPipe();
    auto upEnd = kj::mv(upPipe.in);
    auto results = context.getResults();
    results.setUp(byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));
    auto downEnd = byteStreamFactory.capnpToKj(context.getParams().getDown());
    co_await kj::joinPromisesFailFast(
        kj::arr(upEnd->pumpTo(*request.connection), request.connection->pumpTo(*downEnd)));
  }

 private:
  capnp::ByteStreamFactory& byteStreamFactory;
  kj::HttpClient& network;
  kj::String containerName;
  kj::String containerHost;
  uint16_t containerPort;
};

kj::Promise<ContainerClient::Response> ContainerClient::dockerApiRequest(
    kj::HttpMethod method, kj::StringPtr endpoint, kj::Maybe<kj::StringPtr> body) {
  kj::HttpHeaderTable::Builder builder;
  const auto userAgent = builder.add("User-Agent");
  const auto accept = builder.add("Accept");
  const auto contentType = builder.add("Content-Type");
  const auto contentLength = builder.add("Content-Length");
  auto headerTable = builder.build();
  kj::HttpHeaders headers(*headerTable);
  headers.set(kj::HttpHeaderId::HOST, "localhost");
  headers.set(userAgent, "workerd/1.0");
  headers.set(accept, "*/*");

  KJ_IF_SOME(requestBody, body) {
    headers.set(contentType, "application/json");
    headers.set(contentLength, kj::str(requestBody.size()));

    auto req = httpClient->request(method, endpoint, headers, requestBody.size());
    {
      auto body = kj::mv(req.body);
      co_await body->write(requestBody.asBytes());
    }
    auto response = co_await req.response;
    auto result = co_await response.body->readAllText();
    co_return Response{.statusCode = response.statusCode, .body = kj::mv(result)};
  } else {
    auto req = httpClient->request(method, endpoint, headers);
    { auto body = kj::mv(req.body); }
    auto response = co_await req.response;
    auto result = co_await response.body->readAllText();
    co_return Response{.statusCode = response.statusCode, .body = kj::mv(result)};
  }
}

kj::Promise<bool> ContainerClient::isContainerRunning() {
  // Docker API v1.50: GET /containers/{id}/json
  auto endpoint = kj::str("/containers/", containerName, "/json");

  auto response = co_await dockerApiRequest(kj::HttpMethod::GET, endpoint);
  // We check if the container with the given name exist, and if it's not,
  // we simply return false while avoiding an unnecessary error.
  if (response.statusCode == 404) {
    co_return false;
  }
  JSG_REQUIRE(response.statusCode == 200, Error, "Container inspect failed");

  // Parse JSON response
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerInspectResponse>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerInspectResponse>();
  codec.decode(response.body, jsonRoot);

  // Look for Status field in the JSON object
  if (jsonRoot.hasState()) {
    auto state = jsonRoot.getState();
    if (state.hasStatus()) {
      auto status = state.getStatus();
      co_return status == "running";
    }
  }

  KJ_FAIL_ASSERT("Docker API returned an unknown response for their inspect endpoint.");
}

kj::Promise<void> ContainerClient::createContainer(
    kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
    kj::Maybe<capnp::List<capnp::Text>::Reader> environment) {
  // Docker API v1.50: POST /containers/create
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(imageTag);
  // Add entrypoint if provided
  KJ_IF_SOME(ep, entrypoint) {
    auto jsonCmd = jsonRoot.initCmd(ep.size());
    for (uint32_t i = 0; i < ep.size(); i++) {
      jsonCmd.set(i, ep[i]);
    }
  }

  // Add environment variables if provided
  KJ_IF_SOME(env, environment) {
    auto jsonEnv = jsonRoot.initEnv(env.size());
    for (uint32_t i = 0; i < env.size(); i++) {
      jsonEnv.set(i, env[i]);
    }
  }

  // Encode to JSON string
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerInspectResponse>();
  kj::String jsonBody = codec.encode(jsonRoot);

  auto endpoint = kj::str("/containers/create?name=", containerName);

  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint, jsonBody.asPtr());

  // statusCode 201 refers to "container created successfully"
  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  if (response.statusCode != 201 && response.statusCode != 409) {
    if (response.statusCode == 404) {
      JSG_FAIL_REQUIRE(Error, "No such image available named ", imageTag);
    } else {
      JSG_FAIL_REQUIRE(Error, "Create container failed with: ", response.body);
    }
  }
}

kj::Promise<void> ContainerClient::startContainer() {
  // Docker API v1.50: POST /containers/{id}/start
  auto endpoint = kj::str("/containers/", containerName, "/start");
  // We have to send an empty body since docker API will throw an error if we don't.
  kj::StringPtr body = "";
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint, body);
  // statusCode 304 refers to "container already started"
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
      "Starting container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::stopContainer() {
  // Docker API v1.50: POST /containers/{id}/stop
  auto endpoint = kj::str("/containers/", containerName, "/stop");
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
  // statusCode 204 refers to "no error"
  // statusCode 304 refers to "container already started"
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
      "Stopping container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::killContainer(uint32_t signal) {
  // TODO: Convert signo to signal string here.
  // Docker API v1.50: POST /containers/{id}/kill
  auto endpoint = kj::str("/containers/", containerName, "/kill?signal=", signal);
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
  // statusCode 409 refers to "container is not running"
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 409, Error,
      "Stopping container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::status(StatusContext context) {
  bool running = co_await isContainerRunning();
  context.getResults().setRunning(running);
}

kj::Promise<void> ContainerClient::start(StartContext context) {
  auto params = context.getParams();

  // Get the lists directly from Cap'n Proto
  kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint = kj::none;
  kj::Maybe<capnp::List<capnp::Text>::Reader> environment = kj::none;

  if (params.hasEntrypoint()) {
    entrypoint = params.getEntrypoint();
  }
  if (params.hasEnvironmentVariables()) {
    environment = params.getEnvironmentVariables();
  }

  co_await createContainer(entrypoint, environment);
  co_await startContainer();
}

kj::Promise<void> ContainerClient::monitor(MonitorContext context) {
  // Docker API v1.50: POST /containers/{id}/wait - wait for container to exit
  auto endpoint = kj::str("/containers/", containerName, "/wait");

  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
  JSG_REQUIRE(
      response.statusCode == 200, Error, "Monitoring container failed with: ", response.body);
  // Parse JSON response
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerMonitorResponse>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerMonitorResponse>();
  codec.decode(response.body, jsonRoot);
  auto statusCode = jsonRoot.getStatusCode();
  JSG_REQUIRE(
      statusCode == 0, Error, "Container exited with unexpected exit code ", kj::str(statusCode));
}

kj::Promise<void> ContainerClient::destroy(DestroyContext context) {
  const bool running = co_await isContainerRunning();
  if (running) {
    co_await stopContainer();
    auto endpoint = kj::str("/containers/", containerName, "?force=true");
    auto response = co_await dockerApiRequest(kj::HttpMethod::DELETE, endpoint);
    // statusCode 204 refers to "no error"
    // statusCode 404 refers to "no such container"
    JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 404, Error,
        "Removing a container failed with: ", response.body);
  }
}

kj::Promise<void> ContainerClient::signal(SignalContext context) {
  auto params = context.getParams();
  co_await killContainer(params.getSigno());
}

kj::Promise<void> ContainerClient::getTcpPort(GetTcpPortContext context) {
  auto params = context.getParams();
  uint16_t port = params.getPort();

  auto results = context.getResults();
  // TODO: Use correct hostname here.
  auto dockerPort = kj::heap<DockerPort>(
      byteStreamFactory, *network, kj::str(containerName), kj::str("localhost"), port);
  results.setPort(kj::mv(dockerPort));
  co_return;
}

kj::Promise<void> ContainerClient::listenTcp(ListenTcpContext context) {
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers - use port mapping instead");
}

}  // namespace workerd::io
