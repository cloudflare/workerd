// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/docker-api.capnp.h>

#if _WIN32
#include <ws2tcpip.h>
#undef DELETE
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

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

// Parsed address from parseHostPort()
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
        // No other colons, so this is IPv4 with port
        return {kj::str(beforeColon), port};
      }
    }
  }

  // No port found
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
    kj::String containerEgressInterceptorImage,
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
  // Stop the egress listener
  stopEgressListener();

  // Call the cleanup callback to remove this client from the ActorNamespace map
  cleanupCallback();

  // Destroy the sidecar container first (it depends on the main container's network)
  waitUntilTasks.add(dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", sidecarContainerName, "?force=true"))
                         .ignoreResult());

  // Destroy the main Docker container
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
class ContainerClient::InnerEgressService final: public kj::HttpService {
 public:
  InnerEgressService(IoChannelFactory::SubrequestChannel& channel): channel(channel) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    // Forward to the SubrequestChannel
    IoChannelFactory::SubrequestMetadata metadata;
    auto worker = channel.startRequest(kj::mv(metadata));

    // Forward the request to the worker - the response flows back through 'response'
    co_await worker->request(method, url, headers, requestBody, response);
  }

 private:
  IoChannelFactory::SubrequestChannel& channel;
};

// Outer HTTP service that handles CONNECT requests from the sidecar.
class ContainerClient::EgressHttpService final: public kj::HttpService {
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
    // The host header contains the destination address (e.g., "10.0.0.1:9999")
    // that the container was trying to connect to.
    auto destAddr = kj::str(host);

    // Accept the CONNECT tunnel
    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    // Check if there's a mapping for this destination
    auto mapping = containerClient.findEgressMapping(destAddr, /*defaultPort=*/80);

    KJ_IF_SOME(channel, mapping) {
      // Found a mapping - layer an HttpServer on top of the tunnel connection
      // to handle HTTP parsing/serialization automatically

      // Create the inner service that forwards to the worker binding
      auto innerService = kj::heap<InnerEgressService>(*channel);

      // Create an HttpServer for the tunnel connection
      auto innerServer =
          kj::heap<kj::HttpServer>(containerClient.timer, headerTable, *innerService);

      // Let the HttpServer handle the HTTP traffic inside the tunnel
      co_await innerServer->listenHttpCleanDrain(connection)
          .attach(kj::mv(innerServer), kj::mv(innerService));

      co_return;
    }

    // No mapping found - check if internet access is enabled
    if (!containerClient.internetEnabled) {
      // Internet access not enabled - close the connection
      connection.shutdownWrite();
      co_return;
    }

    // Forward to the general internet via raw TCP
    // Just do bidirectional byte pumping, no HTTP parsing needed
    auto addr = co_await containerClient.network.parseAddress(destAddr);
    auto destConn = co_await addr->connect();

    // Pump bytes bidirectionally: tunnel <-> destination
    auto connToDestination = connection.pumpTo(*destConn).then(
        [&destConn = *destConn](uint64_t) { destConn.shutdownWrite(); });

    auto destinationToConn =
        destConn->pumpTo(connection).then([&connection](uint64_t) { connection.shutdownWrite(); });

    // Wait for both directions to complete
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
  // Docker API: GET /networks/workerd-network
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
      "Failed to get or create workerd-network. "
      "Status: ",
      response.statusCode, ", Body: ", response.body);
}

kj::Promise<void> ContainerClient::createWorkerdNetwork() {
  // Docker API: POST /networks/create
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
    KJ_LOG(WARNING, "Failed to create workerd-network", response.statusCode, response.body);
  }
}

kj::Promise<uint16_t> ContainerClient::startEgressListener(kj::StringPtr listenAddress) {
  // Create the egress HTTP service
  auto service = kj::heap<EgressHttpService>(*this, headerTable);

  // Create the HTTP server
  auto httpServer = kj::heap<kj::HttpServer>(timer, headerTable, *service);
  auto& httpServerRef = *httpServer;

  // Attach service to httpServer so ownership is clear - httpServer owns service
  egressHttpServer = httpServer.attach(kj::mv(service));

  // Listen on the Docker bridge gateway IP with port 0 to let the OS pick a free port
  auto addr = co_await network.parseAddress(kj::str(listenAddress, ":0"));
  auto listener = addr->listen();

  // Get the actual port that was assigned
  uint16_t chosenPort = listener->getPort();

  // Run the server in the background - this promise never completes normally
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
  // Treat both "running" and "restarting" as running. The "restarting" state occurs when
  // Docker is automatically restarting a container (due to restart policy). From the user's
  // perspective, a restarting container is still "alive" and should be treated as running
  // so that start() correctly refuses to start a duplicate and destroy() can clean it up.
  bool running = status == "running" || status == "restarting";
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
  // Docker API: POST /containers/create
  // Equivalent to: docker run --cap-add=NET_ADMIN --network container:$(CONTAINER) ...
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(containerEgressInterceptorImage);

  // Pass the egress port to the sidecar via command line flag
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

  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  // In that case we destroy and re-create the container.
  if (response.statusCode == 409) {
    co_await destroySidecarContainer();
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/create?name=", sidecarContainerName), codec.encode(jsonRoot));
  }

  // statusCode 201 refers to "container created successfully"
  if (response.statusCode != 201) {
    JSG_REQUIRE(response.statusCode != 404, Error, "No such image available named ",
        containerEgressInterceptorImage,
        ". Please ensure the container egress interceptor image is built and available.");
    JSG_REQUIRE(response.statusCode != 409, Error, "Sidecar container already exists");
    JSG_FAIL_REQUIRE(
        Error, "Create sidecar container failed with [", response.statusCode, "] ", response.body);
  }
}

kj::Promise<void> ContainerClient::startSidecarContainer() {
  // Docker API: POST /containers/{id}/start
  auto endpoint = kj::str("/containers/", sidecarContainerName, "/start");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint), kj::str(""));
  // statusCode 304 refers to "container already started"
  JSG_REQUIRE(response.statusCode != 304, Error, "Sidecar container already started");
  // statusCode 204 refers to "no error"
  JSG_REQUIRE(
      response.statusCode == 204, Error, "Starting sidecar container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::destroySidecarContainer() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "?force=true");
  co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint))
      .ignoreResult();
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", sidecarContainerName, "/wait?condition=removed"));
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
      "Waiting for container sidecar removal failed with: ", response.statusCode, response.body);
  KJ_LOG(WARNING, "Container destroyed");
}

kj::Promise<void> ContainerClient::monitorSidecarContainer() {
  // Docker API: POST /containers/{id}/wait - wait for container to exit
  auto endpoint = kj::str("/containers/", sidecarContainerName, "/wait");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));

  if (response.statusCode == 200) {
    // Container exited - parse the exit code and log it
    auto jsonRoot = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(response.body);
    auto exitCode = jsonRoot.getStatusCode();
    KJ_LOG(WARNING, "Sidecar container exited unexpectedly", sidecarContainerName, exitCode);

    // Fetch the container logs to help diagnose the exit
    auto logsEndpoint =
        kj::str("/containers/", sidecarContainerName, "/logs?stdout=true&stderr=true&tail=50");
    auto logsResponse = co_await dockerApiRequest(
        network, kj::str(dockerPath), kj::HttpMethod::GET, kj::mv(logsEndpoint));
    if (logsResponse.statusCode == 200) {
      KJ_LOG(WARNING, "Sidecar container logs:", logsResponse.body);
    }
  } else if (response.statusCode == 404) {
    // Container was removed before we could monitor it - this is normal during shutdown
  } else {
    KJ_LOG(ERROR, "Failed to monitor sidecar container", response.statusCode, response.body);
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

  // Track whether internet access is enabled for this container
  internetEnabled = params.getEnableInternet();

  // Create and start the main user container
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
  // Destroy sidecar first since it depends on the main container's network
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

  struct sockaddr_storage ss;
  memset(&ss, 0, sizeof(ss));

  auto* sin = reinterpret_cast<struct sockaddr_in*>(&ss);
  auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(&ss);

  // This is kind of awful. We could theoretically have a CidrRange
  // parse this, but we don't have a way to compare two CidrRanges.
  // Ideally, KJ would have a library to parse IPs, and we are able to have a cidr.includes(ip) method.
  if (inet_pton(AF_INET, hostAndPort.host.cStr(), &sin->sin_addr) == 1) {
    ss.ss_family = AF_INET;
    sin->sin_port = htons(port);
  } else if (inet_pton(AF_INET6, hostAndPort.host.cStr(), &sin6->sin6_addr) == 1) {
    ss.ss_family = AF_INET6;
    sin6->sin6_port = htons(port);
  } else {
    JSG_KJ_EXCEPTION(FAILED, Error, "host is an invalid address");
  }

  // Find a matching mapping
  for (auto& mapping: egressMappings) {
    if (mapping.cidr.matches(reinterpret_cast<struct sockaddr*>(&ss))) {
      // CIDR matches, now check port.
      // If the port is 0, we match anything.
      if (mapping.port == 0 || mapping.port == port) {
        return mapping.channel.get();
      }
    }
  }

  return kj::none;
}

kj::Promise<void> ContainerClient::setEgressHttp(SetEgressHttpContext context) {
  auto params = context.getParams();
  auto hostPortStr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();
  JSG_REQUIRE(containerEgressInterceptorImage != "", Error, "should be set for setEgressHttp");

  auto parsed = parseHostPort(hostPortStr);
  uint16_t port = parsed.port.orDefault(80);
  auto cidr = kj::mv(parsed.cidr);

  // Wait for any previous setEgressHttp call to complete
  KJ_IF_SOME(lock, egressSetupLock) {
    co_await lock.addBranch();
  }

  // If no egressListenerTask, start one now.
  // The biggest disadvantage of doing it here, is that if the workerd process restarts,
  // and the container is still running, it might have no connectivity.
  if (egressListenerTask == kj::none) {
    // Create a promise/fulfiller pair to signal when setup is complete
    // TODO: Actually, every RPC in this class would benefit from this.
    auto paf = kj::newPromiseAndFulfiller<void>();
    egressSetupLock = paf.promise.fork();
    KJ_DEFER(paf.fulfiller->fulfill());

    // Get the Docker bridge gateway IP to listen on (only accessible from containers)
    auto ipamConfig = co_await getDockerBridgeIPAMConfig();

    // Start the egress listener first so it's ready when the sidecar starts.
    // Use port 0 to let the OS pick a free port dynamically.
    egressListenerPort = co_await startEgressListener(ipamConfig.gateway);

    // Create and start the sidecar container that shares the network namespace
    // with the main container and intercepts egress traffic.
    // Pass the dynamically chosen port so the sidecar knows where to connect.
    co_await createSidecarContainer(egressListenerPort, kj::mv(ipamConfig.subnet));
    co_await startSidecarContainer();

    // Monitor the sidecar container for unexpected exits
    waitUntilTasks.add(monitorSidecarContainer());
  }

  // Redeem the channel token to get a SubrequestChannel
  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  // Store the mapping
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
