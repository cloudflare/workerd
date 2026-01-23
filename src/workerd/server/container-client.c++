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

// HTTP service that handles HTTP CONNECT requests from the container sidecar (dockerproxyanything).
// When the sidecar intercepts container egress traffic, it sends HTTP CONNECT to this service.
// After accepting the CONNECT, the tunnel carries the actual HTTP request from the container,
// which we parse and forward to the appropriate SubrequestChannel based on egressMappings.
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
    auto maybeChannel = containerClient.egressMappings.find(destAddr);

    if (maybeChannel == kj::none) {
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
      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);

      promises.add(connection.pumpTo(*destConn).then([&destConn = *destConn](uint64_t) {
        destConn.shutdownWrite();
      }));

      promises.add(destConn->pumpTo(connection).then([&connection](uint64_t) {
        connection.shutdownWrite();
      }));

      // Wait for both directions to complete, keeping destConn alive
      co_await kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(destConn));
      co_return;
    }

    // Found a mapping - need to parse HTTP and forward to the SubrequestChannel
    auto& channel = KJ_ASSERT_NONNULL(maybeChannel);

    // Parse HTTP requests from the tunnel
    auto httpInput = kj::newHttpInputStream(connection, headerTable);

    // Loop to handle multiple requests on the same connection (HTTP/1.1 keep-alive)
    while (true) {
      // Check if there's more data
      bool hasMore = co_await httpInput->awaitNextMessage();
      if (!hasMore) {
        // Client closed the connection
        co_return;
      }

      auto req = co_await httpInput->readRequest();

      // Forward to the SubrequestChannel
      IoChannelFactory::SubrequestMetadata metadata;
      auto worker = channel->startRequest(kj::mv(metadata));

      // Create a response handler that writes back to the tunnel
      TunnelHttpResponse tunnelResponse(connection);

      // Forward the request to the worker
      co_await worker->request(req.method, req.url, req.headers, *req.body, tunnelResponse);

      // Finalize the response (writes chunked terminator if needed)
      co_await tunnelResponse.end();

      // After the response is complete, shut down the write side to signal EOF
      connection.shutdownWrite();
      co_return;
    }
  }

 private:
  ContainerClient& containerClient;
  kj::HttpHeaderTable& headerTable;

  // Response implementation that writes HTTP responses back through the tunnel.
  // This class serializes the HTTP response and writes it to the tunnel stream.
  class TunnelHttpResponse final: public kj::HttpService::Response {
   public:
    TunnelHttpResponse(kj::AsyncIoStream& tunnel)
        : tunnel(tunnel), isChunked(false) {}

    kj::Own<kj::AsyncOutputStream> send(uint statusCode,
        kj::StringPtr statusText,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {

      isChunked = (expectedBodySize == kj::none);
      auto headersStr = headers.serializeResponse(statusCode, statusText);

      return kj::heap<TunnelOutputStream>(tunnel, kj::mv(headersStr), isChunked);
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      KJ_FAIL_REQUIRE("WebSocket upgrade not supported through egress tunnel");
    }

    // Called after worker->request() completes to finalize the response
    kj::Promise<void> end() {
      if (isChunked) {
        // Write final empty chunk to terminate chunked encoding
        co_await tunnel.write("0\r\n\r\n"_kjb);
      }
    }

   private:
    kj::AsyncIoStream& tunnel;
    bool isChunked;
  };

  // Output stream that writes to the tunnel.
  // Headers are written on the first write or when the stream ends.
  // If chunked mode is enabled, wraps body data in chunked transfer encoding.
  class TunnelOutputStream final: public kj::AsyncOutputStream {
   public:
    TunnelOutputStream(kj::AsyncIoStream& tunnel, kj::String serializedHeaders, bool chunked)
        : tunnel(tunnel),
          serializedHeaders(kj::mv(serializedHeaders)),
          headersWritten(false),
          chunked(chunked) {}

    kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
      co_await ensureHeadersWritten();
      if (chunked) {
        // Write chunk: size in hex, CRLF, data, CRLF
        auto chunkHeader = kj::str(kj::hex(buffer.size()), "\r\n");
        co_await tunnel.write(chunkHeader.asBytes());
        co_await tunnel.write(buffer);
        co_await tunnel.write("\r\n"_kjb);
      } else {
        co_await tunnel.write(buffer);
      }
    }

    kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
      co_await ensureHeadersWritten();
      if (chunked) {
        // Calculate total size for chunk header
        size_t totalSize = 0;
        for (auto& piece : pieces) {
          totalSize += piece.size();
        }
        auto chunkHeader = kj::str(kj::hex(totalSize), "\r\n");
        co_await tunnel.write(chunkHeader.asBytes());
        co_await tunnel.write(pieces);
        co_await tunnel.write("\r\n"_kjb);
      } else {
        co_await tunnel.write(pieces);
      }
    }

    kj::Promise<void> whenWriteDisconnected() override {
      return tunnel.whenWriteDisconnected();
    }

   private:
    // Ensure headers are written
    kj::Promise<void> ensureHeadersWritten() {
      if (!headersWritten) {
        headersWritten = true;
        co_await tunnel.write(serializedHeaders.asBytes());
      }
    }

    kj::AsyncIoStream& tunnel;
    kj::String serializedHeaders;
    bool headersWritten;
    bool chunked;
  };
};

kj::Promise<kj::String> ContainerClient::getDockerBridgeGateway() {
  // Docker API: GET /networks/bridge
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/networks/bridge"));

  if (response.statusCode == 200) {
    auto jsonRoot = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(response.body);
    auto ipamConfig = jsonRoot.getIpam().getConfig();
    if (ipamConfig.size() > 0) {
      auto gateway = ipamConfig[0].getGateway();
      if (gateway.size() > 0) {
        co_return kj::str(gateway);
      }
    }
  }

  // Fallback to default Docker bridge gateway
  KJ_LOG(WARNING, "Could not determine Docker bridge gateway, using default 172.17.0.1");
  co_return kj::str("172.17.0.1");
}

kj::Promise<void> ContainerClient::startEgressListener(kj::StringPtr listenAddress, uint16_t port) {
  // Create header table for HTTP parsing
  auto headerTable = kj::heap<kj::HttpHeaderTable>();
  auto& headerTableRef = *headerTable;
  egressHeaderTable = kj::mv(headerTable);

  // Create the egress HTTP service
  auto service = kj::heap<EgressHttpService>(*this, headerTableRef);

  // Create the HTTP server
  auto httpServer = kj::heap<kj::HttpServer>(timer, headerTableRef, *service);
  auto& httpServerRef = *httpServer;
  egressHttpServer = kj::mv(httpServer);

  // Listen on the Docker bridge gateway IP so only containers can connect
  auto addr = co_await network.parseAddress(kj::str(listenAddress, ":", port));
  auto listener = addr->listen();

  // Run the server - this promise never completes normally
  co_await httpServerRef.listenHttp(*listener).attach(kj::mv(listener), kj::mv(service));
}

void ContainerClient::stopEgressListener() {
  egressListenerTask = kj::none;
  egressHttpServer = kj::none;
  egressHeaderTable = kj::none;
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
  extraHosts.set(0, "host.docker.internal:host-gateway");

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

// Creates the sidecar container for egress proxy.
// The sidecar shares the network namespace with the main container and runs
// dockerproxyanything to intercept and proxy egress traffic.
kj::Promise<void> ContainerClient::createSidecarContainer() {
  // Docker API: POST /containers/create
  // Equivalent to: docker run --cap-add=NET_ADMIN --network container:$(CONTAINER) ...
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(SIDECAR_IMAGE_NAME);

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
    JSG_REQUIRE(response.statusCode != 404, Error,
        "No such image available named ", SIDECAR_IMAGE_NAME,
        ". Please ensure the dockerproxyanything image is built and available.");
    JSG_REQUIRE(response.statusCode != 409, Error, "Sidecar container already exists");
    JSG_FAIL_REQUIRE(Error,
        "Create sidecar container failed with [", response.statusCode, "] ", response.body);
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
  JSG_REQUIRE(response.statusCode == 204, Error,
      "Starting sidecar container failed with: ", response.body);
}

kj::Promise<void> ContainerClient::destroySidecarContainer() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "?force=true");
  co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint)).ignoreResult();
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
    auto logsEndpoint = kj::str("/containers/", sidecarContainerName, "/logs?stdout=true&stderr=true&tail=50");
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

kj::Promise<void> ContainerClient::setEgressHttp(SetEgressHttpContext context) {
  auto params = context.getParams();
  auto addr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();

  // Wait for any previous setEgressHttp call to complete (serializes sidecar setup)
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
    auto bridgeGateway = co_await getDockerBridgeGateway();

    // Start the egress listener first so it's ready when the sidecar starts.
    // The sidecar will connect to this port to proxy container egress traffic.
    // Default port matches the sidecar's expected port (49121).
    // TODO: Multiple containers will break this assumption.
    constexpr uint16_t EGRESS_LISTENER_PORT = 49121;
    egressListenerTask = startEgressListener(bridgeGateway, EGRESS_LISTENER_PORT).eagerlyEvaluate([](kj::Exception&& e) {
      LOG_EXCEPTION("Error listening to port", e);
    });

    // Create and start the sidecar container that shares the network namespace
    // with the main container and intercepts egress traffic.
    // Keep in mind there will be blips of connectivity on multiple calls.
    co_await createSidecarContainer();
    co_await startSidecarContainer();

    // Monitor the sidecar container for unexpected exits
    waitUntilTasks.add(monitorSidecarContainer());
  }

  // Redeem the channel token to get a SubrequestChannel
  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  // Store the mapping
  egressMappings.upsert(kj::mv(addr), kj::mv(subrequestChannel),
      [](auto& existing, auto&& newValue) { existing = kj::mv(newValue); });

  co_return;
}

kj::Promise<void> ContainerClient::setEgressTcp(SetEgressTcpContext context) {
   KJ_UNIMPLEMENTED("setEgressTcp not implemented - use setEgressHttp for now");
}

kj::Own<ContainerClient> ContainerClient::addRef() {
  return kj::addRef(*this);
}

}  // namespace workerd::server
