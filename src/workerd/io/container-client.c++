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

template <typename T>
typename T::Builder decodeJsonResponse(kj::String response) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<T>();
  codec.decode(response, jsonRoot);
  return jsonRoot;
}

ContainerClient::ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
    kj::Timer& timer,
    kj::Own<kj::HttpClient> network,
    kj::Own<kj::HttpClient> httpClient,
    kj::String containerName,
    kj::String imageName)
    : byteStreamFactory(byteStreamFactory),
      timer(timer),
      network(kj::mv(network)),
      httpClient(kj::mv(httpClient)),
      containerName(kj::mv(containerName)),
      imageName(kj::mv(imageName)) {}

// Docker-specific Port implementation that implements rpc::Container::Port::Server
class ContainerClient::DockerPort final: public rpc::Container::Port::Server {
 public:
  DockerPort(ContainerClient& containerClient, kj::String containerHost, uint16_t containerPort)
      : containerClient(containerClient),
        containerHost(kj::mv(containerHost)),
        containerPort(containerPort) {}

  kj::Promise<void> connect(ConnectContext context) override {
    kj::HttpHeaderTable headerTable;
    kj::HttpHeaders headers(headerTable);

    // Port mappings might have outdated mappings, we can't know if a connect request
    // fails because the app hasn't finished starting up or because the mapping is outdated.
    // To be safe we should inspect the container to get up to date mappings.
    auto portMappings = kj::get<1>(co_await containerClient.inspectContainer());
    auto maybeMappedPort = portMappings.find(containerPort);
    if (maybeMappedPort == kj::none) {
      throw KJ_EXCEPTION(DISCONNECTED, "connect(): Connection refused: container port not found");
    }
    auto mappedPort = KJ_ASSERT_NONNULL(maybeMappedPort);

    auto request = containerClient.network->connect(
        kj::str(containerHost, ":", mappedPort), headers, kj::HttpConnectSettings());

    auto status = co_await request.status;
    if (status.statusCode != 200) {
      throw KJ_EXCEPTION(DISCONNECTED, "connect(): Connection refused");
    }

    auto upPipe = kj::newOneWayPipe();
    auto upEnd = kj::mv(upPipe.in);
    auto results = context.getResults();
    results.setUp(containerClient.byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));
    auto downEnd = containerClient.byteStreamFactory.capnpToKj(context.getParams().getDown());
    pumpTask = kj::joinPromisesFailFast(
        kj::arr(upEnd->pumpTo(*request.connection), request.connection->pumpTo(*downEnd)))
                   .ignoreResult()
                   .attach(kj::mv(upEnd), kj::mv(request.connection), kj::mv(downEnd));
    co_return;
  }

 private:
  ContainerClient& containerClient;
  kj::String containerHost;
  uint16_t containerPort;
  kj::Maybe<kj::Promise<void>> pumpTask;
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

kj::Promise<kj::Tuple<bool, kj::HashMap<uint16_t, uint16_t>>> ContainerClient::inspectContainer() {
  // Docker API: GET /containers/{id}/json
  auto endpoint = kj::str("/containers/", containerName, "/json");

  auto response = co_await dockerApiRequest(kj::HttpMethod::GET, endpoint);
  // We check if the container with the given name exist, and if it's not,
  // we simply return false while avoiding an unnecessary error.
  if (response.statusCode == 404) {
    co_return kj::tuple(false, kj::HashMap<uint16_t, uint16_t>{});
  }
  JSG_REQUIRE(response.statusCode == 200, Error, "Container inspect failed");
  // Parse JSON response
  auto jsonRoot =
      decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(kj::mv(response.body));
  kj::HashMap<uint16_t, uint16_t> portMappings;
  for (auto portMapping: jsonRoot.getNetworkSettings().getPorts().getObject()) {
    auto port = portMapping.getName();
    auto portMappingSlashIndex = KJ_ASSERT_NONNULL(port.asString().find("/"));
    auto portNumberStr = port.asString().slice(0, portMappingSlashIndex);
    auto portNumber1 = kj::str(portNumberStr);
    auto portNumber = portNumber1.parseAs<uint16_t>();
    auto mappedPortStr = portMapping.getValue().getArray()[0].getObject()[1].getValue().getString();
    auto mappedPort = mappedPortStr.asString().parseAs<uint16_t>();
    portMappings.insert(portNumber, mappedPort);
  }

  // Look for Status field in the JSON object
  JSG_REQUIRE(jsonRoot.hasState(), Error, "Malformed ContainerInspect response");
  auto state = jsonRoot.getState();
  JSG_REQUIRE(state.hasStatus(), Error, "Malformed ContainerInspect response");
  auto status = state.getStatus();
  bool running = status == "running";
  co_return kj::tuple(running, kj::mv(portMappings));
}

kj::Promise<void> ContainerClient::createContainer(
    kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
    kj::Maybe<capnp::List<capnp::Text>::Reader> environment) {
  // Docker API: POST /containers/create
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(imageName);
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

  auto hostConfig = jsonRoot.initHostConfig();
  hostConfig.setPublishAllPorts(true);
  hostConfig.initRestartPolicy().setName("on-failure");

  // Encode to JSON string
  kj::String jsonBody = codec.encode(jsonRoot);
  auto endpoint = kj::str("/containers/create?name=", containerName);
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint, jsonBody.asPtr());

  // statusCode 201 refers to "container created successfully"
  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  // Both are fine, so long as the container exists. Though we might want to call destroy and
  // recreate on 409 in the future.
  if (response.statusCode != 201 && response.statusCode != 409) {
    if (response.statusCode == 404) {
      JSG_FAIL_REQUIRE(Error, "No such image available named ", imageName);
    } else {
      JSG_FAIL_REQUIRE(Error, "Create container failed with: ", response.body);
    }
  }
}

kj::Promise<void> ContainerClient::startContainer() {
  // Docker API: POST /containers/{id}/start
  auto endpoint = kj::str("/containers/", containerName, "/start");
  // We have to send an empty body since docker API will throw an error if we don't.
  kj::StringPtr body = "";
  try {
    auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint, body);
    // statusCode 204 refers to "no error"
    // statusCode 304 refers to "container already started"
    // Both are fine
    JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
        "Starting container failed with: ", response.body);
  } catch (...) {
    auto e = kj::getCaughtExceptionAsKj();
    kj::throwFatalException(kj::mv(e));
  }
}

kj::Promise<void> ContainerClient::stopContainer() {
  // Docker API: POST /containers/{id}/stop
  auto endpoint = kj::str("/containers/", containerName, "/stop");
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
  // statusCode 204 refers to "no error"
  // statusCode 304 refers to "container already stopped"
  // Both are fine
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
      "Stopping container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::killContainer(uint32_t signal) {
  // TODO: Convert signo to signal string here.
  // Docker API: POST /containers/{id}/kill
  auto endpoint = kj::str("/containers/", containerName, "/kill?signal=", signal);
  auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
  // statusCode 409 refers to "container is not running"
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 409, Error,
      "Stopping container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::status(StatusContext context) {
  bool running = kj::get<0>(co_await inspectContainer());
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
  // Docker API: POST /containers/{id}/wait - wait for container to exit
  auto endpoint = kj::str("/containers/", containerName, "/wait");
  // Monitor is often called right after start but the api layer's start does not await the RPC's
  // start response. That means that the createContainer call might not have even started yet.
  // If it hasn't, we'll give it 3 tries before failing.
  for (int i = 0; i < 3; i++) {
    auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
    if (response.statusCode == 404) {
      co_await timer.afterDelay(1 * kj::SECONDS);
      continue;
    }
    JSG_REQUIRE(response.statusCode == 200, Error,
        "Monitoring container failed with: ", response.statusCode, response.body);
    // Parse JSON response
    auto jsonRoot =
        decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(kj::mv(response.body));
    auto statusCode = jsonRoot.getStatusCode();
    JSG_REQUIRE(
        statusCode == 0, Error, "Container exited with unexpected exit code ", kj::str(statusCode));
    co_return;
  }
  JSG_FAIL_REQUIRE(Error, "Monitor failed to find container");
}

kj::Promise<void> ContainerClient::destroy(DestroyContext context) {
  bool running = kj::get<0>(co_await inspectContainer());
  if (running) {
    co_await stopContainer();
    auto endpoint = kj::str("/containers/", containerName, "?force=true");
    auto response = co_await dockerApiRequest(kj::HttpMethod::DELETE, endpoint);
    // statusCode 204 refers to "no error"
    // statusCode 404 refers to "no such container"
    // Both of which are fine for us since we're tearing down the container anyway.
    JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 404, Error,
        "Removing a container failed with: ", response.body);
    {
      auto endpoint = kj::str("/containers/", containerName, "/wait");
      auto response = co_await dockerApiRequest(kj::HttpMethod::POST, endpoint);
      JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
          "Waiting for container removal failed with: ", response.statusCode, response.body);
    }
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
  auto dockerPort = kj::heap<DockerPort>(*this, kj::str("localhost"), port);
  results.setPort(kj::mv(dockerPort));
  co_return;
}

kj::Promise<void> ContainerClient::listenTcp(ListenTcpContext context) {
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers - use port mapping instead");
}

}  // namespace workerd::io
