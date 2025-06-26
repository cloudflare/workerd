// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <workerd/io/container.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/docker-api.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/exception.h>
#include <kj/string.h>

namespace workerd::server {

namespace {
kj::StringPtr signalToString(uint32_t signal) {
  switch (signal) {
    case 1:
      return "SIGHUP"_kj;  // Hangup
    case 2:
      return "SIGINT"_kj;  // Interrupt
    case 3:
      return "SIGQUIT"_kj;  // Quit
    case 4:
      return "SIGILL"_kj;  // Illegal instruction
    case 5:
      return "SIGTRAP"_kj;  // Trace trap
    case 6:
      return "SIGABRT"_kj;  // Abort
    case 7:
      return "SIGBUS"_kj;  // Bus error
    case 8:
      return "SIGFPE"_kj;  // Floating point exception
    case 9:
      return "SIGKILL"_kj;  // Kill
    case 10:
      return "SIGUSR1"_kj;  // User signal 1
    case 11:
      return "SIGSEGV"_kj;  // Segmentation violation
    case 12:
      return "SIGUSR2"_kj;  // User signal 2
    case 13:
      return "SIGPIPE"_kj;  // Broken pipe
    case 14:
      return "SIGALRM"_kj;  // Alarm clock
    case 15:
      return "SIGTERM"_kj;  // Termination
    case 16:
      return "SIGSTKFLT"_kj;  // Stack fault (Linux)
    case 17:
      return "SIGCHLD"_kj;  // Child status changed
    case 18:
      return "SIGCONT"_kj;  // Continue
    case 19:
      return "SIGSTOP"_kj;  // Stop
    case 20:
      return "SIGTSTP"_kj;  // Terminal stop
    case 21:
      return "SIGTTIN"_kj;  // Background read from tty
    case 22:
      return "SIGTTOU"_kj;  // Background write to tty
    case 23:
      return "SIGURG"_kj;  // Urgent condition on socket
    case 24:
      return "SIGXCPU"_kj;  // CPU limit exceeded
    case 25:
      return "SIGXFSZ"_kj;  // File size limit exceeded
    case 26:
      return "SIGVTALRM"_kj;  // Virtual alarm clock
    case 27:
      return "SIGPROF"_kj;  // Profiling alarm clock
    case 28:
      return "SIGWINCH"_kj;  // Window size change
    case 29:
      return "SIGIO"_kj;  // I/O now possible
    case 30:
      return "SIGPWR"_kj;  // Power failure restart (Linux)
    case 31:
      return "SIGSYS"_kj;  // Bad system call
    default:
      return "SIGKILL"_kj;
  }
}
}  // namespace

template <typename T>
typename T::Builder decodeJsonResponse(kj::StringPtr response) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<T>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<T>();
  codec.decode(response, jsonRoot);
  return jsonRoot;
}

ContainerClient::ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
    kj::Timer& timer,
    kj::Network& network,
    kj::String dockerPath,
    kj::String containerName,
    kj::String imageName,
    kj::TaskSet& waitUntilTasks)
    : byteStreamFactory(byteStreamFactory),
      timer(timer),
      network(network),
      dockerPath(kj::mv(dockerPath)),
      containerName(kj::encodeUriComponent(kj::mv(containerName))),
      imageName(kj::mv(imageName)),
      waitUntilTasks(waitUntilTasks) {}

ContainerClient::~ContainerClient() noexcept(false) {
  waitUntilTasks.add(dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", containerName, "?force=true"))
                         .ignoreResult());
}

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
    const auto [_running, portMappings] = co_await containerClient.inspectContainer();
    auto maybeMappedPort = portMappings.find(containerPort);
    if (maybeMappedPort == kj::none) {
      throw JSG_KJ_EXCEPTION(DISCONNECTED, Error,
          "connect(): Connection refused: container port not found. Make sure you exposed the port in your container definition.");
    }
    auto mappedPort = KJ_ASSERT_NONNULL(maybeMappedPort);

    auto address =
        co_await containerClient.network.parseAddress(kj::str(containerHost, ":", mappedPort));
    auto connection = co_await address->connect();

    auto upPipe = kj::newOneWayPipe();
    auto upEnd = kj::mv(upPipe.in);
    auto results = context.getResults();
    results.setUp(containerClient.byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));
    auto downEnd = containerClient.byteStreamFactory.capnpToKj(context.getParams().getDown());
    pumpTask =
        kj::joinPromisesFailFast(kj::arr(upEnd->pumpTo(*connection), connection->pumpTo(*downEnd)))
            .ignoreResult()
            .attach(kj::mv(upEnd), kj::mv(connection), kj::mv(downEnd));
    co_return;
  }

 private:
  // ContainerClient is owned by the Worker::Actor and keeps it alive.
  ContainerClient& containerClient;
  kj::String containerHost;
  uint16_t containerPort;
  kj::Maybe<kj::Promise<void>> pumpTask;
};

kj::Promise<ContainerClient::Response> ContainerClient::dockerApiRequest(kj::Network& network,
    kj::String dockerPath,
    kj::HttpMethod method,
    kj::String endpoint,
    kj::Maybe<kj::String> body) {
  kj::HttpHeaderTable headerTable;
  auto address = co_await network.parseAddress(dockerPath);
  auto connection = co_await address->connect();
  auto httpClient = kj::newHttpClient(headerTable, *connection).attach(kj::mv(connection));
  kj::HttpHeaders headers(headerTable);
  headers.set(kj::HttpHeaderId::HOST, "localhost");

  KJ_IF_SOME(requestBody, body) {
    headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json");
    headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(requestBody.size()));

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

kj::Promise<ContainerClient::InspectResponse> ContainerClient::inspectContainer() {
  // Docker API: GET /containers/{id}/json
  auto endpoint = kj::str("/containers/", containerName, "/json");

  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::mv(endpoint));
  // We check if the container with the given name exist, and if it's not,
  // we simply return false while avoiding an unnecessary error.
  if (response.statusCode == 404) {
    co_return InspectResponse{.isRunning = false, .ports = {}};
  }
  JSG_REQUIRE(response.statusCode == 200, Error, "Container inspect failed");
  // Parse JSON response
  auto jsonRoot = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(response.body);
  kj::HashMap<uint16_t, uint16_t> portMappings;
  for (auto portMapping: jsonRoot.getNetworkSettings().getPorts().getObject()) {
    auto port = portMapping.getName();
    // We need to get "8080" from "8080/tcp"
    auto rawPort = port.asString().slice(0, KJ_ASSERT_NONNULL(port.asString().find("/")));
    auto portNumber = kj::str(rawPort).parseAs<uint16_t>();
    uint16_t number;
    {
      // We need to retrieve "HostPort" from the following JSON structure
      //
      // "Ports": {
      // 	"8080/tcp": [
      // 		{
      // 			"HostIp": "0.0.0.0",
      // 			"HostPort": "55000"
      // 		}
      // 	]
      // },
      //
      auto array = portMapping.getValue().getArray();
      JSG_REQUIRE(array.size() > 0, Error, "Malformed ContainerInspect port mapping response");
      auto obj = array[0].getObject();
      JSG_REQUIRE(obj.size() > 1, Error, "Malformed ContainerInspect port mapping object");
      auto mappedPort = obj[1].getValue().getString();
      number = mappedPort.asString().parseAs<uint16_t>();
    }
    portMappings.insert(portNumber, number);
  }

  // Look for Status field in the JSON object
  JSG_REQUIRE(jsonRoot.hasState(), Error, "Malformed ContainerInspect response");
  auto state = jsonRoot.getState();
  JSG_REQUIRE(state.hasStatus(), Error, "Malformed ContainerInspect response");
  auto status = state.getStatus();
  bool running = status == "running";
  co_return InspectResponse{.isRunning = running, .ports = kj::mv(portMappings)};
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
    for (uint32_t i: kj::zeroTo(ep.size())) {
      jsonCmd.set(i, ep[i]);
    }
  }

  // Add environment variables if provided
  KJ_IF_SOME(env, environment) {
    auto jsonEnv = jsonRoot.initEnv(env.size());
    for (uint32_t i: kj::zeroTo(env.size())) {
      jsonEnv.set(i, env[i]);
    }
  }

  auto hostConfig = jsonRoot.initHostConfig();
  // We need to publish all ports to properly get the mapped port number locally
  hostConfig.setPublishAllPorts(true);
  // We need to set a restart policy to avoid having ambiguous states
  // where the container we're managing is stuck at "exited" state.
  hostConfig.initRestartPolicy().setName("on-failure");

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/create?name=", containerName), codec.encode(jsonRoot));

  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  // In that case we destroy and re-create the container.
  if (response.statusCode == 409) {
    co_await destroyContainer();
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/create?name=", containerName), codec.encode(jsonRoot));
  }

  // statusCode 201 refers to "container created successfully"
  if (response.statusCode != 201) {
    JSG_REQUIRE(response.statusCode != 404, Error, "No such image available named ", imageName);
    JSG_REQUIRE(response.statusCode != 409, Error, "Container already exists");
    JSG_FAIL_REQUIRE(
        Error, "Create container failed with [", response.statusCode, "] ", response.body);
  }
}

kj::Promise<void> ContainerClient::startContainer() {
  // Docker API: POST /containers/{id}/start
  auto endpoint = kj::str("/containers/", containerName, "/start");
  // We have to send an empty body since docker API will throw an error if we don't.
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint), kj::str(""));
  // statusCode 304 refers to "container already started"
  JSG_REQUIRE(response.statusCode != 304, Error, "Container already started");
  // statusCode 204 refers to "no error"
  JSG_REQUIRE(response.statusCode == 204, Error, "Starting container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::stopContainer() {
  // Docker API: POST /containers/{id}/stop
  auto endpoint = kj::str("/containers/", containerName, "/stop");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));
  // statusCode 204 refers to "no error"
  // statusCode 304 refers to "container already stopped"
  // Both are fine to avoid when stop container is called.
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
      "Stopping container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::killContainer(uint32_t signal) {
  // Docker API: POST /containers/{id}/kill
  auto endpoint = kj::str("/containers/", containerName, "/kill?signal=", signalToString(signal));
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));
  // statusCode 409 refers to "container is not running"
  // We should not throw an error when the container is already not running.
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 409, Error,
      "Stopping container failed with: ", response.body);
}

// This method assumes that the container is already running to avoid sending
// unnecessary requests for checking the status of the container (via /inspect).
// if the container doesn't exist it will return 404.
// Ref: https://docs.docker.com/reference/api/engine/version/v1.50/#tag/Container/operation/ContainerDelete
kj::Promise<void> ContainerClient::destroyContainer() {
  auto endpoint = kj::str("/containers/", containerName, "?force=true");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint));
  // statusCode 204 refers to "no error"
  // statusCode 404 refers to "no such container"
  // Both of which are fine for us since we're tearing down the container anyway.
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 404, Error,
      "Removing a container failed with: ", response.body);
  // Do not send a wait request if container doesn't exist. This avoids sending an
  // unnecessary request.
  if (response.statusCode == 204) {
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/", containerName, "/wait?condition=removed"));
    JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
        "Waiting for container removal failed with: ", response.statusCode, response.body);
  }
}

kj::Promise<void> ContainerClient::status(StatusContext context) {
  const auto [isRunning, _ports] = co_await inspectContainer();
  context.getResults().setRunning(isRunning);
}

kj::Promise<void> ContainerClient::start(StartContext context) {
  const auto params = context.getParams();

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
  // Monitor is often called right after start but the api layer's start does not await the RPC's
  // start response. That means that the createContainer call might not have even started yet.
  // If it hasn't, we'll give it 3 tries before failing.
  auto results = context.getResults();
  for (int i = 0; i < 3; i++) {
    // Docker API: POST /containers/{id}/wait - wait for container to exit
    auto endpoint = kj::str("/containers/", containerName, "/wait");

    auto response = co_await dockerApiRequest(
        network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));
    if (response.statusCode == 404) {
      co_await timer.afterDelay(1 * kj::SECONDS);
      continue;
    }
    JSG_REQUIRE(response.statusCode == 200, Error,
        "Monitoring container failed with: ", response.statusCode, response.body);
    // Parse JSON response
    auto jsonRoot = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(response.body);
    auto statusCode = jsonRoot.getStatusCode();
    results.setExitCode(statusCode);
    co_return;
  }
  JSG_FAIL_REQUIRE(Error, "Monitor failed to find container");
}

kj::Promise<void> ContainerClient::destroy(DestroyContext context) {
  const auto [running, _ports] = co_await inspectContainer();
  if (running) {
    co_await destroyContainer();
  }
}

kj::Promise<void> ContainerClient::signal(SignalContext context) {
  const auto params = context.getParams();
  co_await killContainer(params.getSigno());
}

kj::Promise<void> ContainerClient::getTcpPort(GetTcpPortContext context) {
  const auto params = context.getParams();
  uint16_t port = params.getPort();
  auto results = context.getResults();
  auto dockerPort = kj::heap<DockerPort>(*this, kj::str("localhost"), port);
  results.setPort(kj::mv(dockerPort));
  co_return;
}

kj::Promise<void> ContainerClient::listenTcp(ListenTcpContext context) {
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers - use port mapping instead");
}

}  // namespace workerd::server
