// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
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

// ASCII-only case-insensitive equality for DNS hostnames / SNI.
bool asciiCaseInsensitiveEquals(kj::ArrayPtr<const char> a, kj::ArrayPtr<const char> b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = (a[i] >= 'A' && a[i] <= 'Z') ? (a[i] | 0x20) : a[i];
    char cb = (b[i] >= 'A' && b[i] <= 'Z') ? (b[i] | 0x20) : b[i];
    if (ca != cb) return false;
  }
  return true;
}

// Matches a hostname against a glob pattern (only leading '*' supported).
// "*.example.com" also matches "a.b.example.com" because RFC 6125 single-label
// restriction doesn't apply to interception config (not a TLS certificate).
// Case-insensitive per RFC 6066 §3.
bool matchSniGlob(kj::StringPtr glob, kj::StringPtr hostname) {
  if (glob == "*") return true;
  if (glob.startsWith("*.")) {
    // "*.example.com" should match "foo.example.com" but not "example.com"
    auto suffix = glob.slice(1);  // ".example.com"
    if (hostname.size() <= suffix.size()) return false;
    return asciiCaseInsensitiveEquals(hostname.slice(hostname.size() - suffix.size()), suffix);
  }
  return asciiCaseInsensitiveEquals(glob, hostname);
}

// Extract stdout from Docker's multiplexed exec stream.
// Format: repeated [stream_type(1), padding(3), size_big_endian(4)] + payload(size).
// stream_type 1 = stdout.
kj::Maybe<kj::String> demuxDockerExecStream(kj::ArrayPtr<const kj::byte> data) {
  kj::Vector<kj::byte> result;
  size_t offset = 0;
  while (offset + 8 <= data.size()) {
    uint8_t streamType = data[offset];
    uint32_t frameSize = (static_cast<uint32_t>(data[offset + 4]) << 24) |
        (static_cast<uint32_t>(data[offset + 5]) << 16) |
        (static_cast<uint32_t>(data[offset + 6]) << 8) | static_cast<uint32_t>(data[offset + 7]);
    offset += 8;
    if (offset + frameSize > data.size()) {
      KJ_LOG(WARNING, "Docker exec stream truncated: frame at offset", offset, "claims", frameSize,
          "bytes but only", data.size() - offset, "remain");
      break;
    }
    if (streamType == 1) {
      result.addAll(data.slice(offset, offset + frameSize));
    }

    offset += frameSize;
  }

  if (result.empty()) return kj::none;
  auto bytes = result.releaseAsArray();
  return kj::str(bytes.asChars());
}

void writeTarField(kj::ArrayPtr<kj::byte> field, kj::StringPtr value) {
  auto len = kj::min(value.size(), field.size());
  auto src = value.asBytes().first(len);
  field.first(len).copyFrom(src);
}

// Creates a minimal POSIX (ustar) tar archive containing a single file.
// Used to upload CA certs via PUT /containers/{id}/archive.
// Only handles small files with short known filenames. Replace with a
// proper tar library if requirements grow.
kj::Array<kj::byte> createTarWithFile(
    kj::StringPtr filename, kj::ArrayPtr<const kj::byte> content) {
  KJ_REQUIRE(filename.size() < 100, "tar filename must be < 100 bytes");
  KJ_REQUIRE(
      content.size() < 8ull * 1024 * 1024 * 1024, "tar content too large for 11-digit octal");
  // Tar: 512-byte header + content padded to 512 + two 512-byte EOF blocks.
  size_t paddedSize = (content.size() + 511) & ~511;
  size_t totalSize = 512 + paddedSize + 1024;
  auto tar = kj::heapArray<kj::byte>(totalSize);
  tar.asPtr().fill(0);

  auto header = tar.first(512);

  // Name (offset 0, 100 bytes)
  writeTarField(header.slice(0, 100), filename);

  // Mode (offset 100, 8 bytes)
  writeTarField(header.slice(100, 108), "0000644"_kj);

  // UID/GID (offset 108/116, 8 bytes each)
  writeTarField(header.slice(108, 116), "0000000"_kj);
  writeTarField(header.slice(116, 124), "0000000"_kj);

  // Size (offset 124, 12 bytes), octal
  {
    char sizeBuf[12];
    snprintf(sizeBuf, sizeof(sizeBuf), "%011lo", static_cast<unsigned long>(content.size()));
    writeTarField(header.slice(124, 136), kj::StringPtr(sizeBuf));
  }

  // Mtime (offset 136, 12 bytes)
  writeTarField(header.slice(136, 148), "00000000000"_kj);

  // Typeflag (offset 156), '0' = regular file
  header[156] = '0';

  // Magic (offset 257, 6 bytes) + version (offset 263, 2 bytes)
  writeTarField(header.slice(257, 263), "ustar"_kj);
  writeTarField(header.slice(263, 265), "00"_kj);

  // Checksum (offset 148, 8 bytes): sum of all header bytes with checksum field as spaces.
  header.slice(148, 156).fill(' ');
  uint32_t checksum = 0;
  for (auto b: header) checksum += b;
  {
    char csumBuf[8];
    snprintf(csumBuf, sizeof(csumBuf), "%06o ", checksum);
    writeTarField(header.slice(148, 155), kj::StringPtr(csumBuf));
  }

  tar.slice(512, 512 + content.size()).copyFrom(content);

  return tar;
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
    kj::Promise<void> pendingCleanup,
    kj::Function<void(kj::Promise<void>)> cleanupCallback,
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
      pendingCleanup(kj::mv(pendingCleanup).fork()),
      cleanupCallback(kj::mv(cleanupCallback)),
      channelTokenHandler(channelTokenHandler) {}

ContainerClient::~ContainerClient() noexcept(false) {
  stopEgressListener();

  // Sidecar shares main container's network namespace, so must be destroyed first.
  auto sidecarCleanup = dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", sidecarContainerName, "?force=true"))
                            .ignoreResult()
                            .catch_([](kj::Exception&&) {});

  auto mainCleanup = dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", containerName, "?force=true"))
                         .ignoreResult()
                         .catch_([](kj::Exception&&) {});

  // Pass the joined cleanup promise to the callback. The callback wraps it with the
  // canceler (so a future client creation can cancel it), stores it so the next
  // ContainerClient can await it, and adds a branch to waitUntilTasks to keep the
  // underlying I/O alive.
  cleanupCallback(kj::joinPromises(kj::arr(kj::mv(sidecarCleanup), kj::mv(mainCleanup))));
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
  using ChannelLookup = kj::Function<kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>>()>;

  InnerEgressService(ChannelLookup lookupChannel, kj::StringPtr destAddr, bool isTls = false)
      : lookupChannel(kj::mv(lookupChannel)),
        destAddr(kj::str(destAddr)),
        isTls(isTls) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr requestUri,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    // Look up the channel on each request so we always use the latest mapping,
    // even if it was replaced via interceptOutboundHttp while the tunnel is open.
    auto channel =
        KJ_REQUIRE_NONNULL(lookupChannel(), "egress mapping disappeared during active tunnel");

    IoChannelFactory::SubrequestMetadata metadata;
    auto worker = channel->startRequest(kj::mv(metadata));
    auto urlForWorker = kj::str(requestUri);
    // Probably only a path, try to get it from Host:
    if (requestUri.startsWith("/")) {
      auto scheme = isTls ? "https://"_kj : "http://"_kj;
      auto baseUrl = kj::str(scheme, destAddr);
      // Use Host: when possible
      KJ_IF_SOME(host, headers.get(kj::HttpHeaderId::HOST)) {
        baseUrl = kj::str(scheme, host);
      }

      // Parse url, if invalid, try to use the original requestUri (http://<ip>/<path>
      KJ_IF_SOME(parsedUrl, jsg::Url::tryParse(requestUri, baseUrl.asPtr())) {
        urlForWorker = kj::str(parsedUrl.getHref());
      } else {
        urlForWorker = kj::str(baseUrl, requestUri);
      }
    }

    co_await worker->request(method, urlForWorker, headers, requestBody, response);
  }

 private:
  ChannelLookup lookupChannel;
  kj::String destAddr;
  bool isTls;
};

kj::Promise<void> pumpBidirectional(kj::AsyncIoStream& a, kj::AsyncIoStream& b) {
  auto aToB = a.pumpTo(b).then([&b](uint64_t) { b.shutdownWrite(); });
  auto bToA = b.pumpTo(a).then([&a](uint64_t) { a.shutdownWrite(); });
  co_await kj::joinPromisesFailFast(kj::arr(kj::mv(aToB), kj::mv(bToA)));
}

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

  // CONNECT protocol between the sidecar and workerd:
  //
  // The sidecar sends an HTTP CONNECT for every outbound connection from the container.
  // If it detected a TLS ClientHello, it includes an "X-Tls-Sni" header with the SNI.
  //
  // Response status codes signal the sidecar what to do:
  //   200: Workerd will handle this connection (intercepted HTTP/HTTPS or plaintext
  //        passthrough). For TLS with a mapping the sidecar MITMs and sends decrypted
  //        HTTP through the tunnel; for plaintext it sends raw bytes.
  //   202: TLS with no matching HTTPS mapping. Sidecar passes raw TLS bytes through
  //        the tunnel without decryption (workerd forwards to destination or drops
  //        if internet is disabled).
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    auto destAddr = kj::str(host);

    auto tlsSni = headers.get(containerClient.egressHeaderTable.tlsSniId);

    KJ_IF_SOME(sni, tlsSni) {
      auto httpsMapping = containerClient.findEgressHttpsMapping(sni);

      if (httpsMapping != kj::none) {
        // Mapping matched; tell sidecar to MITM and send us decrypted HTTP.
        kj::HttpHeaders responseHeaders(headerTable);
        response.accept(200, "OK", responseHeaders);

        // Looks up the mapping on each request so channel replacements are
        // picked up on existing tunnels.
        auto innerService = kj::heap<InnerEgressService>(
            [&client = containerClient, sniStr = kj::str(sni)]()
                -> kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> {
          return client.findEgressHttpsMapping(sniStr);
        },
            destAddr, /*isTls=*/true);
        auto innerServer =
            kj::heap<kj::HttpServer>(containerClient.timer, headerTable, *innerService);
        co_await innerServer->listenHttpCleanDrain(connection);
        co_return;
      }

      // No HTTPS mapping, pass raw TLS through (202).
      kj::HttpHeaders responseHeaders(headerTable);
      response.accept(202, "Accepted", responseHeaders);

      if (!containerClient.internetEnabled) {
        connection.shutdownWrite();
        co_return;
      }

      auto addr = co_await containerClient.network.parseAddress(destAddr);
      auto destConn = co_await addr->connect();
      co_await pumpBidirectional(connection, *destConn);
      co_return;
    }

    // No TLS, plain HTTP proxying.
    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(200, "OK", responseHeaders);

    auto mapping = containerClient.findEgressMapping(destAddr, /*defaultPort=*/80);

    if (mapping != kj::none) {
      // Layer an HttpServer on top of the tunnel to handle HTTP parsing/serialization.
      // InnerEgressService looks up the mapping on each request so channel replacements
      // via interceptOutboundHttp are picked up on existing tunnels.
      auto innerService = kj::heap<InnerEgressService>(
          [&client = containerClient, addr = kj::str(destAddr)]()
              -> kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> {
        return client.findEgressMapping(addr, /*defaultPort=*/80);
      },
          destAddr);
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
    co_await pumpBidirectional(connection, *destConn);
    co_return;
  }

 private:
  ContainerClient& containerClient;
  kj::HttpHeaderTable& headerTable;
};

kj::Promise<ContainerClient::IPAMConfigResult> ContainerClient::getDockerBridgeIPAMConfig() {
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/networks/bridge"));
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
      "Failed to get bridge. "
      "Status: ",
      response.statusCode, ", Body: ", response.body);
}

kj::Promise<bool> ContainerClient::isDaemonIpv6Enabled() {
  // Inspect the default bridge network. When the Docker daemon has "ipv6": true in
  // daemon.json, the default bridge gets an IPv6 IPAM subnet entry (e.g. "fd00::/80").
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/networks/bridge"));

  if (response.statusCode != 200) {
    co_return false;
  }

  auto jsonRoot = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(response.body);
  for (auto config: jsonRoot.getIpam().getConfig()) {
    // IPv6 subnets contain ':' (e.g. "fd00::/80", "2001:db8::/64")
    if (kj::StringPtr(config.getSubnet()).findFirst(':') != kj::none) {
      co_return true;
    }
  }

  co_return false;
}

// Returns the gateway IP on Linux for direct container access.
// Returns kj::none on macOS where Docker Desktop routes host-gateway to host loopback.
static kj::Maybe<kj::String> gatewayForPlatform(kj::String gateway) {
#ifdef __APPLE__
  return kj::none;
#else
  return kj::mv(gateway);
#endif
}

kj::Promise<uint16_t> ContainerClient::startEgressListener(
    kj::String listenAddress, uint16_t port) {
  auto service = kj::heap<EgressHttpService>(*this, headerTable);
  auto httpServer = kj::heap<kj::HttpServer>(timer, headerTable, *service);
  auto& httpServerRef = *httpServer;

  egressHttpServer = httpServer.attach(kj::mv(service));

  auto addr = co_await network.parseAddress(kj::str(listenAddress, ":", port));
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
  egressListenerStarted.store(false, std::memory_order_release);
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

kj::Promise<kj::Maybe<kj::String>> ContainerClient::readFileFromContainer(
    kj::StringPtr container, kj::StringPtr path) {
  // Uses Docker exec ("cat <path>") instead of the archive/tar API to avoid
  // tar format parsing issues across Docker versions.
  capnp::JsonCodec createCodec;
  createCodec.handleByAnnotation<docker_api::Docker::ExecCreateRequest>();
  capnp::MallocMessageBuilder createMsg;
  auto createReq = createMsg.initRoot<docker_api::Docker::ExecCreateRequest>();
  auto cmdList = createReq.initCmd(2);
  cmdList.set(0, "cat");
  cmdList.set(1, path);
  createReq.setAttachStdout(true);

  auto createResponse =
      co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
          kj::str("/containers/", container, "/exec"), createCodec.encode(createReq));
  if (createResponse.statusCode != 201) {
    co_return kj::Maybe<kj::String>(kj::none);
  }

  capnp::JsonCodec respCodec;
  respCodec.handleByAnnotation<docker_api::Docker::ExecCreateResponse>();
  capnp::MallocMessageBuilder respMsg;
  auto createResp = respMsg.initRoot<docker_api::Docker::ExecCreateResponse>();
  respCodec.decode(createResponse.body, createResp);
  if (!createResp.hasId()) {
    co_return kj::Maybe<kj::String>(kj::none);
  }
  auto execId = kj::str(createResp.getId());

  // Tty=false gives us the multiplexed stream format (no \r\n mangling).
  capnp::JsonCodec startCodec;
  startCodec.handleByAnnotation<docker_api::Docker::ExecStartRequest>();
  capnp::MallocMessageBuilder startMsg;
  auto startReq = startMsg.initRoot<docker_api::Docker::ExecStartRequest>();
  // Detach and Tty default to false, which is what we want.

  auto startResponse = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/exec/", execId, "/start"), startCodec.encode(startReq));
  if (startResponse.statusCode != 200) {
    co_return kj::Maybe<kj::String>(kj::none);
  }

  co_return demuxDockerExecStream(startResponse.body.asBytes());
}

kj::Promise<void> ContainerClient::writeFileToContainer(kj::StringPtr container,
    kj::StringPtr dir,
    kj::StringPtr filename,
    kj::ArrayPtr<const kj::byte> content) {
  kj::HttpHeaderTable table;
  auto address = co_await network.parseAddress(kj::str(dockerPath));
  auto connection = co_await address->connect();
  auto httpClient = kj::newHttpClient(table, *connection).attach(kj::mv(connection));

  auto tar = createTarWithFile(filename, content);

  kj::HttpHeaders headers(table);
  headers.setPtr(kj::HttpHeaderId::HOST, "localhost");
  headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "application/x-tar");
  headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(tar.size()));

  auto endpoint = kj::str("/containers/", container, "/archive?path=", kj::encodeUriComponent(dir));
  auto req = httpClient->request(kj::HttpMethod::PUT, endpoint, headers, tar.size());
  {
    auto body = kj::mv(req.body);
    co_await body->write(tar.asBytes());
  }
  auto response = co_await req.response;
  auto result = co_await response.body->readAllText();
  JSG_REQUIRE(response.statusCode == 200, Error, "Failed to write file", dir, filename,
      "to container [", response.statusCode, "] ", result);
}

static constexpr kj::StringPtr sidecarCaCertPath = "/ca/ca.crt"_kj;

// Distro-independent path for the Cloudflare CA cert inside the user container.
// Written relative to /etc; Docker's tar extraction creates intermediate dirs.
static constexpr kj::StringPtr cloudflareCaDir = "/etc"_kj;
static constexpr kj::StringPtr cloudflareCaFilename =
    "cloudflare/certs/cloudflare-containers-ca.crt"_kj;

kj::Promise<void> ContainerClient::injectCACert() {
  if (caCertInjected.exchange(true, std::memory_order_acquire)) {
    co_return;
  }

  bool succeeded = false;
  KJ_DEFER(if (!succeeded) caCertInjected.store(false, std::memory_order_release));

  // Retry because the sidecar may still be generating the cert.
  static constexpr int caCertMaxRetries = 5;
  static constexpr auto caCertRetryDelay = 1 * kj::SECONDS;
  kj::Maybe<kj::String> maybeCaCert;
  for (int attempt = 0; attempt < caCertMaxRetries; ++attempt) {
    maybeCaCert = co_await readFileFromContainer(sidecarContainerName, sidecarCaCertPath);
    if (maybeCaCert != kj::none) break;
    if (attempt + 1 < caCertMaxRetries) {
      co_await timer.afterDelay(caCertRetryDelay);
    }
  }

  auto caCert = KJ_REQUIRE_NONNULL(kj::mv(maybeCaCert), "Sidecar CA cert not found at ",
      sidecarCaCertPath, " after ", caCertMaxRetries, " attempts.",
      " Ensure the sidecar is running with --tls-intercept.");

  co_await writeFileToContainer(
      containerName, cloudflareCaDir, cloudflareCaFilename, caCert.asBytes());

  succeeded = true;
  co_return;
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

kj::Promise<kj::Maybe<uint16_t>> ContainerClient::inspectSidecarEgressPort() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "/json");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::mv(endpoint));

  if (response.statusCode == 404) {
    co_return kj::Maybe<uint16_t>(kj::none);
  }

  JSG_REQUIRE(response.statusCode == 200, Error, "Sidecar container inspect failed");

  auto jsonRoot = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(response.body);

  // Check if sidecar is actually running
  if (jsonRoot.hasState()) {
    auto state = jsonRoot.getState();
    if (state.hasStatus()) {
      auto status = state.getStatus();
      if (status != "running" && status != "restarting") {
        co_return kj::Maybe<uint16_t>(kj::none);
      }
    }
  }

  // Parse args to find --http-egress-port value
  if (jsonRoot.hasArgs()) {
    auto args = jsonRoot.getArgs();
    for (auto i = 0u; i < args.size(); i++) {
      if (args[i] == "--http-egress-port" && i + 1 < args.size()) {
        co_return kj::str(args[i + 1]).parseAs<uint16_t>();
      }
    }
  }

  co_return kj::Maybe<uint16_t>(kj::none);
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
  // Add host.docker.internal mapping so containers can reach the host.
  // The sidecar uses host-gateway to reach the egress listener on the host.
  auto extraHosts = hostConfig.initExtraHosts(1);
  extraHosts.set(0, "host.docker.internal:host-gateway"_kj);

  hostConfig.setNetworkMode("bridge");

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

  auto ipv6Enabled = co_await isDaemonIpv6Enabled();

  uint32_t cmdSize = 5;  // --http-egress-port <port> --docker-gateway-cidr <cidr> --tls-intercept
  if (!ipv6Enabled) cmdSize += 1;  // --disable-ipv6

  auto cmd = jsonRoot.initCmd(cmdSize);
  uint32_t idx = 0;
  cmd.set(idx++, "--http-egress-port");
  cmd.set(idx++, kj::str(egressPort));
  cmd.set(idx++, "--docker-gateway-cidr");
  cmd.set(idx++, networkCidr);
  if (!ipv6Enabled) {
    cmd.set(idx++, "--disable-ipv6");
  }

  // Enabling tls-intercept is OK because it adds minimal overhead,
  // we won't attempt to intercept in workerd unless the SNI glob matches.
  cmd.set(idx++, "--tls-intercept");

  auto hostConfig = jsonRoot.initHostConfig();
  // Share network namespace with the main container
  hostConfig.setNetworkMode(kj::str("container:", containerName));

  // Sidecar needs NET_ADMIN capability for iptables/TPROXY
  auto capAdd = hostConfig.initCapAdd(1);
  capAdd.set(0, "NET_ADMIN");

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
  // statusCode 304 refers to "container already started"
  // statusCode 204 refers to "request succeeded"
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 304, Error,
      "Starting network sidecar container failed with: ", response.statusCode, response.body);
}

kj::Promise<void> ContainerClient::destroySidecarContainer() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "?force=true");
  auto responseDestroy = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint));
  // statusCode 204 refers to "no error"
  // statusCode 404 refers to "no such container"
  // statusCode 409 refers to "removal already in progress" (race between concurrent destroys)
  // All of which are fine for us since we're tearing down the sidecar
  JSG_REQUIRE(responseDestroy.statusCode == 204 || responseDestroy.statusCode == 404 ||
          responseDestroy.statusCode == 409,
      Error, "Destroying network sidecar container failed with: ", responseDestroy.statusCode,
      responseDestroy.body);
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", sidecarContainerName, "/wait?condition=removed"));
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
      "Destroying docker network sidecar container failed: ", response.statusCode, response.body);
}

ContainerClient::RpcTurn ContainerClient::getRpcTurn() {
  auto paf = kj::newPromiseAndFulfiller<void>();
  auto prev = mutationQueue.addBranch();
  mutationQueue = paf.promise.fork();
  return {kj::mv(prev), kj::mv(paf.fulfiller)};
}

kj::Promise<void> ContainerClient::status(StatusContext context) {
  // Wait for any pending cleanup from a previous ContainerClient (Docker DELETE).
  // If the cleanup was already cancelled via containerCleanupCanceler the .catch_()
  // in the destructor resolves it immediately, so this is a no-op in that case.
  co_await pendingCleanup.addBranch();

  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  const auto [isRunning, _ports] = co_await inspectContainer();
  containerStarted.store(isRunning, std::memory_order_release);

  if (isRunning && containerEgressInterceptorImage != kj::none) {
    // If the sidecar container is already running (e.g. workerd restarted while
    // containers stayed up), recover the egress port it was started with and
    // start the host-side egress listener on that same port so the sidecar can
    // reconnect.
    KJ_IF_SOME(port, co_await inspectSidecarEgressPort()) {
      containerSidecarStarted.store(true, std::memory_order_release);
      co_await ensureEgressListenerStarted(port);
    }
  }

  context.getResults().setRunning(isRunning);
}

kj::Promise<void> ContainerClient::start(StartContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

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

  if (!egressMappings.empty() || !egressHttpsMappings.empty()) {
    // The user container will be blocked on network connectivity until this finishes.
    containerSidecarStarted = false;
    co_await ensureSidecarStarted();
  }

  if (!egressHttpsMappings.empty()) {
    caCertInjected = false;
    co_await injectCACert();
  }

  containerStarted.store(true, std::memory_order_release);
}

kj::Promise<void> ContainerClient::monitor(MonitorContext context) {
  // Wait for any in-progress mutating RPCs (e.g. start()) to complete
  // before issuing the Docker wait request.
  co_await mutationQueue.addBranch();

  auto results = context.getResults();
  KJ_DEFER(containerStarted.store(false, std::memory_order_release));

  auto endpoint = kj::str("/containers/", containerName, "/wait");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));

  JSG_REQUIRE(response.statusCode == 200, Error,
      "Monitoring container failed with: ", response.statusCode, " ", response.body);

  auto jsonRoot = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(response.body);
  results.setExitCode(jsonRoot.getStatusCode());
}

kj::Promise<void> ContainerClient::destroy(DestroyContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  // Sidecar shares main container's network namespace, so must be destroyed first
  co_await destroySidecarContainer();
  co_await destroyContainer();
}

kj::Promise<void> ContainerClient::signal(SignalContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

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

void ContainerClient::upsertEgressMapping(EgressMapping mapping) {
  auto cidrStr = mapping.cidr.toString();
  for (auto& m: egressMappings) {
    if (m.port == mapping.port && m.cidr.toString() == cidrStr) {
      m.channel = kj::mv(mapping.channel);
      return;
    }
  }

  egressMappings.add(kj::mv(mapping));
}

kj::Maybe<kj::Own<workerd::IoChannelFactory::SubrequestChannel>> ContainerClient::findEgressMapping(
    kj::StringPtr destAddr, uint16_t defaultPort) {
  auto hostAndPort = stripPort(destAddr);
  uint16_t port = hostAndPort.port.orDefault(defaultPort);

  for (auto& mapping: egressMappings) {
    if (mapping.cidr.matches(hostAndPort.host)) {
      // CIDR matches, now check port.
      // If the port is 0, we match anything.
      if (mapping.port == 0 || mapping.port == port) {
        return kj::addRef(*mapping.channel);
      }
    }
  }

  return kj::none;
}

void ContainerClient::upsertEgressHttpsMapping(EgressHttpsMapping mapping) {
  for (auto& m: egressHttpsMappings) {
    if (m.sniGlob == mapping.sniGlob) {
      m.channel = kj::mv(mapping.channel);
      return;
    }
  }

  egressHttpsMappings.add(kj::mv(mapping));
}

kj::Maybe<kj::Own<workerd::IoChannelFactory::SubrequestChannel>> ContainerClient::
    findEgressHttpsMapping(kj::StringPtr sni) {
  for (auto& mapping: egressHttpsMappings) {
    if (matchSniGlob(mapping.sniGlob, sni)) {
      return kj::addRef(*mapping.channel);
    }
  }

  return kj::none;
}

kj::Promise<void> ContainerClient::ensureSidecarStarted() {
  if (containerSidecarStarted.exchange(true, std::memory_order_acquire)) {
    co_return;
  }

  // We need to call destroy here, it's mandatory that this is a fresh sidecar
  // start. Maybe we lost track of it on a previous workerd restart.
  co_await destroySidecarContainer();

  KJ_ON_SCOPE_FAILURE(containerSidecarStarted.store(false, std::memory_order_release));

  auto ipamConfig = co_await getDockerBridgeIPAMConfig();
  co_await createSidecarContainer(egressListenerPort, kj::mv(ipamConfig.subnet));
  co_await startSidecarContainer();
}

kj::Promise<void> ContainerClient::ensureEgressListenerStarted(uint16_t port) {
  if (egressListenerStarted.exchange(true, std::memory_order_acquire)) {
    co_return;
  }

  KJ_ON_SCOPE_FAILURE(egressListenerStarted.store(false, std::memory_order_release));

  // Determine the listen address: on Linux, use the Docker bridge gateway IP
  // and fall back to loopback (Docker Desktop
  // routes host-gateway to host loopback through the VM).
  auto ipamConfig = co_await getDockerBridgeIPAMConfig();
  egressListenerPort = co_await startEgressListener(
      gatewayForPlatform(kj::mv(ipamConfig.gateway)).orDefault(kj::str("127.0.0.1")), port);
}

kj::Promise<void> ContainerClient::setEgressHttp(SetEgressHttpContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  auto params = context.getParams();
  auto hostPortStr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();

  auto parsed = parseHostPort(hostPortStr);
  uint16_t port = parsed.port.orDefault(80);
  auto cidr = kj::mv(parsed.cidr);

  co_await ensureEgressListenerStarted();

  if (containerStarted.load(std::memory_order_acquire)) {
    // Only try to create and start a sidecar container
    // if the user container is running.
    co_await ensureSidecarStarted();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  upsertEgressMapping(EgressMapping{
    .cidr = kj::mv(cidr),
    .port = port,
    .channel = kj::mv(subrequestChannel),
  });

  co_return;
}

kj::Promise<void> ContainerClient::setEgressHttps(SetEgressHttpsContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  auto params = context.getParams();
  auto sniGlob = kj::str(params.getSniGlob());
  auto tokenBytes = params.getChannelToken();

  KJ_REQUIRE(sniGlob.size() > 0, "sniGlob must not be empty");
  if (sniGlob[0] == '*') {
    KJ_REQUIRE(sniGlob.size() == 1 || (sniGlob[1] == '.' && sniGlob.size() > 2),
        "wildcard glob must be \"*\" or \"*.hostname\", got: ", sniGlob);
  }
  for (size_t i = 1; i < sniGlob.size(); ++i) {
    KJ_REQUIRE(sniGlob[i] != '*', "sniGlob may only contain a leading '*', got: ", sniGlob);
  }

  co_await ensureEgressListenerStarted();

  if (containerStarted.load(std::memory_order_acquire)) {
    co_await ensureSidecarStarted();
  }

  if (containerStarted.load(std::memory_order_acquire) &&
      containerSidecarStarted.load(std::memory_order_acquire)) {
    co_await injectCACert();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  upsertEgressHttpsMapping(EgressHttpsMapping{
    .sniGlob = kj::mv(sniGlob),
    .channel = kj::mv(subrequestChannel),
  });

  co_return;
}

kj::Own<ContainerClient> ContainerClient::addRef() {
  return kj::addRef(*this);
}

}  // namespace workerd::server
