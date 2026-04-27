// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container-client.h"

#include "ada.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/worker-interface.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
#include <workerd/server/docker-api.capnp.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/strings.h>
#include <workerd/util/uuid.h>

#include <stdio.h>

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

#include <limits>

namespace workerd::server {

namespace {

constexpr uint16_t SIDECAR_INGRESS_PORT = 39001;

constexpr kj::StringPtr SIDECAR_DNS_SERVERS[] = {
  "1.1.1.1"_kj,
  "8.8.8.8"_kj,
};

// Default limit for JSON API responses (16 MiB — Docker JSON responses are small).
constexpr uint64_t MAX_JSON_RESPONSE_SIZE = 16ULL * 1024 * 1024;

constexpr kj::StringPtr SNAPSHOT_VOLUME_PREFIX = "workerd-snap-"_kj;
constexpr kj::StringPtr SNAPSHOT_CLONE_VOLUME_PREFIX = "workerd-snap-clone-"_kj;
constexpr kj::StringPtr CONTAINER_SNAPSHOT_IMAGE_PREFIX = "workerd-container-snap-"_kj;
constexpr kj::StringPtr SNAPSHOT_VOLUME_CREATED_AT_LABEL = "dev.workerd.snapshot-created-at"_kj;

// Prefix applied to user-supplied labels when writing them to the Docker container, and
// stripped back out when reading them via inspect(). Lets us distinguish labels the worker
// set via start() from labels that came from the image (via Dockerfile LABEL) or engine.
constexpr kj::StringPtr WORKERD_LABEL_PREFIX = "workerd-"_kj;
constexpr auto SNAPSHOT_STALE_AGE = 30 * kj::DAYS;

// Maximum size of a snapshot tar archive held in memory during snapshot create/restore.
constexpr size_t MAX_SNAPSHOT_TAR_SIZE = 1ULL * 1024 * 1024 * 1024;  // 1 GiB
static_assert(static_cast<double>(MAX_SNAPSHOT_TAR_SIZE) == MAX_SNAPSHOT_TAR_SIZE,
    "MAX_SNAPSHOT_TAR_SIZE must be exactly representable as double");

// POSIX tar stores file size in an 11-digit octal header field.
constexpr size_t MAX_TAR_CONTENT_SIZE = 8ull * 1024 * 1024 * 1024;

// Ensures the stale-volume check runs at most once per process.
std::atomic_bool staleSnapshotVolumeCheckScheduled = false;

struct ParsedAddress {
  kj::OneOf<kj::CidrRange, kj::String> destination;
  kj::Maybe<uint16_t> port;
};

struct HostAndPort {
  kj::String host;
  kj::Maybe<uint16_t> port;
};

struct DockerResponse {
  kj::uint statusCode;
  kj::String body;
};

struct DockerBinaryResponse {
  kj::uint statusCode;
  kj::Array<kj::byte> body;
};

struct DockerStreamedResponse {
  kj::uint statusCode;
  kj::String statusText;
  kj::Own<kj::AsyncIoStream> connection;
};

// Validates an absolute path for snapshot use and returns the parsed component path.
// Rejects relative paths, embedded null bytes, and path traversal components ("..").
kj::Path parseAbsolutePath(kj::StringPtr path) {
  JSG_REQUIRE(
      path.size() > 0 && path[0] == '/', Error, "Snapshot path must be absolute, got: ", path);

  JSG_REQUIRE(path.findFirst('\0') == kj::none, Error, "Snapshot path must not contain null bytes");

  try {
    return kj::Path::parse(path.slice(1));
  } catch (kj::Exception& e) {
    JSG_FAIL_REQUIRE(
        Error, "Snapshot path contains invalid components: ", path, "; ", e.getDescription());
  }
}

// Parse and validate a snapshot ID. Throws an error if the snapshot ID is invalid.
kj::String parseSnapshotId(kj::StringPtr snapshotId) {
  KJ_IF_SOME(uuid, UUID::fromString(snapshotId)) {
    auto s = uuid.toString();
    JSG_REQUIRE(s == snapshotId, Error, "Invalid snapshot ID", snapshotId);
    return s;
  } else {
    JSG_FAIL_REQUIRE(Error, "Invalid snapshot ID", snapshotId);
  }
}

// Really similar to BufferedInputStreamWrapper, but Async...
// We need this because of Docker's exec keeping a bidirectional connection
// needing to own the IoStream after writing and reading headers, as it does
// "Upgrade: tcp".
class BufferedAsyncIoStream final: public kj::AsyncIoStream {
 public:
  BufferedAsyncIoStream(kj::Own<kj::AsyncIoStream> inner, kj::Array<kj::byte> buffered)
      : inner(kj::mv(inner)),
        buffered(kj::mv(buffered)) {}

  kj::Promise<size_t> tryRead(void* dst, size_t minBytes, size_t maxBytes) override {
    KJ_REQUIRE(minBytes <= maxBytes, minBytes, maxBytes);

    auto out = kj::arrayPtr(reinterpret_cast<kj::byte*>(dst), maxBytes);
    size_t copied = 0;

    auto bufferedRemaining = buffered.size() - bufferedOffset;
    if (bufferedRemaining > 0) {
      auto toCopy = kj::min(maxBytes, bufferedRemaining);
      out.first(toCopy).copyFrom(buffered.asPtr().slice(bufferedOffset, bufferedOffset + toCopy));
      bufferedOffset += toCopy;
      copied = toCopy;

      if (copied >= minBytes || copied == maxBytes) {
        co_return copied;
      }
    }

    auto read = co_await inner->tryRead(out.begin() + copied, minBytes - copied, maxBytes - copied);
    co_return copied + read;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    KJ_IF_SOME(innerLength, inner->tryGetLength()) {
      return innerLength + (buffered.size() - bufferedOffset);
    }
    return kj::none;
  }

  kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
    uint64_t pumped = 0;
    auto bufferedRemaining = buffered.size() - bufferedOffset;
    if (bufferedRemaining > 0) {
      auto toWrite = static_cast<size_t>(kj::min(amount, static_cast<uint64_t>(bufferedRemaining)));
      co_await output.write(buffered.asPtr().slice(bufferedOffset, bufferedOffset + toWrite));
      bufferedOffset += toWrite;
      pumped += toWrite;

      if (pumped == amount) {
        co_return pumped;
      }
    }

    co_return pumped + co_await inner->pumpTo(output, amount - pumped);
  }

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner->write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner->write(pieces);
  }
  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount = kj::maxValue) override {
    return inner->tryPumpFrom(input, amount);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }
  void abortWrite(kj::Exception&& exception) override {
    inner->abortWrite(kj::mv(exception));
  }

  void shutdownWrite() override {
    inner->shutdownWrite();
  }
  void abortRead() override {
    inner->abortRead();
  }
  void getsockopt(int level, int option, void* value, kj::uint* length) override {
    inner->getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, kj::uint length) override {
    inner->setsockopt(level, option, value, length);
  }
  void getsockname(struct sockaddr* addr, kj::uint* length) override {
    inner->getsockname(addr, length);
  }
  void getpeername(struct sockaddr* addr, kj::uint* length) override {
    inner->getpeername(addr, length);
  }
  kj::Maybe<int> getFd() const override {
    return inner->getFd();
  }

 private:
  kj::Own<kj::AsyncIoStream> inner;
  kj::Array<kj::byte> buffered;
  size_t bufferedOffset = 0;
};

// Docker exec uses a single hijacked stream for stdin and stdout/stderr. Keep that stream in a
// small refcounted holder so the returned stdin ByteStream and the output demux task can share it.
class SharedExecConnection final: public kj::Refcounted {
 public:
  explicit SharedExecConnection(kj::Own<kj::AsyncIoStream> connection)
      : connection(kj::mv(connection)) {}

  kj::Own<kj::AsyncIoStream> connection;
  bool stdinOpened = false;
  bool stdinClosed = false;
};

class DockerExecStdinStream final: public capnp::ExplicitEndOutputStream {
 public:
  explicit DockerExecStdinStream(kj::Own<SharedExecConnection> sharedConnection)
      : sharedConnection(kj::mv(sharedConnection)) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return sharedConnection->connection->write(buffer);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return sharedConnection->connection->write(pieces);
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return sharedConnection->connection->whenWriteDisconnected();
  }

  kj::Promise<void> end() override {
    if (!sharedConnection->stdinClosed) {
      sharedConnection->connection->shutdownWrite();
      sharedConnection->stdinClosed = true;
    }
    return kj::READY_NOW;
  }

 private:
  kj::Own<SharedExecConnection> sharedConnection;
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

kj::Maybe<kj::CidrRange> tryMakeCidr(kj::StringPtr host) {
  kj::Maybe<kj::CidrRange> cidr;
  KJ_IF_SOME(_, kj::runCatchingExceptions([&]() { cidr = makeCidr(host); })) {
    return kj::none;
  }

  return kj::mv(cidr);
}

// normalizeHostname normalizes the hostname. It's designed to receive the hostname when
// proxy-everything sends the HTTP CONNECT with the X-Hostname hint.
kj::String normalizeHostname(kj::StringPtr hostname) {
  auto url = kj::str("http://", hostname);
  auto parsed = ada::parse<ada::url_aggregator>({url.begin(), url.size()}, nullptr);
  KJ_REQUIRE(parsed.has_value(), "Invalid X-Hostname URL hint.", hostname);
  auto normalizedHostname = parsed->get_hostname();
  return kj::heapString(normalizedHostname.data(), normalizedHostname.size());
}

// hostnameGlobMatches should match patterns like:
//   cloudflare.*.com
//   cloudflare.com
//   cloudflare
//   *
//
// hostname must be normalized beforehand
bool hostnameGlobMatches(kj::StringPtr pattern, kj::StringPtr hostname) {
  size_t patternIndex = 0;
  size_t hostnameIndex = 0;
  size_t restartHostnameIndex = 0;
  kj::Maybe<size_t> starPatternIndex;

  while (hostnameIndex < hostname.size()) {
    if (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
      starPatternIndex = patternIndex++;
      restartHostnameIndex = hostnameIndex;
      continue;
    }

    if (patternIndex < pattern.size() && pattern[patternIndex] == hostname[hostnameIndex]) {
      ++patternIndex;
      ++hostnameIndex;
      continue;
    }

    KJ_IF_SOME(starIndex, starPatternIndex) {
      patternIndex = starIndex + 1;
      hostnameIndex = ++restartHostnameIndex;
      continue;
    }

    return false;
  }

  while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
    ++patternIndex;
  }

  return patternIndex == pattern.size();
}

kj::Maybe<kj::StringPtr> getHeader(const kj::HttpHeaders& headers, kj::StringPtr name) {
  kj::Maybe<kj::StringPtr> result;
  headers.forEach([&](kj::StringPtr headerName, kj::StringPtr value) {
    if (result == kj::none && workerd::strcaseeq(headerName, name)) {
      result = value;
    }
  });
  return result;
}

// Parses "host[:port]" strings. Handles:
// - IPv4: "10.0.0.1", "10.0.0.1:8080", "10.0.0.0/8", "10.0.0.0/8:8080"
// - IPv6 with brackets: "[::1]", "[::1]:8080", "[fe80::1]", "[fe80::/10]:8080"
// - IPv6 without brackets: "::1", "fe80::1", "fe80::/10"
ParsedAddress parseHostPort(kj::StringPtr str) {
  auto hostAndPort = stripPort(str);
  KJ_REQUIRE(hostAndPort.host.size() > 0, "Host must not be empty.", str);

  KJ_IF_SOME(cidr, tryMakeCidr(hostAndPort.host)) {
    return {
      .destination = kj::mv(cidr),
      .port = hostAndPort.port,
    };
  }

  return {
    .destination = workerd::toLower(hostAndPort.host),
    .port = hostAndPort.port,
  };
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

void writeTarField(kj::ArrayPtr<kj::byte> field, kj::StringPtr value) {
  auto len = kj::min(value.size(), field.size());
  field.first(len).copyFrom(value.asBytes().first(len));
}

// createTarWithFile creates simple tar files without importing a full blown TAR library.
// It's a pretty limited method that creates a single tar file with a single file on it,
// as the Docker API only accepts tars.
kj::Array<kj::byte> createTarWithFile(
    kj::StringPtr filename, kj::ArrayPtr<const kj::byte> content) {
  KJ_REQUIRE(filename.size() < 100, "tar filename must be < 100 bytes");
  KJ_REQUIRE(content.size() < MAX_TAR_CONTENT_SIZE, "tar content too large for 11-digit octal");

  size_t paddedSize = (content.size() + 511) & ~static_cast<size_t>(511);
  size_t totalSize = 512 + paddedSize + 1024;
  auto tar = kj::heapArray<kj::byte>(totalSize);
  tar.asPtr().fill(0);

  auto header = tar.first(512);
  writeTarField(header.slice(0, 100), filename);
  writeTarField(header.slice(100, 108), "0000644"_kj);
  writeTarField(header.slice(108, 116), "0000000"_kj);
  writeTarField(header.slice(116, 124), "0000000"_kj);

  {
    char sizeBuf[12];
    snprintf(sizeBuf, sizeof(sizeBuf), "%011" PRIo64, static_cast<uint64_t>(content.size()));
    writeTarField(header.slice(124, 136), kj::StringPtr(sizeBuf));
  }

  writeTarField(header.slice(136, 148), "00000000000"_kj);
  header[156] = '0';
  writeTarField(header.slice(257, 263), "ustar"_kj);
  writeTarField(header.slice(263, 265), "00"_kj);

  header.slice(148, 156).fill(' ');
  uint32_t checksum = 0;
  for (auto byte: header) checksum += byte;

  {
    char checksumBuf[8];
    snprintf(checksumBuf, sizeof(checksumBuf), "%06o ", checksum);
    writeTarField(header.slice(148, 155), kj::StringPtr(checksumBuf));
  }

  tar.slice(512, 512 + content.size()).copyFrom(content);
  return tar;
}

// Shared Docker API HTTP helper. Connects to the Docker socket, sends a request with an
// optional body, and reads the response as raw bytes.
kj::Promise<DockerBinaryResponse> dockerApiRequestRaw(kj::Network& network,
    kj::String dockerPath,
    kj::HttpMethod method,
    kj::String endpoint,
    kj::Maybe<kj::ArrayPtr<const kj::byte>> body,
    kj::StringPtr contentType,
    uint64_t maxResponseSize) {
  kj::HttpHeaderTable headerTable;
  auto address = co_await network.parseAddress(dockerPath);
  auto connection = co_await address->connect();
  auto httpClient = kj::newHttpClient(headerTable, *connection).attach(kj::mv(connection));
  kj::HttpHeaders headers(headerTable);
  headers.setPtr(kj::HttpHeaderId::HOST, "localhost");

  KJ_IF_SOME(requestBody, body) {
    headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, contentType);
    headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(requestBody.size()));

    auto req = httpClient->request(method, endpoint, headers, requestBody.size());
    {
      auto stream = kj::mv(req.body);
      co_await stream->write(requestBody);
    }
    auto response = co_await req.response;
    auto result = co_await response.body->readAllBytes(maxResponseSize);
    co_return DockerBinaryResponse{.statusCode = response.statusCode, .body = kj::mv(result)};
  } else {
    auto req = httpClient->request(method, endpoint, headers);
    { auto stream = kj::mv(req.body); }
    auto response = co_await req.response;
    auto result = co_await response.body->readAllBytes(maxResponseSize);
    co_return DockerBinaryResponse{.statusCode = response.statusCode, .body = kj::mv(result)};
  }
}

kj::Promise<DockerResponse> dockerApiRequest(kj::Network& network,
    kj::String dockerPath,
    kj::HttpMethod method,
    kj::String endpoint,
    kj::Maybe<kj::String> body = kj::none) {
  kj::Maybe<kj::ArrayPtr<const kj::byte>> bodyBytes;
  KJ_IF_SOME(b, body) {
    bodyBytes = b.asBytes();
  }
  auto raw = co_await dockerApiRequestRaw(network, kj::mv(dockerPath), method, kj::mv(endpoint),
      bodyBytes, "application/json"_kj, MAX_JSON_RESPONSE_SIZE);
  co_return DockerResponse{.statusCode = raw.statusCode, .body = kj::str(raw.body.asChars())};
}

kj::Promise<DockerBinaryResponse> dockerApiBinaryRequest(kj::Network& network,
    kj::String dockerPath,
    kj::HttpMethod method,
    kj::String endpoint,
    kj::Maybe<kj::Array<kj::byte>> body,
    uint64_t maxResponseSize) {
  kj::Maybe<kj::ArrayPtr<const kj::byte>> bodyBytes;
  KJ_IF_SOME(b, body) {
    bodyBytes = b.asPtr();
  }
  co_return co_await dockerApiRequestRaw(network, kj::mv(dockerPath), method, kj::mv(endpoint),
      bodyBytes, "application/x-tar"_kj, maxResponseSize);
}

kj::Promise<void> deleteVolume(kj::Network& network, kj::String dockerPath, kj::String volumeName) {
  auto response = co_await dockerApiRequest(
      network, kj::mv(dockerPath), kj::HttpMethod::DELETE, kj::str("/volumes/", volumeName));
  if (response.statusCode != 204 && response.statusCode != 404) {
    KJ_LOG(WARNING, "failed to delete volume", volumeName, response.statusCode, response.body);
  }
}

kj::Promise<void> deleteVolumes(
    kj::Network& network, kj::String dockerPath, kj::Array<kj::String> snapshotCloneVolumes) {
  kj::Vector<kj::Promise<void>> volumeDeletes;
  volumeDeletes.reserve(snapshotCloneVolumes.size());
  for (auto& volumeName: snapshotCloneVolumes) {
    auto logName = kj::str(volumeName);
    volumeDeletes.add(deleteVolume(network, kj::str(dockerPath), kj::mv(volumeName))
                          .catch_([logName = kj::mv(logName)](kj::Exception&& e) {
      KJ_LOG(WARNING, "failed to delete volume", logName, e);
    }));
  }
  co_await kj::joinPromises(volumeDeletes.releaseAsArray());
}

kj::Promise<void> removeContainer(
    kj::Network& network, kj::String dockerPath, kj::String containerName, bool wait = true) {
  auto endpoint = kj::str("/containers/", containerName, "?force=true");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::mv(endpoint));
  // 204 means the container was removed.
  // 404 means it was already gone.
  // 409 means removal is already in progress, which is fine for our teardown paths.
  KJ_REQUIRE(response.statusCode == 204 || response.statusCode == 404 || response.statusCode == 409,
      "Removing a container failed with: ", response.body);

  // If removal succeeded or is already in progress, wait for Docker to report the container as
  // fully removed before proceeding with any follow-up cleanup like deleting mounted volumes.
  if (wait && (response.statusCode == 204 || response.statusCode == 409)) {
    response = co_await dockerApiRequest(network, kj::mv(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/", containerName, "/wait?condition=removed"));
    // 200 means Docker observed the removal. 404 means the container disappeared before the wait
    // request was processed, which is also fine.
    KJ_REQUIRE(response.statusCode == 200 || response.statusCode == 404,
        "Waiting for container removal failed with: ", response.statusCode, response.body);
  }
}

kj::Maybe<size_t> tryFindHttpHeaderEnd(kj::ArrayPtr<const kj::byte> bytes) {
  for (auto i: kj::zeroTo(bytes.size())) {
    if (i + 4 > bytes.size()) {
      return kj::none;
    }
    if (bytes[i] == '\r' && bytes[i + 1] == '\n' && bytes[i + 2] == '\r' && bytes[i + 3] == '\n') {
      return i;
    }
  }
  return kj::none;
}

// readDockerStreamedResponse is necessary because Docker streamed responses
// require an open bidirectional stream after the response headers have been read.
kj::Promise<DockerStreamedResponse> readDockerStreamedResponse(
    kj::Own<kj::AsyncIoStream> connection) {
  kj::Vector<kj::byte> buffer;
  auto& input = *connection;

  while (true) {
    KJ_IF_SOME(headerEnd, tryFindHttpHeaderEnd(buffer.asPtr())) {
      auto parsedHeaders = kj::heapArray<char>(headerEnd + 2);
      for (auto i: kj::zeroTo(parsedHeaders.size())) {
        parsedHeaders[i] = static_cast<char>(buffer[i]);
      }

      kj::HttpHeaderTable headerTable;
      kj::HttpHeaders headers(headerTable);
      auto parsedResponse = headers.tryParseResponse(parsedHeaders.asPtr());
      headers.takeOwnership(kj::mv(parsedHeaders));

      kj::uint statusCode = 0;
      kj::String statusText;
      KJ_SWITCH_ONEOF(parsedResponse) {
        KJ_CASE_ONEOF(response, kj::HttpHeaders::Response) {
          statusCode = response.statusCode;
          statusText = kj::str(response.statusText);
        }
        KJ_CASE_ONEOF(protocolError, kj::HttpHeaders::ProtocolError) {
          KJ_FAIL_REQUIRE("Docker streamed response returned malformed HTTP headers: ",
              protocolError.statusMessage, ": ", protocolError.description);
        }
      }

      auto bodyOffset = headerEnd + 4;
      auto prefetchedBytes = kj::heapArray(buffer.asPtr().slice(bodyOffset));
      kj::Own<kj::AsyncIoStream> prefixedConnection = kj::mv(connection);
      if (prefetchedBytes.size() > 0) {
        prefixedConnection =
            kj::heap<BufferedAsyncIoStream>(kj::mv(prefixedConnection), kj::mv(prefetchedBytes));
      }
      co_return DockerStreamedResponse{
        .statusCode = statusCode,
        .statusText = kj::mv(statusText),
        .connection = kj::mv(prefixedConnection),
      };
    }

    auto scratch = kj::heapArray<kj::byte>(4096);
    auto amount = co_await input.tryRead(scratch.begin(), 1, scratch.size());
    KJ_REQUIRE(amount > 0, "EOF while waiting for Docker streamed response headers");
    buffer.addAll(scratch.first(amount));
    KJ_REQUIRE(buffer.size() <= 65536, "Docker streamed response headers exceeded 64KiB");
  }
}

kj::Promise<DockerStreamedResponse> dockerApiStreamedRequest(kj::Network& network,
    kj::String dockerPath,
    kj::HttpMethod method,
    kj::String endpoint,
    const kj::HttpHeaders& headers,
    kj::Maybe<kj::ArrayPtr<const kj::byte>> body = kj::none) {
  auto address = co_await network.parseAddress(dockerPath);
  auto connection = co_await address->connect();

  auto requestHeaders = headers.serializeRequest(method, endpoint);
  KJ_IF_SOME(requestBody, body) {
    kj::ArrayPtr<const kj::byte> pieces[] = {requestHeaders.asBytes(), requestBody};
    co_await connection->write(kj::arrayPtr(pieces));
  } else {
    co_await connection->write(requestHeaders.asBytes());
  }

  co_return co_await readDockerStreamedResponse(kj::mv(connection));
}

// Docker multiplexed stream frames: 1 byte stream ID + 3 reserved + 4 bytes big-endian length.
constexpr size_t DOCKER_FRAME_HEADER_SIZE = 8;

uint32_t parseDockerFrameLength(kj::ArrayPtr<const kj::byte> frameHeader) {
  KJ_REQUIRE(frameHeader.size() >= DOCKER_FRAME_HEADER_SIZE, "Docker raw stream header too short");
  return (static_cast<uint32_t>(frameHeader[4]) << 24) |
      (static_cast<uint32_t>(frameHeader[5]) << 16) | (static_cast<uint32_t>(frameHeader[6]) << 8) |
      static_cast<uint32_t>(frameHeader[7]);
}

void detachEnd(kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> stream) {
  KJ_IF_SOME(s, stream) {
    s->end().attach(kj::mv(s)).detach([](kj::Exception&&) {});
  }
}

// demuxDockerExecOutput demuxes the input from Docker to passed stdout/stderr.
kj::Promise<void> demuxDockerExecOutput(kj::AsyncInputStream& input,
    kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> stdoutWriter,
    kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> stderrWriter,
    bool combinedOutput) {
  kj::Vector<kj::byte> buffer;
  size_t offset = 0;

  auto compactBuffer = [&]() {
    if (offset == 0) {
      return;
    }

    kj::Vector<kj::byte> compacted;
    compacted.addAll(buffer.asPtr().slice(offset));
    buffer = kj::mv(compacted);
    offset = 0;
  };

  auto ensureBytes = [&](size_t count) -> kj::Promise<bool> {
    while (buffer.size() - offset < count) {
      compactBuffer();
      auto scratch = kj::heapArray<kj::byte>(4096);
      auto amount = co_await input.tryRead(scratch.begin(), 1, scratch.size());
      if (amount == 0) {
        co_return false;
      }
      buffer.addAll(scratch.first(amount));
    }
    co_return true;
  };

  try {
    while (co_await ensureBytes(DOCKER_FRAME_HEADER_SIZE)) {
      auto frameHeader = buffer.asPtr().slice(offset, offset + DOCKER_FRAME_HEADER_SIZE);
      auto streamId = frameHeader[0];
      auto frameLength = parseDockerFrameLength(frameHeader);
      KJ_REQUIRE(co_await ensureBytes(DOCKER_FRAME_HEADER_SIZE + frameLength),
          "Docker exec raw stream ended in the middle of a frame");

      auto payload = buffer.asPtr().slice(
          offset + DOCKER_FRAME_HEADER_SIZE, offset + DOCKER_FRAME_HEADER_SIZE + frameLength);
      if (streamId == 1) {
        KJ_IF_SOME(out, stdoutWriter) {
          co_await out->write(payload);
        }
      } else {
        if (streamId == 2) {
          if (combinedOutput) {
            KJ_IF_SOME(out, stdoutWriter) {
              co_await out->write(payload);
            }
          } else {
            KJ_IF_SOME(err, stderrWriter) {
              co_await err->write(payload);
            }
          }
        }
      }

      offset += DOCKER_FRAME_HEADER_SIZE + frameLength;
    }

    if (buffer.size() != offset) {
      KJ_FAIL_REQUIRE("Docker exec raw stream ended with a truncated frame header");
    }

    // We need to detach ourselves from the end() as the user might've
    // decided to not read them altogether.
    detachEnd(kj::mv(stdoutWriter));
    detachEnd(kj::mv(stderrWriter));
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();

    KJ_IF_SOME(out, stdoutWriter) {
      out->abortWrite(exception.clone());
    }
    KJ_IF_SOME(err, stderrWriter) {
      err->abortWrite(exception.clone());
    }
    kj::throwFatalException(kj::mv(exception));
  }
}

kj::String currentSnapshotVolumeTimestamp() {
  return kj::str((kj::systemPreciseCalendarClock().now() - kj::UNIX_EPOCH) / kj::SECONDS);
}

kj::Maybe<int64_t> tryGetSnapshotCreatedAt(capnp::JsonValue::Reader labels) {
  if (!labels.isObject()) {
    return kj::none;
  }

  for (auto field: labels.getObject()) {
    if (field.getName() != SNAPSHOT_VOLUME_CREATED_AT_LABEL) {
      continue;
    }

    auto value = field.getValue();
    if (!value.isString()) {
      return kj::none;
    }
    return value.getString().tryParseAs<int64_t>();
  }

  return kj::none;
}

kj::Promise<void> warnAboutStaleSnapshotVolumes(kj::Network& network, kj::String dockerPath) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::VolumeListFilters>();
  capnp::MallocMessageBuilder filterMessage;
  auto filters = filterMessage.initRoot<docker_api::Docker::VolumeListFilters>();
  auto names = filters.initName(1);
  names.set(0, SNAPSHOT_VOLUME_PREFIX);

  auto response = co_await dockerApiRequest(network, kj::mv(dockerPath), kj::HttpMethod::GET,
      kj::str("/volumes?filters=", kj::encodeUriComponent(codec.encode(filters))));
  if (response.statusCode != 200) {
    co_return;
  }

  auto message = decodeJsonResponse<docker_api::Docker::VolumeListResponse>(response.body);
  auto root = message->getRoot<docker_api::Docker::VolumeListResponse>();
  auto now = kj::systemPreciseCalendarClock().now();
  kj::Vector<kj::String> staleVolumes;

  for (auto volume: root.getVolumes()) {
    KJ_IF_SOME(createdAtSeconds, tryGetSnapshotCreatedAt(volume.getLabels())) {
      auto createdAt = kj::UNIX_EPOCH + createdAtSeconds * kj::SECONDS;
      if (now - createdAt >= SNAPSHOT_STALE_AGE) {
        staleVolumes.add(kj::str(volume.getName()));
      }
    }
  }

  if (!staleVolumes.empty()) {
    KJ_LOG(WARNING, "the following snapshot volumes were created 30+ days ago and may be stale",
        kj::strArray(staleVolumes, ", "));
  }
}

// Returns the gateway IP on Linux for direct container access.
// Returns kj::none on macOS where Docker Desktop routes host-gateway to host loopback.
kj::Maybe<kj::String> gatewayForPlatform(kj::String gateway) {
#ifdef __APPLE__
  return kj::none;
#else
  return kj::mv(gateway);
#endif
}

kj::Maybe<uint16_t> tryParsePublishedHostPort(capnp::json::Value::Reader portMappingValue) {
  if (portMappingValue.isNull()) {
    return kj::none;
  }

  JSG_REQUIRE(
      portMappingValue.isArray(), Error, "Malformed ContainerInspect port mapping response");
  auto bindings = portMappingValue.getArray();
  if (bindings.size() == 0) {
    return kj::none;
  }

  auto binding = bindings[0];
  JSG_REQUIRE(binding.isObject(), Error, "Malformed ContainerInspect port binding response");
  for (auto field: binding.getObject()) {
    if (field.getName() == "HostPort") {
      auto value = field.getValue();
      JSG_REQUIRE(value.isString(), Error, "Malformed ContainerInspect port binding response");
      kj::StringPtr hostPort = value.getString();
      return KJ_REQUIRE_NONNULL(
          hostPort.tryParseAs<uint16_t>(), "Malformed ContainerInspect host port");
    }
  }

  KJ_FAIL_REQUIRE("Malformed ContainerInspect port binding response: missing HostPort");
}

}  // namespace

// Represents a parsed egress mapping. IP/CIDR mappings match destination IPs,
// while hostnameGlob mappings match either HTTP hostnames or TLS SNI depending on protocol.
// Defined here (not in the header) to avoid pulling kj::OneOf, kj::CidrRange, and
// kj::Vector into server.c++ which includes container-client.h.
struct ContainerClient::EgressMapping {
  kj::OneOf<kj::CidrRange, kj::String> destination;
  uint16_t port;  // 0 means match all ports
  EgressProtocol protocol;
  kj::Own<workerd::IoChannelFactory::SubrequestChannel> channel;
};

// Holds all egress mapping state. Stored via kj::Own<EgressState> in ContainerClient
// so that the EgressMapping type is not visible in container-client.h.
struct ContainerClient::EgressState {
  kj::Vector<EgressMapping> mappings;
};

ContainerClient::ContainerClient(capnp::ByteStreamFactory& byteStreamFactory,
    kj::Timer& timer,
    kj::Network& network,
    kj::String dockerPath,
    kj::String containerName,
    kj::String imageName,
    kj::String containerEgressInterceptorImage,
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
      channelTokenHandler(channelTokenHandler),
      egressState(kj::heap<EgressState>()) {
  if (!staleSnapshotVolumeCheckScheduled.exchange(true)) {
    waitUntilTasks.add(warnAboutStaleSnapshotVolumes(network, kj::str(this->dockerPath))
                           .catch_([](kj::Exception&& e) {
      KJ_LOG(WARNING, "failed to inspect snapshot volumes for staleness", e);
    }));
  }
}

ContainerClient::~ContainerClient() noexcept(false) {
  stopEgressListener();

  // Best-effort cleanup for both containers.
  auto sidecarCleanup =
      removeContainer(network, kj::str(dockerPath), kj::str(sidecarContainerName), false)
          .catch_([](kj::Exception&&) {});

  // Also try to delete any cloned snapshot volumes.
  auto volumes = snapshotClones.releaseAsArray();
  auto mainCleanup = removeContainer(network, kj::str(dockerPath), kj::str(containerName))
                         .catch_([](kj::Exception&&) {})
                         .then([&network = network, dockerPath = kj::str(dockerPath),
                                   volumes = kj::mv(volumes)]() mutable {
    return deleteVolumes(network, kj::mv(dockerPath), kj::mv(volumes));
  }).catch_([](kj::Exception&&) {});

  // Pass the joined cleanup promise to the callback. The callback wraps it with the
  // canceler (so a future client creation can cancel it), stores it so the next
  // ContainerClient can await it, and adds a branch to waitUntilTasks to keep the
  // underlying I/O alive.
  cleanupCallback(kj::joinPromises(kj::arr(kj::mv(sidecarCleanup), kj::mv(mainCleanup))));
}

// Docker-specific Port implementation that implements rpc::Container::Port::Server
// It does a HTTP CONNECT to the proxy-everything sidecar port.
class ContainerClient::DockerPort final: public rpc::Container::Port::Server {
 public:
  DockerPort(ContainerClient& containerClient, kj::String containerHost, uint16_t containerPort)
      : containerClient(containerClient),
        containerHost(kj::mv(containerHost)),
        containerPort(containerPort) {}

  kj::Promise<void> connect(ConnectContext context) override {
    auto mappedPort = JSG_REQUIRE_NONNULL(containerClient.sidecarIngressHostPort, Error,
        "connect(): Container ingress proxy is not running.");

    auto dstAddr = kj::str(containerHost, ":", containerPort);

    auto address = co_await containerClient.network.parseAddress(kj::str("127.0.0.1:", mappedPort));

    kj::HttpHeaderTable::Builder headerTableBuilder;
    auto xDstAddrHeader = headerTableBuilder.add("X-Dst-Addr");
    auto headerTable = headerTableBuilder.build();
    kj::HttpHeaders headers(*headerTable);
    headers.set(xDstAddrHeader, kj::str(dstAddr));

    auto proxyConnection = co_await address->connect();
    auto httpClient = kj::newHttpClient(*headerTable, *proxyConnection)
                          .attach(kj::mv(proxyConnection), kj::mv(headerTable));
    auto connectRequest = httpClient->connect(dstAddr, headers, {});
    auto status = co_await kj::mv(connectRequest.status);

    if (status.statusCode == 400) {
      throw JSG_KJ_EXCEPTION(
          DISCONNECTED, Error, "Container is not listening to port ", containerPort);
    }

    if (status.statusCode < 200 || status.statusCode >= 300) {
      KJ_IF_SOME(errorBody, status.errorBody) {
        auto errorBodyText = co_await errorBody->readAllText();
        JSG_FAIL_REQUIRE(Error, "Connecting to container port through proxy-everything failed: [",
            status.statusCode, "] ", status.statusText, " ", errorBodyText);
      }

      JSG_FAIL_REQUIRE(Error, "Connecting to container port through proxy-everything failed: [",
          status.statusCode, "] ", status.statusText);
    }

    auto connection = kj::mv(connectRequest.connection);
    auto upPipe = kj::newOneWayPipe();
    auto upEnd = kj::mv(upPipe.in);
    auto results = context.getResults();
    results.setUp(containerClient.byteStreamFactory.kjToCapnp(kj::mv(upPipe.out)));
    auto downEnd = containerClient.byteStreamFactory.capnpToKj(context.getParams().getDown());

    pumpTask =
        kj::joinPromisesFailFast(kj::arr(upEnd->pumpTo(*connection), connection->pumpTo(*downEnd)))
            .ignoreResult()
            .attach(kj::mv(httpClient), kj::mv(upEnd), kj::mv(connection), kj::mv(downEnd));
    co_return;
  }

 private:
  // ContainerClient is owned by the Worker::Actor and keeps it alive.
  ContainerClient& containerClient;
  kj::String containerHost;
  uint16_t containerPort;
  kj::Maybe<kj::Promise<void>> pumpTask;
};

class ContainerClient::DockerProcessHandle final: public rpc::Container::ProcessHandle::Server {
 public:
  DockerProcessHandle(ContainerClient& containerClient,
      kj::String execId,
      kj::Own<kj::AsyncIoStream> connection,
      kj::Maybe<capnp::ByteStream::Client> stdoutWriter,
      kj::Maybe<capnp::ByteStream::Client> stderrWriter,
      bool combinedOutput)
      : containerClient(containerClient.addRef()),
        execId(kj::mv(execId)),
        sharedConnection(kj::refcounted<SharedExecConnection>(kj::mv(connection))) {
    kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> stdoutStream = kj::none;
    KJ_IF_SOME(out, stdoutWriter) {
      stdoutStream = this->containerClient->byteStreamFactory.capnpToKjExplicitEnd(out);
    } else {
      stdoutStream = capnp::ExplicitEndOutputStream::wrap(newNullOutputStream(), []() {});
    }

    kj::Maybe<kj::Own<capnp::ExplicitEndOutputStream>> stderrStream = kj::none;
    KJ_IF_SOME(err, stderrWriter) {
      stderrStream = this->containerClient->byteStreamFactory.capnpToKjExplicitEnd(err);
    } else if (!combinedOutput) {
      stderrStream = capnp::ExplicitEndOutputStream::wrap(newNullOutputStream(), []() {});
    }

    // Always drain the Docker exec stream. This lets wait() use stream closure as the primary
    // process-completion signal, even when stdout/stderr are ignored.
    auto task = demuxDockerExecOutput(
        *sharedConnection->connection, kj::mv(stdoutStream), kj::mv(stderrStream), combinedOutput)
                    .attach(this->containerClient->addRef(), kj::addRef(*sharedConnection));
    streamClosedTask = kj::mv(task).fork();
  }

  kj::Promise<void> wait(WaitContext context) override {
    waitStarted = true;
    if (!sharedConnection->stdinOpened && !sharedConnection->stdinClosed) {
      sharedConnection->connection->shutdownWrite();
      sharedConnection->stdinClosed = true;
    }

    co_await KJ_ASSERT_NONNULL(streamClosedTask).addBranch();

    // Docker's exec-inspect state can lag slightly behind the hijacked stream closing, so after
    // we observe EOF we allow a short bounded retry window to obtain the final exit code.
    for (auto attempt: kj::zeroTo(20)) {
      auto inspect = co_await containerClient->inspectExec(execId);
      if (!inspect.running) {
        context.getResults().setExitCode(inspect.exitCode);
        co_return;
      }

      if (attempt + 1 < 20) {
        co_await containerClient->timer.afterDelay(50 * kj::MILLISECONDS);
      }
    }

    JSG_FAIL_REQUIRE(Error, "Docker exec stream closed before exit status became available.");
  }

  kj::Promise<void> stdinWriter(StdinWriterContext context) override {
    JSG_REQUIRE(!waitStarted, Error, "Process stdinWriter() cannot be called after wait().");
    JSG_REQUIRE(
        !sharedConnection->stdinOpened, Error, "Process stdinWriter() can only be called once.");

    sharedConnection->stdinOpened = true;
    context.getResults().setWriter(containerClient->byteStreamFactory.kjToCapnp(
        kj::heap<DockerExecStdinStream>(kj::addRef(*sharedConnection))));
    co_return;
  }

  kj::Promise<void> kill(KillContext context) override {
    auto inspect = co_await containerClient->inspectExec(execId);
    JSG_REQUIRE(inspect.pid > 0, Error, "Exec process does not have a visible pid to signal.");

    auto signal = kj::str("-", signalToString(context.getParams().getSigno()));
    auto pid = kj::str(inspect.pid);
    auto cmd = kj::arr(kj::str("kill"), kj::mv(signal), kj::mv(pid));
    co_await containerClient->runSimpleExec(cmd.asPtr());
  }

 private:
  kj::Own<ContainerClient> containerClient;
  kj::String execId;
  kj::Own<SharedExecConnection> sharedConnection;
  bool waitStarted = false;
  kj::Maybe<kj::ForkedPromise<void>> streamClosedTask;
};

// ConnectResponse adapter for TCP egress. Since we've already accepted the sidecar's HTTP
// CONNECT before calling worker->connect(), this adapter simply records the worker's
// accept/reject decision without sending anything on the wire.
class TcpEgressConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    // Worker accepted the connection. Nothing additional to do since we already
    // accepted the sidecar CONNECT.
  }

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    KJ_FAIL_REQUIRE("TCP egress worker rejected the connection: ", statusCode, " ", statusText);
  }
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

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    auto destAddr = kj::str(host);
    if (co_await handleConnectMode(destAddr, headers, connection, response, "X-Tls-Sni",
            /*defaultPort=*/443, EgressProtocol::HTTPS)) {
      co_return;
    }

    if (co_await handleConnectMode(destAddr, headers, connection, response, "X-Hostname",
            /*defaultPort=*/80, EgressProtocol::HTTP)) {
      co_return;
    }

    // Try raw TCP mapping before falling through to passthrough. TCP mappings match
    // on IP/CIDR + port without any application-layer hostname information.
    if (co_await handleTcpConnect(destAddr, connection, response)) {
      co_return;
    }

    kj::HttpHeaders responseHeaders(headerTable);
    // 202 is interpreted by proxy-everything as "just send bytes as-is".
    // If the connection was TLS, it's useful so we just proxy transparently
    // to the internet.
    response.accept(202, "Accepted", responseHeaders);

    co_await passThroughConnection(destAddr, connection);
  }

 private:
  kj::Promise<bool> handleConnectMode(kj::StringPtr destAddr,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::StringPtr hostnameHeader,
      uint16_t defaultPort,
      EgressProtocol protocol) {
    kj::Maybe<kj::String> requestHostname;
    KJ_IF_SOME(value, getHeader(headers, hostnameHeader)) {
      requestHostname = kj::str(value);
    }

    auto mapping = containerClient.findEgressMapping(destAddr, defaultPort,
        requestHostname.map([](auto& hostname) {
      return kj::Maybe<kj::StringPtr>(hostname);
    }).orDefault(kj::none),
        protocol);

    if (requestHostname == kj::none && mapping == kj::none) {
      co_return false;
    }

    if (mapping != kj::none) {
      kj::HttpHeaders responseHeaders(headerTable);
      response.accept(200, "OK", responseHeaders);

      bool isTls = (protocol == EgressProtocol::HTTPS);
      auto innerService = kj::heap<InnerEgressService>(
          [&client = containerClient, addr = kj::str(destAddr),
              hostname = requestHostname.map([](auto& value) { return kj::str(value); }),
              defaultPort,
              protocol]() mutable -> kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> {
        return client.findEgressMapping(addr, defaultPort,
            hostname.map([](auto& value) {
          return kj::Maybe<kj::StringPtr>(value);
        }).orDefault(kj::none),
            protocol);
      },
          destAddr, isTls);
      auto innerServer =
          kj::heap<kj::HttpServer>(containerClient.timer, headerTable, *innerService);

      co_await innerServer->listenHttpCleanDrain(connection);
      co_return true;
    }

    kj::HttpHeaders responseHeaders(headerTable);
    response.accept(202, "Accepted", responseHeaders);

    co_await passThroughConnection(destAddr, connection);
    co_return true;
  }

  // Handles raw TCP egress by forwarding the sidecar tunnel to the worker's connect() handler.
  // Returns true if a TCP mapping matched and the connection was handled.
  kj::Promise<bool> handleTcpConnect(
      kj::StringPtr destAddr, kj::AsyncIoStream& connection, ConnectResponse& response) {
    // For TCP, we match on IP:port only — no hostname matching since raw TCP
    // doesn't carry application-layer hostname information.
    auto mapping = containerClient.findEgressMapping(
        destAddr, /*defaultPort=*/0, /*hostname=*/kj::none, EgressProtocol::TCP);

    if (mapping == kj::none) {
      co_return false;
    }

    auto& channel = KJ_ASSERT_NONNULL(mapping);

    kj::HttpHeaders responseHeaders(headerTable);
    // 202 tells proxy-everything to send bytes as-is without attempting to
    // interpret the stream (e.g. if the underlying TCP carries TLS).
    response.accept(202, "Accepted", responseHeaders);

    IoChannelFactory::SubrequestMetadata metadata;
    auto worker = channel->startRequest(kj::mv(metadata));

    // Bridge the sidecar tunnel to the worker's connect() handler. The worker entrypoint
    // is expected to implement connect() (e.g., a WorkerEntrypoint that proxies TCP).
    // We provide a simple ConnectResponse adapter since we've already accepted the
    // sidecar's CONNECT above.
    TcpEgressConnectResponse tcpResponse;
    kj::HttpHeaders connectHeaders(headerTable);
    co_await worker->connect(destAddr, connectHeaders, connection, tcpResponse, {});

    co_return true;
  }

  kj::Promise<void> passThroughConnection(kj::StringPtr destAddr, kj::AsyncIoStream& connection) {
    if (!containerClient.internetEnabled.orDefault(false)) {
      connection.shutdownWrite();
      co_return;
    }

    auto addr = co_await containerClient.network.parseAddress(destAddr);
    auto destConn = co_await addr->connect();
    co_await pumpBidirectional(connection, *destConn);
    co_return;
  }

  ContainerClient& containerClient;
  kj::HttpHeaderTable& headerTable;
};

kj::Promise<ContainerClient::IPAMConfigResult> ContainerClient::getDockerBridgeIPAMConfig() {
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/networks/bridge"));
  if (response.statusCode == 200) {
    auto message = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(response.body);
    auto jsonRoot = message->getRoot<docker_api::Docker::NetworkInspectResponse>();
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

  auto message = decodeJsonResponse<docker_api::Docker::NetworkInspectResponse>(response.body);
  auto jsonRoot = message->getRoot<docker_api::Docker::NetworkInspectResponse>();
  for (auto config: jsonRoot.getIpam().getConfig()) {
    // IPv6 subnets contain ':' (e.g. "fd00::/80", "2001:db8::/64")
    if (kj::StringPtr(config.getSubnet()).findFirst(':') != kj::none) {
      co_return true;
    }
  }

  co_return false;
}

kj::Promise<uint16_t> ContainerClient::startEgressListener(
    kj::String listenAddress, uint16_t port) {
  auto service = kj::heap<EgressHttpService>(*this, headerTable);
  auto httpServer = kj::heap<kj::HttpServer>(timer, headerTable, *service);
  auto& httpServerRef = *httpServer;

  egressHttpServer = httpServer.attach(kj::mv(service));

  auto addr = co_await network.parseAddress(kj::str(listenAddress, ":", port));
  // The gateway IP from Docker's bridge network is not always bindable on the host.
  // On WSL with Docker Desktop, 172.17.0.1 lives inside the Docker VM, not on the WSL host's
  // interfaces. In that case, fall back to loopback — the sidecar reaches the host via
  // host-gateway (which Docker Desktop maps to the host loopback) so 127.0.0.1 works correctly.
  kj::Own<kj::ConnectionReceiver> listener;
  KJ_IF_SOME(e, kj::runCatchingExceptions([&]() { listener = addr->listen(); })) {
    KJ_LOG(WARNING, "Could not bind egress listener to gateway address, falling back to loopback",
        listenAddress, e);
    auto fallbackAddr = co_await network.parseAddress(kj::str("127.0.0.1:", port));
    listener = fallbackAddr->listen();
  }

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
  JSG_REQUIRE(response.statusCode == 200, Error, "Failed to write file ", dir, "/", filename,
      " to container [", response.statusCode, "] ", result);
}

static constexpr kj::StringPtr cloudflareCaDir = "/etc"_kj;
static constexpr kj::StringPtr cloudflareCaFilename =
    "cloudflare/certs/cloudflare-containers-ca.crt"_kj;

kj::Promise<void> ContainerClient::readCACert() {
  auto ingressPort = KJ_REQUIRE_NONNULL(
      sidecarIngressHostPort, "Cannot read CA cert: sidecar ingress port not known");

  auto response = co_await dockerApiRequest(
      network, kj::str("127.0.0.1:", ingressPort), kj::HttpMethod::GET, kj::str("/ca"));

  JSG_REQUIRE(response.statusCode == 200, Error,
      "Failed to read CA cert from sidecar: ", response.statusCode, " ", response.body);

  caCert = kj::mv(response.body);
}

kj::Promise<void> ContainerClient::injectCACert() {
  if (caCertInjected.exchange(true, std::memory_order_acquire)) {
    co_return;
  }

  bool succeeded = false;
  KJ_DEFER(if (!succeeded) caCertInjected.store(false, std::memory_order_release));

  if (caCert == kj::none) {
    co_await readCACert();
  }

  auto& cert = KJ_REQUIRE_NONNULL(caCert, "CA cert not read from sidecar yet");
  co_await writeFileToContainer(
      containerName, cloudflareCaDir, cloudflareCaFilename, cert.asBytes());

  succeeded = true;
}

kj::Promise<kj::Maybe<ContainerClient::InspectResponse>> ContainerClient::inspectContainer() {
  auto endpoint = kj::str("/containers/", containerName, "/json");

  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::mv(endpoint));
  // We check if the container with the given name exist, and if it's not,
  // we simply return false while avoiding an unnecessary error.
  if (response.statusCode == 404) {
    co_return kj::none;
  }

  JSG_REQUIRE(response.statusCode == 200, Error, "Container inspect failed");
  // Parse JSON response
  auto message = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(response.body);
  auto jsonRoot = message->getRoot<docker_api::Docker::ContainerInspectResponse>();

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

  kj::Vector<Label> labels;
  if (jsonRoot.hasConfig() && jsonRoot.getConfig().hasLabels()) {
    auto labelsJson = jsonRoot.getConfig().getLabels();
    if (labelsJson.isObject()) {
      for (auto field: labelsJson.getObject()) {
        kj::StringPtr name = field.getName();
        if (!name.startsWith(WORKERD_LABEL_PREFIX)) continue;
        auto value = field.getValue();
        JSG_REQUIRE(value.isString(), Error, "Malformed ContainerInspect label value");
        labels.add(Label{
          .name = kj::str(name.slice(WORKERD_LABEL_PREFIX.size())),
          .value = kj::str(value.getString()),
        });
      }
    }
  }

  co_return InspectResponse{.isRunning = running, .labels = labels.releaseAsArray()};
}

kj::Promise<kj::Maybe<ContainerClient::SidecarInspectResponse>> ContainerClient::inspectSidecar() {
  auto endpoint = kj::str("/containers/", sidecarContainerName, "/json");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::mv(endpoint));

  if (response.statusCode == 404) {
    co_return kj::none;
  }

  JSG_REQUIRE(response.statusCode == 200, Error, "Sidecar container inspect failed");

  auto message = decodeJsonResponse<docker_api::Docker::ContainerInspectResponse>(response.body);
  auto jsonRoot = message->getRoot<docker_api::Docker::ContainerInspectResponse>();

  // Check if sidecar is actually running
  bool running = false;
  if (jsonRoot.hasState()) {
    auto state = jsonRoot.getState();
    if (state.hasStatus()) {
      auto status = state.getStatus();
      running = status == "running" || status == "restarting";
    }
  }

  if (!running) {
    co_return kj::none;
  }

  kj::Maybe<uint16_t> ingressHostPort;

  auto ingressPortKey = kj::str(SIDECAR_INGRESS_PORT, "/tcp");
  for (auto portMapping: jsonRoot.getNetworkSettings().getPorts().getObject()) {
    if (portMapping.getName() != ingressPortKey) {
      continue;
    }

    ingressHostPort = tryParsePublishedHostPort(portMapping.getValue());
    break;
  }

  auto requiredIngressHostPort =
      KJ_REQUIRE_NONNULL(ingressHostPort, "running sidecar missing ingress host port");

  co_return SidecarInspectResponse{
    .ingressHostPort = requiredIngressHostPort,
  };
}

kj::Promise<void> ContainerClient::updateSidecarEgressPort(
    uint16_t ingressHostPort, uint16_t egressPort) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::ProxyEverything::Port>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::ProxyEverything::Port>();
  jsonRoot.setPort(egressPort);

  auto body = codec.encode(jsonRoot);
  auto response = co_await dockerApiRequest(network, kj::str("127.0.0.1:", ingressHostPort),
      kj::HttpMethod::PUT, kj::str("/egress"), kj::mv(body));

  JSG_REQUIRE(response.statusCode >= 200 && response.statusCode < 300, Error,
      "Updating sidecar egress port failed with: ", response.statusCode, " ", response.body);
}

kj::Promise<void> ContainerClient::updateSidecarEgressConfig(
    uint16_t ingressHostPort, uint16_t egressPort) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::ProxyEverything::Port>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::ProxyEverything::Port>();
  jsonRoot.setPort(egressPort);

  auto allowHostnames = getDnsAllowHostnames();
  auto dns = jsonRoot.initDns();
  auto allowHostnamesList = dns.initAllowHostnames(allowHostnames.size());
  for (auto i: kj::indices(allowHostnames)) {
    allowHostnamesList.set(i, allowHostnames[i]);
  }

  KJ_IF_SOME(enabled, internetEnabled) {
    jsonRoot.initInternet().setEnabled(enabled);
  }

  auto body = codec.encode(jsonRoot);
  auto response = co_await dockerApiRequest(network, kj::str("127.0.0.1:", ingressHostPort),
      kj::HttpMethod::PUT, kj::str("/egress"), kj::mv(body));

  JSG_REQUIRE(response.statusCode >= 200 && response.statusCode < 300, Error,
      "Updating sidecar egress config failed with: ", response.statusCode, " ", response.body);
}

kj::Promise<void> ContainerClient::createContainer(kj::StringPtr effectiveImage,
    kj::Maybe<capnp::List<capnp::Text>::Reader> entrypoint,
    kj::Maybe<capnp::List<capnp::Text>::Reader> environment,
    kj::ArrayPtr<const SnapshotRestoreMount> restoreMounts,
    rpc::Container::StartParams::Reader params) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(effectiveImage);
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

  // Pass user-supplied labels as Docker object labels, prefixed so we can distinguish
  // them from image/engine labels when reading back via inspect().
  if (params.hasLabels()) {
    auto lbls = params.getLabels();
    auto labelsObj = jsonRoot.initLabels().initObject(lbls.size());
    for (auto i: kj::zeroTo(lbls.size())) {
      labelsObj[i].setName(kj::str(WORKERD_LABEL_PREFIX, lbls[i].getName()));
      labelsObj[i].initValue().setString(lbls[i].getValue());
    }
  }

  auto hostConfig = jsonRoot.initHostConfig();
  // We need to set a restart policy to avoid having ambiguous states
  // where the container we're managing is stuck at "exited" state.
  hostConfig.initRestartPolicy().setName("on-failure");

  hostConfig.setNetworkMode(kj::str("container:", sidecarContainerName));

  // When containersPidNamespace is NOT enabled, use host PID namespace for backwards compatibility.
  // This allows the container to see processes on the host.
  if (!params.getCompatibilityFlags().getContainersPidNamespace()) {
    hostConfig.setPidMode("host");
  }

  if (restoreMounts.size() > 0) {
    auto mounts = hostConfig.initMounts(restoreMounts.size());
    for (auto i: kj::indices(restoreMounts)) {
      auto mount = mounts[i];
      auto& restoreMount = restoreMounts[i];
      mount.setType("volume");
      mount.setSource(restoreMount.cloneVolume);
      mount.setTarget(restoreMount.restorePath.toString(true));
      mount.initVolumeOptions().setNoCopy(true);
    }
  }

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/create?name=", containerName), codec.encode(jsonRoot));

  // statusCode 409 refers to "conflict". Occurs when a container with the given name exists.
  // In that case we destroy and re-create the container. We retry a few times with delays
  // because Docker may take a moment to fully release the container name after removal.
  constexpr int MAX_RETRIES = 3;
  constexpr auto RETRY_DELAY = 100 * kj::MILLISECONDS;

  for (int attempt = 0; response.statusCode == 409 && attempt < MAX_RETRIES; ++attempt) {
    co_await removeContainer(network, kj::str(dockerPath), kj::str(containerName));
    co_await timer.afterDelay(RETRY_DELAY);
    response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
        kj::str("/containers/create?name=", containerName), codec.encode(jsonRoot));
  }

  // statusCode 201 refers to "container created successfully"
  if (response.statusCode != 201) {
    JSG_REQUIRE(
        response.statusCode != 404, Error, "No such image available named ", effectiveImage);
    JSG_REQUIRE(response.statusCode != 409, Error, "Container already exists");
    JSG_FAIL_REQUIRE(
        Error, "Create container failed with [", response.statusCode, "] ", response.body);
  }
}

kj::Promise<kj::String> ContainerClient::createExec(capnp::List<capnp::Text>::Reader cmd,
    rpc::Container::ExecOptions::Reader params,
    bool attachStdout,
    bool attachStderr) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ExecCreateRequest>();

  capnp::MallocMessageBuilder message;
  auto request = message.initRoot<docker_api::Docker::ExecCreateRequest>();
  request.setAttachStdin(true);
  request.setAttachStdout(attachStdout);
  request.setAttachStderr(attachStderr);
  request.setTty(false);

  auto jsonCmd = request.initCmd(cmd.size());
  for (auto i: kj::zeroTo(cmd.size())) {
    jsonCmd.set(i, cmd[i]);
  }

  if (params.hasEnv()) {
    auto env = params.getEnv();
    auto jsonEnv = request.initEnv(env.size());
    for (auto i: kj::zeroTo(env.size())) {
      jsonEnv.set(i, env[i]);
    }
  }

  if (params.hasWorkingDirectory()) {
    request.setWorkingDir(params.getWorkingDirectory());
  }

  if (params.hasUser()) {
    request.setUser(params.getUser());
  }

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", containerName, "/exec"), codec.encode(request));
  JSG_REQUIRE(response.statusCode == 201, Error, "Creating Docker exec failed with [",
      response.statusCode, "] ", response.body);

  auto parsed = decodeJsonResponse<docker_api::Docker::ExecCreateResponse>(response.body);
  co_return kj::str(parsed->getRoot<docker_api::Docker::ExecCreateResponse>().getId());
}

kj::Promise<kj::Own<kj::AsyncIoStream>> ContainerClient::startExec(kj::String execId) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ExecStartRequest>();

  capnp::MallocMessageBuilder message;
  auto requestBody = message.initRoot<docker_api::Docker::ExecStartRequest>();
  requestBody.setDetach(false);
  requestBody.setTty(false);
  auto encodedBody = codec.encode(requestBody);

  // Exec attach uses HTTP connection hijacking. A plain POST can succeed with 200 OK but then not
  // behave like the raw stream Docker's CLI expects, so we must request the upgrade explicitly.
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  headers.setPtr(kj::HttpHeaderId::HOST, "localhost");
  headers.setPtr(kj::HttpHeaderId::CONNECTION, "Upgrade");
  // ... Why not CONNECT or WebSockets, Docker?
  headers.setPtr(kj::HttpHeaderId::UPGRADE, "tcp");
  headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "application/json");
  headers.set(kj::HttpHeaderId::CONTENT_LENGTH, kj::str(encodedBody.size()));
  kj::ArrayPtr<const kj::byte> encodedBodyBytes = encodedBody.asBytes();

  auto response = co_await dockerApiStreamedRequest(network, kj::str(dockerPath),
      kj::HttpMethod::POST, kj::str("/exec/", execId, "/start"), headers, encodedBodyBytes);
  if (response.statusCode != 101) {
    auto errorBodyBytes = co_await response.connection->readAllBytes(MAX_JSON_RESPONSE_SIZE);
    auto errorBody = kj::str(errorBodyBytes.asChars());
    JSG_FAIL_REQUIRE(Error, "Starting Docker exec failed with [", response.statusCode, "] ",
        response.statusText, " ", errorBody);
  }

  co_return kj::mv(response.connection);
}

kj::Promise<ContainerClient::ExecInspectResponse> ContainerClient::inspectExec(
    kj::StringPtr execId) {
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/exec/", execId, "/json"));
  JSG_REQUIRE(response.statusCode == 200, Error, "Inspecting Docker exec failed with [",
      response.statusCode, "] ", response.body);

  auto parsed = decodeJsonResponse<docker_api::Docker::ExecInspectResponse>(response.body);
  auto root = parsed->getRoot<docker_api::Docker::ExecInspectResponse>();
  auto exitCodeValue = root.getExitCode();
  auto exitCode = exitCodeValue.isNumber() ? static_cast<int32_t>(exitCodeValue.getNumber()) : 0;
  co_return ExecInspectResponse{
    .exitCode = exitCode,
    .running = root.getRunning(),
    .pid = root.getPid(),
  };
}

kj::Promise<void> ContainerClient::runSimpleExec(kj::ArrayPtr<const kj::String> cmd) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ExecCreateRequest>();

  capnp::MallocMessageBuilder createMessage;
  auto createRequest = createMessage.initRoot<docker_api::Docker::ExecCreateRequest>();
  createRequest.setAttachStdin(false);
  createRequest.setAttachStdout(false);
  createRequest.setAttachStderr(false);
  createRequest.setTty(false);

  auto jsonCmd = createRequest.initCmd(cmd.size());
  for (auto i: kj::indices(cmd)) {
    jsonCmd.set(i, cmd[i]);
  }

  auto createResponse =
      co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
          kj::str("/containers/", containerName, "/exec"), codec.encode(createRequest));
  JSG_REQUIRE(createResponse.statusCode == 201, Error, "Creating helper Docker exec failed with [",
      createResponse.statusCode, "] ", createResponse.body);

  auto parsedCreate =
      decodeJsonResponse<docker_api::Docker::ExecCreateResponse>(createResponse.body);
  auto execId = kj::str(parsedCreate->getRoot<docker_api::Docker::ExecCreateResponse>().getId());

  capnp::JsonCodec startCodec;
  startCodec.handleByAnnotation<docker_api::Docker::ExecStartRequest>();

  capnp::MallocMessageBuilder startMessage;
  auto startRequest = startMessage.initRoot<docker_api::Docker::ExecStartRequest>();
  startRequest.setDetach(true);
  startRequest.setTty(false);

  auto startResponse = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/exec/", execId, "/start"), startCodec.encode(startRequest));
  JSG_REQUIRE(startResponse.statusCode == 200, Error, "Starting helper Docker exec failed with [",
      startResponse.statusCode, "] ", startResponse.body);

  while (true) {
    auto inspect = co_await inspectExec(execId);
    if (!inspect.running) {
      JSG_REQUIRE(inspect.exitCode == 0, Error, "Helper Docker exec failed with exit code ",
          inspect.exitCode);
      co_return;
    }
    co_await timer.afterDelay(50 * kj::MILLISECONDS);
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
  co_await removeContainer(network, kj::str(dockerPath), kj::str(containerName));
  co_await deleteVolumes(network, kj::str(dockerPath), snapshotClones.releaseAsArray());
}

// Creates the sidecar container that owns the shared network namespace.
// The application container joins this namespace and all ingress/egress goes through it.
kj::Promise<void> ContainerClient::createSidecarContainer(
    uint16_t egressPort, kj::String networkCidr) {
  // Equivalent to: docker run --cap-add=NET_ADMIN -p <random-host>:39001 ...
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(containerEgressInterceptorImage);

  auto ipv6Enabled = co_await isDaemonIpv6Enabled();

  // determined by the number of flags we need to pass to proxy-everything
  uint32_t cmdSize =
      8;  // --http-egress-port <port> --http-ingress-address 0.0.0.0:<port> --docker-gateway-cidr <cidr> --dns-enabled --tls-intercept
  if (!ipv6Enabled) cmdSize += 1;  // --disable-ipv6

  auto cmd = jsonRoot.initCmd(cmdSize);
  uint32_t idx = 0;
  cmd.set(idx++, "--http-egress-port");
  cmd.set(idx++, kj::str(egressPort));
  cmd.set(idx++, "--http-ingress-address");
  cmd.set(idx++, kj::str("0.0.0.0:", SIDECAR_INGRESS_PORT));
  cmd.set(idx++, "--docker-gateway-cidr");
  cmd.set(idx++, networkCidr);
  cmd.set(idx++, "--dns-enabled");
  cmd.set(idx++, "--tls-intercept");
  if (!ipv6Enabled) {
    cmd.set(idx++, "--disable-ipv6");
  }

  jsonRoot.initExposedPorts().setRaw(kj::str("{\"", SIDECAR_INGRESS_PORT, "/tcp\":{}}"));

  auto hostConfig = jsonRoot.initHostConfig();
  hostConfig.setPublishAllPorts(true);
  hostConfig.setNetworkMode("bridge");
  auto dns = hostConfig.initDns(kj::size(SIDECAR_DNS_SERVERS));
  for (auto i: kj::indices(SIDECAR_DNS_SERVERS)) {
    dns.set(i, SIDECAR_DNS_SERVERS[i]);
  }

  auto extraHosts = hostConfig.initExtraHosts(1);
  extraHosts.set(0, "host.docker.internal:host-gateway"_kj);

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
    JSG_REQUIRE(response.statusCode != 404, Error, "No such image available named ",
        containerEgressInterceptorImage,
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
  co_await removeContainer(network, kj::str(dockerPath), kj::str(sidecarContainerName));
}

kj::Promise<void> ContainerClient::createVolume(kj::StringPtr volumeName) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::VolumeCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto req = message.initRoot<docker_api::Docker::VolumeCreateRequest>();
  req.setName(volumeName);
  auto labels = req.initLabels().initObject(1);
  labels[0].setName(SNAPSHOT_VOLUME_CREATED_AT_LABEL);
  labels[0].initValue().setString(currentSnapshotVolumeTimestamp());

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/volumes/create"), codec.encode(req));
  // Docker returns 201 for new volumes and 200 for existing ones.
  JSG_REQUIRE(response.statusCode == 201 || response.statusCode == 200, Error,
      "Failed to create Docker volume '", volumeName, "': ", response.statusCode, " ",
      response.body);
}

kj::Promise<void> ContainerClient::deleteVolume(kj::String volumeName) {
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::DELETE, kj::str("/volumes/", volumeName));
  // 204 = deleted, 404 = not found (both are fine)
  JSG_REQUIRE(response.statusCode == 204 || response.statusCode == 404, Error,
      "Failed to delete Docker volume '", volumeName, "': ", response.statusCode, " ",
      response.body);
}

kj::Promise<void> ContainerClient::commitContainer(kj::StringPtr imageRef) {
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/commit?container=", containerName,
          "&pause=true&repo=", kj::encodeUriComponent(imageRef)),
      kj::str(""));
  JSG_REQUIRE(response.statusCode == 201, Error, "Failed to commit container to image '", imageRef,
      "': ", response.statusCode, " ", response.body);
}

kj::Promise<ContainerClient::ImageInspectResponse> ContainerClient::inspectImage(
    kj::StringPtr imageRef) {
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::GET,
      kj::str("/images/", kj::encodeUriComponent(imageRef), "/json"));
  JSG_REQUIRE(response.statusCode == 200, Error, "Failed to inspect Docker image '", imageRef,
      "': ", response.statusCode, " ", response.body);

  auto message = decodeJsonResponse<docker_api::Docker::ImageInspectResponse>(response.body);
  auto root = message->getRoot<docker_api::Docker::ImageInspectResponse>();
  co_return ImageInspectResponse{kj::str(root.getId()), root.getSize()};
}

kj::Promise<void> ContainerClient::deleteImage(kj::String imageRef) {
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/images/", kj::encodeUriComponent(imageRef), "?noprune=true"));
  JSG_REQUIRE(response.statusCode == 200 || response.statusCode == 404, Error,
      "Failed to delete Docker image '", imageRef, "': ", response.statusCode, " ", response.body);
}

kj::Promise<kj::String> ContainerClient::createTempContainerWithVolume(
    kj::StringPtr volumeName, kj::StringPtr mountPath) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(imageName);

  auto hostConfig = jsonRoot.initHostConfig();
  auto binds = hostConfig.initBinds(1);
  binds.set(0, kj::str(volumeName, ":", mountPath));

  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/create"), codec.encode(jsonRoot));
  JSG_REQUIRE(response.statusCode == 201, Error, "Failed to create temp container for volume '",
      volumeName, "': ", response.statusCode, " ", response.body);

  auto respMessage = decodeJsonResponse<docker_api::Docker::ContainerCreateResponse>(response.body);
  auto respRoot = respMessage->getRoot<docker_api::Docker::ContainerCreateResponse>();
  co_return kj::str(respRoot.getId());
}

kj::Promise<void> ContainerClient::cloneSnapshot(SnapshotRestoreMount& snapshot) {
  co_await createVolume(snapshot.cloneVolume);

  bool cloneCommitted = false;
  KJ_DEFER(if (!cloneCommitted) {
    waitUntilTasks.add(deleteVolume(kj::str(snapshot.cloneVolume)).catch_([](kj::Exception&&) {
    }).attach(addRef()));
  });

  capnp::JsonCodec codec;
  codec.handleByAnnotation<docker_api::Docker::ContainerCreateRequest>();
  capnp::MallocMessageBuilder message;
  auto jsonRoot = message.initRoot<docker_api::Docker::ContainerCreateRequest>();
  jsonRoot.setImage(containerEgressInterceptorImage);
  jsonRoot.setEntrypoint("/bin/cp");

  // Run `/bin/cp -a /src/. /dst/` so the clone volume gets the snapshot contents directly.
  auto cmd = jsonRoot.initCmd(3);
  cmd.set(0, "-a");
  cmd.set(1, "/src/.");
  cmd.set(2, "/dst/");

  auto hostConfig = jsonRoot.initHostConfig();
  auto binds = hostConfig.initBinds(2);
  binds.set(0, kj::str(snapshot.sourceVolume, ":/src:ro"));
  binds.set(1, kj::str(snapshot.cloneVolume, ":/dst"));

  auto createResponse = co_await dockerApiRequest(network, kj::str(dockerPath),
      kj::HttpMethod::POST, kj::str("/containers/create"), codec.encode(jsonRoot));
  JSG_REQUIRE(createResponse.statusCode == 201, Error,
      "Failed to create snapshot clone helper container for volume '", snapshot.sourceVolume,
      "': ", createResponse.statusCode, " ", createResponse.body);

  auto createMessage =
      decodeJsonResponse<docker_api::Docker::ContainerCreateResponse>(createResponse.body);
  auto createRoot = createMessage->getRoot<docker_api::Docker::ContainerCreateResponse>();
  auto helperContainerId = kj::str(createRoot.getId());
  bool helperDeleted = false;
  KJ_DEFER(if (!helperDeleted) {
    waitUntilTasks.add(
        deleteTempContainer(kj::str(helperContainerId)).catch_([](kj::Exception&&) {
    }).attach(addRef()));
  });

  auto startResponse = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", helperContainerId, "/start"), kj::str(""));
  JSG_REQUIRE(startResponse.statusCode == 204, Error,
      "Failed to start snapshot clone helper container '", helperContainerId,
      "': ", startResponse.statusCode, " ", startResponse.body);

  auto waitResponse = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::POST,
      kj::str("/containers/", helperContainerId, "/wait?condition=not-running"));
  JSG_REQUIRE(waitResponse.statusCode == 200, Error,
      "Failed waiting for snapshot clone helper container '", helperContainerId,
      "': ", waitResponse.statusCode, " ", waitResponse.body);

  auto waitMessage =
      decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(waitResponse.body);
  auto waitRoot = waitMessage->getRoot<docker_api::Docker::ContainerMonitorResponse>();
  // A non-zero exit means the copy failed and the clone volume contents are incomplete.
  JSG_REQUIRE(waitRoot.getStatusCode() == 0, Error, "Snapshot clone helper container '",
      helperContainerId, "' exited with status ", waitRoot.getStatusCode());

  co_await deleteTempContainer(kj::str(helperContainerId));
  helperDeleted = true;
  cloneCommitted = true;
  snapshotClones.add(kj::str(snapshot.cloneVolume));
}

kj::Promise<void> ContainerClient::deleteTempContainer(kj::String tempContainerId) {
  auto response = co_await dockerApiRequest(network, kj::str(dockerPath), kj::HttpMethod::DELETE,
      kj::str("/containers/", tempContainerId, "?force=true"));
  // 204 = deleted, 404 = not found (both are fine).
  KJ_REQUIRE(response.statusCode == 204 || response.statusCode == 404,
      "Failed to delete temp container", tempContainerId, response.statusCode, response.body);
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

  bool isRunning = false;
  KJ_IF_SOME(info, co_await inspectContainer()) {
    isRunning = info.isRunning;
  }
  containerStarted.store(isRunning, std::memory_order_release);
  containerSidecarStarted.store(false, std::memory_order_release);
  this->sidecarIngressHostPort = kj::none;

  if (isRunning) {
    // If the sidecar container is already running (e.g. workerd restarted while
    // containers stayed up), recover its published ingress port, then configure
    // it to use our current egress listener port.
    auto sidecar = KJ_REQUIRE_NONNULL(co_await inspectSidecar(),
        "Recovered running container without a running networking sidecar");
    containerSidecarStarted.store(true, std::memory_order_release);
    this->sidecarIngressHostPort = sidecar.ingressHostPort;
    co_await ensureEgressListenerStarted();
    co_await updateSidecarEgressPort(sidecar.ingressHostPort, egressListenerPort);
    co_await readCACert();
  }

  context.getResults().setRunning(isRunning);
}

kj::Promise<void> ContainerClient::inspect(InspectContext context) {
  auto maybeResp = co_await inspectContainer();
  auto info = context.getResults().initInfo();
  KJ_IF_SOME(resp, maybeResp) {
    if (resp.isRunning) {
      auto started = info.initStarted();
      auto list = started.initLabels(resp.labels.size());
      for (auto i: kj::indices(resp.labels)) {
        list[i].setName(resp.labels[i].name);
        list[i].setValue(resp.labels[i].value);
      }
      co_return;
    }
  }
  info.setNone();
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

  kj::String effectiveImage = kj::str(imageName);
  if (params.hasContainerSnapshotId()) {
    auto snapshotId = parseSnapshotId(params.getContainerSnapshotId());
    effectiveImage = kj::str(CONTAINER_SNAPSHOT_IMAGE_PREFIX, snapshotId);
    co_await inspectImage(effectiveImage);
  }

  // If startup fails after we clone any snapshot volumes, tear down the app container first and
  // then delete those clone volumes so we don't leave mounted Docker volumes behind.
  KJ_DEFER(if (!containerStarted.load(std::memory_order_acquire)) {
    waitUntilTasks.add(destroyContainer().attach(addRef()));
  });

  kj::Vector<SnapshotRestoreMount> restoreMounts;
  if (params.hasDirectorySnapshots()) {
    auto snapshotList = params.getDirectorySnapshots();
    restoreMounts.reserve(snapshotList.size());
    for (auto i: kj::zeroTo(snapshotList.size())) {
      auto entry = snapshotList[i];
      auto snapshotId = parseSnapshotId(entry.getSnapshotId());

      auto restorePath = parseAbsolutePath(entry.getRestorePath());
      JSG_REQUIRE(restorePath.toString(true) != "/", Error,
          "Directory snapshot cannot be restored to root directory.");

      auto sourceVolume = kj::str(SNAPSHOT_VOLUME_PREFIX, snapshotId);

      auto inspectResp = co_await dockerApiRequest(
          network, kj::str(dockerPath), kj::HttpMethod::GET, kj::str("/volumes/", sourceVolume));
      JSG_REQUIRE(inspectResp.statusCode == 200, Error, "Snapshot '", snapshotId,
          "' not found (volume '", sourceVolume, "' does not exist)");

      restoreMounts.add(SnapshotRestoreMount{kj::mv(restorePath), kj::mv(sourceVolume),
        kj::str(SNAPSHOT_CLONE_VOLUME_PREFIX, randomUUID(kj::none))});
    }

    for (auto& restoreMount: restoreMounts) {
      co_await cloneSnapshot(restoreMount);
    }
  }

  co_await ensureEgressListenerStarted();
  containerSidecarStarted.store(false, std::memory_order_release);
  co_await ensureSidecarStarted();

  caCertInjected.store(false, std::memory_order_release);
  co_await createContainer(effectiveImage, entrypoint, environment, restoreMounts.asPtr(), params);

  for (auto& mapping: egressState->mappings) {
    if (mapping.protocol == EgressProtocol::HTTPS) {
      co_await injectCACert();
      break;
    }
  }

  co_await startContainer();

  containerStarted.store(true, std::memory_order_release);
}

kj::Promise<void> ContainerClient::monitor(MonitorContext context) {
  // Wait for any in-progress mutating RPCs (e.g. start()) to complete
  // before issuing the Docker wait request.
  co_await mutationQueue.addBranch();

  // If start() ran but failed (e.g. snapshot restore error), containerStarted
  // remains false. Reject immediately rather than hanging on Docker /wait for a
  // container that was never started.
  JSG_REQUIRE(containerStarted.load(std::memory_order_acquire), Error, "Container failed to start");

  auto results = context.getResults();
  KJ_DEFER(containerStarted.store(false, std::memory_order_release));

  auto endpoint = kj::str("/containers/", containerName, "/wait");
  auto response = co_await dockerApiRequest(
      network, kj::str(dockerPath), kj::HttpMethod::POST, kj::mv(endpoint));

  JSG_REQUIRE(response.statusCode == 200, Error,
      "Monitoring container failed with: ", response.statusCode, " ", response.body);

  auto message = decodeJsonResponse<docker_api::Docker::ContainerMonitorResponse>(response.body);
  auto jsonRoot = message->getRoot<docker_api::Docker::ContainerMonitorResponse>();
  results.setExitCode(jsonRoot.getStatusCode());
}

kj::Promise<void> ContainerClient::destroy(DestroyContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  this->sidecarIngressHostPort = kj::none;
  co_await destroyContainer();
  co_await destroySidecarContainer();
}

kj::Promise<void> ContainerClient::signal(SignalContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  const auto params = context.getParams();
  co_await killContainer(params.getSigno());
}

kj::Promise<void> ContainerClient::exec(ExecContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  JSG_REQUIRE(containerStarted.load(std::memory_order_acquire), Error,
      "exec() requires a running container.");

  auto request = context.getParams();
  auto execParams = request.getParams();
  // Always attach stdout/stderr to Docker so the hijacked stream lifetime continues to track the
  // process even when the JS API requested "ignore". We discard ignored output locally.
  bool attachStdout = true;
  bool attachStderr = true;

  auto execId = co_await createExec(request.getCmd(), execParams, attachStdout, attachStderr);
  kj::Own<kj::AsyncIoStream> execConnection = co_await startExec(kj::str(execId));
  kj::Maybe<capnp::ByteStream::Client> stdoutWriter = kj::none;
  if (request.hasStdoutWriter()) {
    stdoutWriter = request.getStdoutWriter();
  }

  kj::Maybe<capnp::ByteStream::Client> stderrWriter = kj::none;
  if (request.hasStderrWriter()) {
    stderrWriter = request.getStderrWriter();
  }

  // Retrying is not great, however Docker's inspectExec might return running = false
  // before it has fully spawned the process (as startExec() returns before
  // even docker has spawned the process...)
  ExecInspectResponse inspect{.exitCode = 0, .running = false, .pid = 0};
  for (auto attempt: kj::zeroTo(20)) {
    inspect = co_await inspectExec(execId);
    if (inspect.pid != 0 || !inspect.running || attempt + 1 == 20) {
      break;
    }

    co_await timer.afterDelay(50 * kj::MILLISECONDS);
  }

  auto process = context.getResults().initProcess();
  process.setPid(static_cast<int32_t>(inspect.pid));
  process.setHandle(kj::heap<DockerProcessHandle>(*this, kj::mv(execId), kj::mv(execConnection),
      kj::mv(stdoutWriter), kj::mv(stderrWriter), execParams.getCombinedOutput()));
}

kj::Promise<void> ContainerClient::setInactivityTimeout(SetInactivityTimeoutContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

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

kj::Promise<void> ContainerClient::snapshotDirectory(SnapshotDirectoryContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  const auto params = context.getParams();

  const auto dir = parseAbsolutePath(params.getDir()).toString(true);

  auto name = params.hasName() && params.getName().size() > 0
      ? kj::Maybe<kj::String>(kj::str(params.getName()))
      : kj::Maybe<kj::String>(kj::none);

  JSG_REQUIRE(containerStarted.load(std::memory_order_acquire), Error,
      "snapshotDirectory() requires a running container.");

  auto snapshotId = randomUUID(kj::none);

  // GET tar archive of the directory CONTENTS from the running container.
  // The trailing "/." tells Docker to return contents without the directory wrapper,
  // so the tar entries are relative to the directory (e.g., "hello/aaa.txt" instead
  // of "data/hello/aaa.txt"). This decouples storage from the directory name,
  // allowing restore to a different mount point.
  // Append "/." to the path to get directory contents without the directory wrapper.
  // For dir == "/", this is just "/."; for others, e.g. "/app/data" → "/app/data/.".
  auto archivePath = dir == "/" ? kj::str("/.") : kj::str(dir, "/.");
  auto tarResponse = co_await dockerApiBinaryRequest(network, kj::str(dockerPath),
      kj::HttpMethod::GET,
      kj::str("/containers/", containerName, "/archive?path=", kj::encodeUriComponent(archivePath)),
      kj::none, MAX_SNAPSHOT_TAR_SIZE);

  if (tarResponse.statusCode == 404) {
    JSG_FAIL_REQUIRE(Error, "snapshotDirectory(): directory not found in container: ", dir);
  }
  JSG_REQUIRE(tarResponse.statusCode == 200, Error,
      "snapshotDirectory(): failed to read directory '", dir,
      "' from container: ", tarResponse.statusCode);

  auto tarSize = static_cast<uint64_t>(tarResponse.body.size());

  // Create a Docker volume to store the snapshot contents. If anything after this
  // fails, clean up the volume so we don't leak Docker resources on retries.
  auto volumeName = kj::str(SNAPSHOT_VOLUME_PREFIX, snapshotId);
  co_await createVolume(volumeName);
  bool volumeCommitted = false;
  KJ_DEFER(if (!volumeCommitted) {
    waitUntilTasks.add(
        deleteVolume(kj::str(volumeName)).catch_([](kj::Exception&&) {}).attach(addRef()));
  });

  // Store the contents tar in the volume via a temp container mounted at /mnt.
  auto tempId = co_await createTempContainerWithVolume(volumeName, "/mnt");
  KJ_DEFER(waitUntilTasks.add(deleteTempContainer(kj::str(tempId)).attach(addRef())));

  auto putResponse = co_await dockerApiBinaryRequest(network, kj::str(dockerPath),
      kj::HttpMethod::PUT, kj::str("/containers/", tempId, "/archive?path=/mnt"),
      kj::mv(tarResponse.body), MAX_JSON_RESPONSE_SIZE);
  JSG_REQUIRE(putResponse.statusCode == 200, Error,
      "snapshotDirectory(): failed to store snapshot in volume '", volumeName,
      "': ", putResponse.statusCode);

  volumeCommitted = true;
  KJ_LOG(INFO, "created snapshot volume", volumeName, dir, tarSize);

  auto result = context.getResults().initSnapshot();
  result.setId(snapshotId);
  result.setSize(tarSize);
  result.setDir(dir);
  KJ_IF_SOME(n, name) {
    result.setName(n);
  }
}

kj::Promise<void> ContainerClient::snapshotContainer(SnapshotContainerContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  const auto params = context.getParams();

  JSG_REQUIRE(containerStarted.load(std::memory_order_acquire), Error,
      "snapshotContainer() requires a running container.");

  auto snapshotId = randomUUID(kj::none);
  auto imageRef = kj::str(CONTAINER_SNAPSHOT_IMAGE_PREFIX, snapshotId);
  bool imageCommitted = false;
  KJ_DEFER(if (imageCommitted) {
    waitUntilTasks.add(
        deleteImage(kj::str(imageRef)).catch_([](kj::Exception&&) {}).attach(addRef()));
  });

  co_await commitContainer(imageRef);
  imageCommitted = true;

  auto image = co_await inspectImage(imageRef);

  auto result = context.getResults().initSnapshot();
  result.setId(snapshotId);
  result.setSize(image.size);
  if (params.hasName() && params.getName().size() > 0) {
    result.setName(params.getName());
  }

  imageCommitted = false;
}

kj::Promise<void> ContainerClient::getTcpPort(GetTcpPortContext context) {
  co_await mutationQueue.addBranch();

  const auto params = context.getParams();
  uint16_t port = params.getPort();
  auto results = context.getResults();
  auto dockerPort = kj::heap<DockerPort>(*this, kj::str("127.0.0.1"), port);
  results.setPort(kj::mv(dockerPort));
  co_return;
}

kj::Promise<void> ContainerClient::listenTcp(ListenTcpContext context) {
  KJ_UNIMPLEMENTED("listenTcp not implemented for Docker containers - use port mapping instead");
}

void ContainerClient::upsertEgressMapping(EgressMapping mapping) {
  for (auto& m: egressState->mappings) {
    // If the mapping differs in port or protocol, we skip it as it's
    // not the same.
    if (m.port != mapping.port || m.protocol != mapping.protocol) {
      continue;
    }

    bool matches = false;
    KJ_SWITCH_ONEOF(m.destination) {
      KJ_CASE_ONEOF(existingCidr, kj::CidrRange) {
        KJ_IF_SOME(newCidr, mapping.destination.tryGet<kj::CidrRange>()) {
          matches = existingCidr.toString() == newCidr.toString();
        }
      }
      KJ_CASE_ONEOF(existingHostnameGlob, kj::String) {
        KJ_IF_SOME(newHostnameGlob, mapping.destination.tryGet<kj::String>()) {
          matches = existingHostnameGlob == newHostnameGlob;
        }
      }
    }

    if (matches) {
      m.channel = kj::mv(mapping.channel);
      return;
    }
  }

  egressState->mappings.add(kj::mv(mapping));
}

kj::Vector<kj::String> ContainerClient::getDnsAllowHostnames() const {
  // result N can be at most size of egressState->mappings.
  kj::Vector<kj::String> result;

  for (auto& mapping: egressState->mappings) {
    KJ_SWITCH_ONEOF(mapping.destination) {
      KJ_CASE_ONEOF(_, kj::CidrRange) {
        result.add(kj::str("*"));
        return result;
      }
      KJ_CASE_ONEOF(hostnameGlob, kj::String) {
        bool alreadyPresent = false;
        // Check if we have the hostnameGlob already present in the DNS allow
        // list.
        for (auto& existing: result) {
          if (existing == hostnameGlob) {
            alreadyPresent = true;
            break;
          }
        }

        if (!alreadyPresent) {
          result.add(kj::str(hostnameGlob));
        }
      }
    }
  }

  return result;
}

kj::Maybe<kj::Own<workerd::IoChannelFactory::SubrequestChannel>> ContainerClient::findEgressMapping(
    kj::StringPtr destAddr,
    uint16_t defaultPort,
    kj::Maybe<kj::StringPtr> hostname,
    EgressProtocol protocol) {
  auto hostAndPort = stripPort(destAddr);
  uint16_t port = hostAndPort.port.orDefault(defaultPort);
  kj::Maybe<kj::String> normalizedHostname;
  KJ_IF_SOME(hostnameValue, hostname) {
    normalizedHostname = normalizeHostname(hostnameValue);
  }

  for (auto& mapping: egressState->mappings) {
    // Mappings can differ in port, protocol and the cidr/hostname.
    // Users can specify things like google.com:7070, or 0.0.0.0:7070. On top of that,
    // they might want TLS interception (HTTPS) or raw TCP forwarding.
    if (mapping.protocol != protocol) {
      continue;
    }

    if (mapping.port != 0 && mapping.port != port) {
      continue;
    }

    KJ_SWITCH_ONEOF(mapping.destination) {
      KJ_CASE_ONEOF(cidr, kj::CidrRange) {
        if (cidr.matches(hostAndPort.host)) {
          return kj::addRef(*mapping.channel);
        }
      }
      KJ_CASE_ONEOF(hostnameGlob, kj::String) {
        KJ_IF_SOME(hostnameValue, normalizedHostname) {
          if (hostnameGlobMatches(hostnameGlob, hostnameValue)) {
            return kj::addRef(*mapping.channel);
          }
        }
      }
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

  auto sidecar = KJ_REQUIRE_NONNULL(co_await inspectSidecar(), "started sidecar not running");
  this->sidecarIngressHostPort = sidecar.ingressHostPort;

  // Wait for the sidecar's HTTP server to be ready by calling updateSidecarEgressConfig
  // in a retry loop with a per-attempt timeout.
  constexpr int MAX_READY_RETRIES = 10;
  constexpr auto READY_RETRY_DELAY = 200 * kj::MILLISECONDS;
  constexpr auto READY_ATTEMPT_TIMEOUT = 2 * kj::SECONDS;
  for (int attempt = 0;; ++attempt) {
    kj::Maybe<kj::Exception> maybeError;
    try {
      co_await timer.timeoutAfter(READY_ATTEMPT_TIMEOUT,
          updateSidecarEgressConfig(sidecar.ingressHostPort, egressListenerPort));
    } catch (...) {
      maybeError = kj::getCaughtExceptionAsKj();
    }

    if (maybeError == kj::none) break;
    if (attempt >= MAX_READY_RETRIES - 1)
      kj::throwFatalException(kj::mv(KJ_REQUIRE_NONNULL(maybeError)));
    co_await timer.afterDelay(READY_RETRY_DELAY);
  }

  co_await readCACert();
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

  co_await ensureEgressListenerStarted();

  if (containerStarted.load(std::memory_order_acquire)) {
    // Only try to create and start a sidecar container
    // if the user container is running.
    co_await ensureSidecarStarted();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  upsertEgressMapping(EgressMapping{
    .destination = kj::mv(parsed.destination),
    .port = port,
    .protocol = EgressProtocol::HTTP,
    .channel = kj::mv(subrequestChannel),
  });

  KJ_IF_SOME(ingressHostPort, sidecarIngressHostPort) {
    co_await updateSidecarEgressConfig(ingressHostPort, egressListenerPort);
  }

  co_return;
}

kj::Promise<void> ContainerClient::setEgressHttps(SetEgressHttpsContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  auto params = context.getParams();
  auto hostPortStr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();

  auto parsed = parseHostPort(hostPortStr);
  uint16_t port = parsed.port.orDefault(443);

  co_await ensureEgressListenerStarted();

  if (containerStarted.load(std::memory_order_acquire)) {
    co_await injectCACert();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  upsertEgressMapping(EgressMapping{
    .destination = kj::mv(parsed.destination),
    .port = port,
    .protocol = EgressProtocol::HTTPS,
    .channel = kj::mv(subrequestChannel),
  });

  KJ_IF_SOME(ingressHostPort, sidecarIngressHostPort) {
    co_await updateSidecarEgressConfig(ingressHostPort, egressListenerPort);
  }

  co_return;
}

kj::Promise<void> ContainerClient::setEgressTcp(SetEgressTcpContext context) {
  auto [ready, done] = getRpcTurn();
  co_await ready;
  KJ_DEFER(done->fulfill());

  auto params = context.getParams();
  auto hostPortStr = kj::str(params.getHostPort());
  auto tokenBytes = params.getChannelToken();

  auto parsed = parseHostPort(hostPortStr);
  // For TCP, default to port 0 (match all ports) when no port is specified.
  uint16_t port = parsed.port.orDefault(0);

  co_await ensureEgressListenerStarted();

  if (containerStarted.load(std::memory_order_acquire)) {
    co_await ensureSidecarStarted();
  }

  auto subrequestChannel = channelTokenHandler.decodeSubrequestChannelToken(
      workerd::IoChannelFactory::ChannelTokenUsage::RPC, tokenBytes);

  upsertEgressMapping(EgressMapping{
    .destination = kj::mv(parsed.destination),
    .port = port,
    .protocol = EgressProtocol::TCP,
    .channel = kj::mv(subrequestChannel),
  });

  KJ_IF_SOME(ingressHostPort, sidecarIngressHostPort) {
    co_await updateSidecarEgressConfig(ingressHostPort, egressListenerPort);
  }

  co_return;
}

kj::Own<ContainerClient> ContainerClient::addRef() {
  return kj::addRef(*this);
}

}  // namespace workerd::server
