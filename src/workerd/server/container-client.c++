// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/docker-api.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/cidr.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/exception.h>
#include <kj/string.h>

namespace workerd::server {

namespace {

struct ParsedAddress {
  kj::CidrRange cidr;
  kj::Maybe<uint16_t> port;
};

struct HostAndPort {
  kj::String host;
  kj::Maybe<uint16_t> port;
};

// Strips a port suffix from a string, returning the host and port separately.
// For IPv6, expects brackets: "[::1]:8080" -> ("::1", 8080)
// For IPv4: "10.0.0.1:8080" -> ("10.0.0.1", 8080)
// If no port, returns the host as-is with no port.
HostAndPort stripPort(kj::StringPtr str) {
  if (str.startsWith("[")) {
    // Bracketed IPv6: "[ipv6]" or "[ipv6]:port"
    size_t closeBracket =
        KJ_REQUIRE_NONNULL(str.findLast(']'), "Unclosed '[' in address string.", str);

    auto host = str.slice(1, closeBracket);

    if (str.size() > closeBracket + 1) {
      KJ_REQUIRE(
          str.slice(closeBracket + 1).startsWith(":"), "Expected port suffix after ']'.", str);
      auto port = KJ_REQUIRE_NONNULL(
          str.slice(closeBracket + 2).tryParseAs<uint16_t>(), "Invalid port number.", str);
      return {kj::str(host), port};
    }
    return {kj::str(host), kj::none};
  }

  // No brackets - check if there's exactly one colon (IPv4 with port)
  // IPv6 without brackets has 2+ colons and no port suffix supported
  KJ_IF_SOME(colonPos, str.findLast(':')) {
    auto afterColon = str.slice(colonPos + 1);
    KJ_IF_SOME(port, afterColon.tryParseAs<uint16_t>()) {
      // Valid port - but only treat as port for IPv4 (check no other colons before)
      auto beforeColon = str.first(colonPos);
      if (beforeColon.findFirst(':') == kj::none) {
        return {kj::str(beforeColon), port};
      }
    }
  }

  return {kj::str(str), kj::none};
}

// Build a CidrRange from a host string, adding /32 or /128 prefix if not present.
kj::CidrRange makeCidr(kj::StringPtr host) {
  if (host.findFirst('/') != kj::none) {
    return kj::CidrRange(host);
  }
  // No CIDR prefix - add /32 for IPv4, /128 for IPv6
  bool isIpv6 = host.findFirst(':') != kj::none;
  return kj::CidrRange(kj::str(host, isIpv6 ? "/128" : "/32"));
}

// Parses "host[:port]" strings. Handles:
// - IPv4: "10.0.0.1", "10.0.0.1:8080", "10.0.0.0/8", "10.0.0.0/8:8080"
// - IPv6 with brackets: "[::1]", "[::1]:8080", "[fe80::1]", "[fe80::/10]:8080"
// - IPv6 without brackets: "::1", "fe80::1", "fe80::/10"
ParsedAddress parseHostPort(kj::StringPtr str) {
  auto hostAndPort = stripPort(str);
  return {makeCidr(hostAndPort.host), hostAndPort.port};
}

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
T::Builder decodeJsonResponse(kj::StringPtr response) {
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
    kj::Maybe<kj::String> containerEgressInterceptorImage,
    kj::TaskSet& waitUntilTasks,
    kj::Function<void()> cleanupCallback,
    ChannelTokenHandler& channelTokenHandler)
    : byteStreamFactory(byteStreamFactory),
      timer(timer),
      network(network),
      dockerPath(kj::mv(dockerPath)),
      containerName(kj::encodeUriComponent(kj::str(containerName))),
      sidecarContainerName(kj::encodeUriComponent(kj::str(containerName, "-proxy"))),
      imageName(kj::mv(imageName)),
      containerEgressInterceptorImage(kj::mv(containerEgressInterceptorImage)),
      waitUntilTasks(waitUntilTasks),
      cleanupCallback(kj::mv(cleanupCallback)),
      channelTokenHandler(channelTokenHandler) {}

ContainerClient::~ContainerClient() noexcept(false) {
  stopEgressListener();

  // Call the cleanup callback to remove this client from the ActorNamespace map
  cleanupCallback();

  // Sidecar shares main container's network namespace, so must be destroyed first
  waitUntilTasks.add(dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", sidecarContainerName, "?force=true"))
                         .ignoreResult());

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

// HTTP service that handles HTTP CONNECT requests from the container sidecar (proxy-everything).
// When the sidecar intercepts container egress traffic, it sends HTTP CONNECT to this service.
// After accepting the CONNECT, the tunnel carries the actual HTTP request from the container,
// which we parse and forward to the appropriate SubrequestChannel based on egressMappings.
// Inner HTTP service that handles requests inside the CONNECT tunnel.
// Forwards requests to the worker binding via SubrequestChannel.
class InnerEgressService final: public kj::HttpService {
 public:
  InnerEgressService(IoChannelFactory::SubrequestChannel& channel): channel(channel) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    IoChannelFactory::SubrequestMetadata metadata;
    auto worker = channel.startRequest(kj::mv(metadata));
    co_await worker->request(method, url, headers, requestBody, response);
  }

 private:
  IoChannelFactory::SubrequestChannel& channel;
};

// Outer HTTP service that handles CONNECT requests from the sidecar.
class EgressHttpService final: public kj::HttpService {
 public:
  EgressHttpService(ContainerClient& containerClient, kj::HttpHeaderTable& headerTable)
      : containerClient(containerClient),
        headerTable(headerTable) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    // Regular HTTP requests are not expected - we only handle CONNECT
    co_return co_await response.sendError(405, "Method Not Allowed", headerTable);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    auto destAddr = kj::str(host);

    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    auto mapping = containerClient.findEgressMapping(destAddr, /*defaultPort=*/80);

    KJ_IF_SOME(channel, mapping) {
      // Layer an HttpServer on top of the tunnel to handle HTTP parsing/serialization
      auto innerService = kj::heap<InnerEgressService>(*channel);
      auto innerServer =
          kj::heap<kj::HttpServer>(containerClient.timer, headerTable, *innerService);

      co_await innerServer->listenHttpCleanDrain(connection);

      co_return;
    }

    if (!containerClient.internetEnabled) {
      connection.shutdownWrite();
      co_return;
    }

    // No egress mapping and internet enabled, so forward via raw TCP
    auto addr = co_await containerClient.network.parseAddress(destAddr);
    auto destConn = co_await addr->connect();

    auto connToDestination = connection.pumpTo(*destConn).then(
        [&destConn = *destConn](uint64_t) { destConn.shutdownWrite(); });

    auto destinationToConn =
        destConn->pumpTo(connection).then([&connection](uint64_t) { connection.shutdownWrite(); });

    co_await kj::joinPromisesFailFast(
        kj::arr(kj::mv(connToDestination), kj::mv(destinationToConn)));
    co_return;
  }

 private:
  ContainerClient& containerClient;
  kj::HttpHeaderTable& headerTable;
};

// The name of the docker workerd network. All containers spawned by Workerd
// will be attached to this network.
constexpr kj::StringPtr WORKERD_NETWORK_NAME = "workerd-network"_kj;

kj::Promise<ContainerClient::IPAMConfigResult> ContainerClient::getDockerBridgeIPAMConfig() {
  // First, try to find or create the workerd-network
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::GET,
      kj::str("/networks/", WORKERD_NETWORK_NAME));

  if (response.statusCode == 404) {
    // Network doesn't exist, create it
    // Equivalent to: docker network create -d bridge --ipv6 workerd-network
    co_await createWorkerdNetwork();
    // Re-fetch the network to get the gateway
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::GET,
        kj::str("/networks/", WORKERD_NETWORK_NAME));
  }

  if (response.statusCode == 200) {
    auto jsonRoot = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(response.body);
    auto ipamConfig = jsonRoot.getIpam().getConfig();
    if (ipamConfig.size() > 0) {
      auto config = ipamConfig[0];
      co_return IPAMConfigResult{
        .gateway = kj::str(config.getGateway()),
        .subnet = kj::str(config.getSubnet()),
      };
    }
  }

  JSG_FAIL_REQUIRE(Error,
      "Failed to get workerd-network. "
      "Status: ",
      response.statusCode, ", Body: ", response.body);
}

kj::Promise<void> ContainerClient::createWorkerdNetwork() {
  // Equivalent to: docker network create -d bridge --ipv6 workerd-network
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::NetworkCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::NetworkCreateRequest>();
  jsonRoot.setName(WORKERD_NETWORK_NAME);
  jsonRoot.setDriver("bridge");
  jsonRoot.setEnableIpv6(true);

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/networks/create"), codec.encode(jsonRoot));

  if (response.statusCode != 201 && response.statusCode != 409) {
    JSG_FAIL_REQUIRE(Error,
        "Failed to create workerd-network."
        "Status: ",
        response.statusCode, ", Body: ", response.body);
  }
}

kj::Promise<uint16_t> ContainerClient::startEgressListener(kj::StringPtr listenAddress) {
  auto service = kj::heap<EgressHttpService>(*this, headerTable);
  auto httpServer = kj::heap<kj::HttpServer>(timer, headerTable, *service);
  auto& httpServerRef = *httpServer;

  egressHttpServer = httpServer.attach(kj::mv(service));

  // Listen on the Docker bridge gateway IP with port 0 to let the OS pick a free port
  auto addr = co_await network.parseAddress(kj::str(listenAddress, ":0"));
  auto listener = addr->listen();

  uint16_t chosenPort = listener->getPort();

  egressListenerTask = httpServerRef.listenHttp(*listener)
                           .attach(kj::mv(listener))
                           .eagerlyEvaluate([](kj::Exception&& e) {
    LOG_EXCEPTION(
        "Workerd could not listen in the TCP port to proxy traffic off the docker container", e);
  });

  co_return chosenPort;
}

void ContainerClient::stopEgressListener() {
  egressListenerTask = kj::none;
  egressHttpServer = kj::none;
}

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
  headers.setPtr(kj::HttpHeaderId::HOST, "localhost");

  KJ_IF_SOME(requestBody, body) {
    headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "application/json");
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
  // Treat both "running" and "restarting" as running. The "restarting" state occurs when
  // Docker is automatically restarting a container (due to restart policy). From the user's
  // perspective, a restarting container is still "alive" and should be treated as running
  // so that start() correctly refuses to start a duplicate and destroy() can clean it up.
  bool running = status == "running" || status == "restarting";
  co_return InspectResponse{.isRunning = running, .ports = kj::mv(portMappings)};
}

kj::Promise<void> ContainerClient::createContainer(
    kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
    kj::Maybe<capnp::List<capnp::Text>::Reader> environment,
    rpc::Container::StartParams::Reader params) {
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

  auto envSize = environment.map([](auto& env) { return env.size(); }).orDefault(0);
  auto jsonEnv = jsonRoot.initEnv(envSize + kj::size(defaultEnv));

  co_await createWorkerdNetwork();

  KJ_IF_SOME(env, environment) {
    for (uint32_t i: kj::zeroTo(env.size())) {
      jsonEnv.set(i, env[i]);
    }
  }

  for (uint32_t i: kj::zeroTo(kj::size(defaultEnv))) {
    jsonEnv.set(envSize + i, defaultEnv[i]);
  }

  auto hostConfig = jsonRoot.initHostConfig();
  // We need to publish all ports to properly get the mapped port number locally
  hostConfig.setPublishAllPorts(true);
  // We need to set a restart policy to avoid having ambiguous states
  // where the container we're managing is stuck at "exited" state.
  hostConfig.initRestartPolicy().setName("on-failure");
  // Add host.docker.internal mapping so containers can reach the host
  // This is equivalent to --add-host=host.docker.internal:host-gateway
  auto extraHosts = hostConfig.initExtraHosts(1);
  auto ipamConfigForHost = co_await getDockerBridgeIPAMConfig();
  extraHosts.set(0, kj::str("host.docker.internal:", ipamConfigForHost.gateway));
  // Connect the container to the workerd-network for IPv6 support and container isolation
  hostConfig.setNetworkMode(WORKERD_NETWORK_NAME);

  // When containersPidNamespace is NOT enabled, use host PID namespace for backwards compatibility.
  // This allows the container to see processes on the host.
  if (!params.getCompatibilityFlags().getContainersPidNamespace()) {
    hostConfig.setPidMode("host");
  }

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/create?name=", containerName), codec.encode(jsonRoot));

  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  // In that case we destroy and re-create the container. We retry a few times with delays
  // because Docker may take a moment to fully release the container name after removal.
  constexpr int MAX_RETRIES = 3;
  constexpr auto RETRY_DELAY = 100 * kj::MILLISECONDS;

  for (int attempt = 0; response.statusCode == 409 && attempt < MAX_RETRIES; ++attempt) {
    co_await destroyContainer();
    co_await timer.afterDelay(RETRY_DELAY);
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
  auto endpoint = kj::str("/containers/", containerName, "/kill?signal=", signalToString(signal));
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));
  // statusCode 409 refers to "container is not running"
  // We should not throw an error when the container is already not running.
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 409, Error,
      "Stopping container failed with: ", response.body);
}

// Destroys the container.
// No-op when the container does not exist.
// Wait for the container to actually be stopped and removed when it exists.
kj::Promise<void> ContainerClient::destroyContainer() {
  auto endpoint = kj::str("/containers/", containerName, "?force=true");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint));
  // statusCode 204 refers to "no error"
  // statusCode 404 refers to "no such container"
  // statusCode 409 refers to "removal already in progress" (race between concurrent destroys)
  // All of which are fine for us since we're tearing down the container anyway.
  JSG_REQUIRE(
      response.statusCode == 204 || response.statusCode == 404 || response.statusCode == 409, Error,
      "Removing a container failed with: ", response.body);
  // Do not send a wait request if container doesn't exist. This avoids sending an
  // unnecessary request.
  if (response.statusCode == 204 || response.statusCode == 409) {
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/", containerName, "/wait?condition=removed"));
    JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
        "Waiting for container removal failed with: ", response.statusCode, response.body);
  }
}

// Creates the sidecar container for egress proxy.
// The sidecar shares the network namespace with the main container and runs
// proxy-everything to intercept and proxy egress traffic.
kj::Promise<void> ContainerClient::createSidecarContainer(
    uint16_t egressPort, kj::String networkCidr) {
  // Equivalent to: docker run --cap-add=NET_ADMIN --network container:$(CONTAINER) ...
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  auto& image = KJ_ASSERT_NONNULL(containerEgressInterceptorImage,
      "containerEgressInterceptorImage must be configured to use egress interception. "
      "Set it in the localDocker configuration.");
  jsonRoot.setImage(image);

  auto cmd = jsonRoot.initCmd(4);
  cmd.set(0, "--http-egress-port");
  cmd.set(1, kj::str(egressPort));
  cmd.set(2, "--docker-gateway-cidr");
  cmd.set(3, networkCidr);

  auto hostConfig = jsonRoot.initHostConfig();
  // Share network namespace with the main container
  hostConfig.setNetworkMode(kj::str("container:", containerName));

  // Sidecar needs NET_ADMIN capability for iptables/TPROXY
  auto capAdd = hostConfig.initCapAdd(1);
  capAdd.set(0, "NET_ADMIN");
  hostConfig.setAutoRemove(true);

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/create?name=", sidecarContainerName), codec.encode(jsonRoot));

  if (response.statusCode == 409) {
    // Already created, nothing to do
    co_return;
  }

  if (response.statusCode != 201) {
    JSG_REQUIRE(response.statusCode != 404, Error, "No such image available named ", image,
        ". Please ensure the container egress interceptor image is built and available.");
    JSG_FAIL_REQUIRE(Error, "Failed to create the networking sidecar [", response.statusCode, "] ",
        response.body);
  }
}

kj::Promise<void> ContainerClient::startSidecarContainer() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "/start");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint), kj::str(""));
  JSG_REQUIRE(response.statusCode == 204, Error,
      "Starting network sidecar container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::destroySidecarContainer() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "?force=true");
  co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint));
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", sidecarContainerName, "/wait?condition=removed"));
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
      "Destroying docker network sidecar container failed: ", response.statusCode, response.body);
}

kj::Promise<void> ContainerClient::status(StatusContext context) {
  const auto [isRunning, _ports] = co_await inspectContainer();
  containerStarted.store(isRunning, std::memory_order_release);
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

  internetEnabled = params.getEnableInternet();

  co_await createContainer(entrypoint, environment, params);
  co_await startContainer();

  // Opt in to the proxy sidecar container only if the user has configured egressMappings
  // for now. In the future, it will always run when a user container is running
  if (!egressMappings.empty()) {
    // The user container will be blocked on network connectivity until this finishes.
    // When workerd-network is more battle-tested and goes out of experimental so it's non-optional,
    // we should make the sidecar start first and _then_ make the user container join the sidecar network.
    co_await ensureSidecarStarted();
  }

  containerStarted.store(true, std::memory_order_release);
}

kj::Promise<void> ContainerClient::monitor(MonitorContext context) {
  // Monitor is often called right after start but the api layer's start does not await the RPC's
  // start response. That means that the createContainer call might not have even started yet.
  // If it hasn't, we'll give it 3 tries before failing.
  auto results = context.getResults();
  for (int i = 0; i < 3; i++) {
    auto endpoint = kj::str("/containers/", containerName, "/wait");

    auto response = co_await dockerApiRequest(
        network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));
    if (response.statusCode == 404) {
      co_await timer.afterDelay(1 * kj::SECONDS);
      continue;
    }

    containerStarted.store(false, std::memory_order_release);
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
  // Sidecar shares main container's network namespace, so must be destroyed first
  co_await destroySidecarContainer();
  co_await destroyContainer();
}

kj::Promise<void> ContainerClient::signal(SignalContext context) {
  const auto params = context.getParams();
  co_await killContainer(params.getSigno());
}

kj::Promise<void> ContainerClient::setInactivityTimeout(SetInactivityTimeoutContext context) {
  auto params = context.getParams();
  auto durationMs = params.getDurationMs();

  JSG_REQUIRE(
      durationMs > 0, Error, "setInactivityTimeout() requires durationMs > 0, got ", durationMs);

  auto timeout = durationMs * kj::MILLISECONDS;

  // Add a timer task that holds a reference to this ContainerClient.
  waitUntilTasks.add(timer.afterDelay(timeout).then([self = kj::addRef(*this)]() {
    // This callback does nothing but drop the reference
  }));

  co_return;
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

kj::Maybe<workerd::IoChannelFactory::SubrequestChannel*> ContainerClient::findEgressMapping(
    kj::StringPtr destAddr, uint16_t defaultPort) {
  auto hostAndPort = stripPort(destAddr);
  uint16_t port = hostAndPort.port.orDefault(defaultPort);

  for (auto& mapping: egressMappings) {
    if (mapping.cidr.matches(hostAndPort.host)) {
      // CIDR matches, now check port.
      // If the port is 0, we match anything.
      if (mapping.port == 0 || mapping.port == port) {
        return mapping.channel.get();
      }
    }
  }

  return kj::none;
}

kj::Promise<void> ContainerClient::ensureSidecarStarted() {
  if (containerSidecarStarted.exchange(true, std::memory_order_acquire)) {
    co_return;
  }

  KJ_ON_SCOPE_FAILURE(containerSidecarStarted.store(false, std::memory_order_release));

  // Get the Docker bridge gateway IP to listen on (only accessible from containers)
  auto ipamConfig = co_await getDockerBridgeIPAMConfig();
  // Create and start the sidecar container that shares the network namespace
  // with the main container and intercepts egress traffic.
  co_await createSidecarContainer(egressListenerPort, kj::mv(ipamConfig.subnet));
  co_await startSidecarContainer();
}

kj::Promise<void> ContainerClient::setEgressHttp(SetEgressHttpContext context) {
  auto params = context.getParams();
  auto hostPortStr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();

  auto parsed = parseHostPort(hostPortStr);
  uint16_t port = parsed.port.orDefault(80);
  auto cidr = kj::mv(parsed.cidr);

  if (egressListenerTask == kj::none) {
    // Get the Docker bridge gateway IP to listen on (only accessible from containers)
    auto ipamConfig = co_await getDockerBridgeIPAMConfig();

    // Start the egress listener first so it's ready when the sidecar starts.
    // Use port 0 to let the OS pick a free port dynamically.
    egressListenerPort = co_await startEgressListener(ipamConfig.gateway);
  }

  if (containerStarted.load(std::memory_order_acquire)) {
    // Only try to create and start a sidecar container
    // if the user container is running.
    co_await ensureSidecarStarted();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  egressMappings.add(EgressMapping{
    .cidr = kj::mv(cidr),
    .port = port,
    .channel = kj::mv(subrequestChannel),
  });

  co_return;
}

kj::Own<ContainerClient> ContainerClient::addRef() {
  return kj::addRef(*this);
}

}  // namespace workerd::server
