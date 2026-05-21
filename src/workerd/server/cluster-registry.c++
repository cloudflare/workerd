// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cluster-registry.h"

#if !_WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <openssl/curve25519.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <capnp/message.h>
#include <kj/cidr.h>
#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/refcount.h>

namespace workerd {

#if __linux__

namespace {

constexpr kj::Duration REGISTRY_MAINTENANCE_INTERVAL = 60 * kj::SECONDS;
constexpr kj::Duration DIRECTORY_SCAN_INTERVAL = 30 * kj::SECONDS;
constexpr kj::Duration NEW_REGISTRY_ENTRY_GRACE_PERIOD = 15 * kj::SECONDS;
constexpr kj::Duration FAILED_PROBE_THROTTLE = 5 * kj::SECONDS;
constexpr kj::Duration SUSPECT_PEER_AVOIDANCE_PERIOD = 15 * kj::SECONDS;
constexpr kj::Duration PEER_LIVENESS_CACHE_TTL = 15 * kj::SECONDS;

}  // namespace

// =======================================================================================
// X25519PublicKey

kj::String X25519PublicKey::toHex() const {
  return kj::encodeHex(kj::arrayPtr(bytes, sizeof(bytes)));
}

X25519PublicKey X25519PublicKey::fromHex(kj::StringPtr hex) {
  KJ_REQUIRE(hex.size() == 64, "X25519 public key hex must be 64 characters", hex.size());
  auto decoded = kj::decodeHex(hex.asArray());
  KJ_REQUIRE(!decoded.hadErrors, "invalid hex in X25519 public key");
  KJ_ASSERT(decoded.size() == 32);
  X25519PublicKey result;
  memcpy(result.bytes, decoded.begin(), 32);
  return result;
}

// =======================================================================================
// ClusterRegistry::NativeAddress

void ClusterRegistry::NativeAddress::setPort(uint port) {
  KJ_REQUIRE(port <= static_cast<uint16_t>(kj::maxValue), "invalid port", port);

  switch (storage.ss_family) {
    case AF_INET: {
      KJ_REQUIRE(length == sizeof(sockaddr_in), "invalid IPv4 sockaddr length", length);
      auto& sin = *reinterpret_cast<sockaddr_in*>(&storage);
      sin.sin_port = htons(port);
      break;
    }
    case AF_INET6: {
      KJ_REQUIRE(length == sizeof(sockaddr_in6), "invalid IPv6 sockaddr length", length);
      auto& sin6 = *reinterpret_cast<sockaddr_in6*>(&storage);
      sin6.sin6_port = htons(port);
      break;
    }
    default:
      KJ_FAIL_REQUIRE("address does not have a TCP port", storage.ss_family);
  }
}

ClusterRegistry::NativeAddress ClusterRegistry::NativeAddress::fromSockaddr(const sockaddr* addr) {
  KJ_REQUIRE(addr != nullptr, "missing sockaddr");

  NativeAddress result;
  switch (addr->sa_family) {
    case AF_INET:
      result.length = sizeof(sockaddr_in);
      break;
    case AF_INET6:
      result.length = sizeof(sockaddr_in6);
      break;
    default:
      KJ_FAIL_REQUIRE("sockaddr is not an IP address", addr->sa_family);
  }

  memcpy(&result.storage, addr, result.length);
  return result;
}

ClusterRegistry::NativeAddress ClusterRegistry::NativeAddress::parse(
    kj::ArrayPtr<const byte> bytes) {
  KJ_REQUIRE(
      bytes.size() >= sizeof(sa_family_t), "peer registry address is too short", bytes.size());
  KJ_REQUIRE(
      bytes.size() <= sizeof(sockaddr_storage), "peer registry address is too long", bytes.size());

  NativeAddress result;
  result.length = bytes.size();
  memcpy(&result.storage, bytes.begin(), bytes.size());

  switch (result.storage.ss_family) {
    case AF_INET:
      KJ_REQUIRE(
          result.length == sizeof(sockaddr_in), "invalid IPv4 sockaddr length", result.length);
      break;
    case AF_INET6:
      KJ_REQUIRE(
          result.length == sizeof(sockaddr_in6), "invalid IPv6 sockaddr length", result.length);
      break;
    default:
      KJ_FAIL_REQUIRE("peer registry address is not an IP sockaddr", result.storage.ss_family);
  }

  return result;
}

// =======================================================================================
// ClusterRegistry::ConnectionImpl

class ClusterRegistry::ConnectionImpl final: public ClusterVatNetworkBase::Connection,
                                             public kj::Refcounted {
 public:
  ConnectionImpl(ClusterRegistry& registry,
      kj::Own<kj::AsyncIoStream> stream,
      kj::Maybe<X25519PublicKey> peerKey,
      kj::Maybe<kj::Own<bool>> wasRefused)
      : registry(registry),
        stream(kj::mv(stream)),
        msgStream(*this->stream, capnp::IncomingRpcMessage::getShortLivedCallback()),
        peerKey(peerKey),
        wasRefused(kj::mv(wasRefused)),
        outbound(peerKey != kj::none) {
    // We always need to pump immediately to send the handshake.
    ensurePumping();
  }

  ~ConnectionImpl() noexcept(false) {
    unregister();
  }

  cluster::VatId::Reader getPeerVatId() override {
    // We can safely expected the RPC system won't call this method until there is actually some
    // activity on the stream that causes it to be needed. If this is an inbound stream, there
    // is necessary no activity until we receive some messages, and we must have received the
    // peer key before that.
    //
    // This is convenient because otherwise we'd have to figure out what to do with this call
    // if we haven't received the handshake yet, and therefore don't actually know who the peer
    // is.
    auto pk = KJ_ASSERT_NONNULL(peerKey, "getPeerVatId() called too early");

    // Build `peerVatId` on first use.
    KJ_IF_SOME(p, peerVatId) {
      return p.getRoot<cluster::VatId>().asReader();
    } else {
      auto builder = peerVatId.emplace(peerVatIdScratch).getRoot<cluster::VatId>();
      builder.setPublicKey(kj::arrayPtr(pk.bytes, sizeof(pk.bytes)));
      return builder.asReader();
    }
  }

  kj::Own<capnp::OutgoingRpcMessage> newOutgoingMessage(uint firstSegmentWordSize) override;
  kj::Promise<kj::Maybe<kj::Own<capnp::IncomingRpcMessage>>> receiveIncomingMessage() override;
  kj::Promise<void> shutdown() override;
  void setIdle(bool newIdle) override;

 private:
  class OutgoingMessageImpl;
  class IncomingMessageImpl;

  ClusterRegistry& registry;
  kj::Own<kj::AsyncIoStream> stream;
  capnp::BufferedMessageStream msgStream;
  kj::Maybe<X25519PublicKey> peerKey;
  kj::Maybe<kj::Own<bool>> wasRefused;
  bool outbound;  // is this an outbound or inbound connection?

  capnp::word peerVatIdScratch[8]{};
  kj::Maybe<capnp::MallocMessageBuilder> peerVatId;

  kj::Vector<kj::Own<OutgoingMessageImpl>> writeQueue;
  kj::Maybe<kj::Promise<void>> pumpTask;
  kj::Maybe<kj::Exception> brokenReason;

  bool pumping = false;     // is pump() running?
  bool isShutdown = false;  // has shutdown() been called?

  bool idle = false;  // tracks last value passed to setIdle()
  bool idleTimedOut = false;

  bool sentHandshake = false;
  bool receivedHandshake = false;

  kj::Maybe<kj::Promise<void>> idleTimer;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Maybe<kj::Own<capnp::MessageReader>>>>>
      interruptReceiveFulfiller;

  void ensurePumping();
  kj::Promise<void> pump();

  void requireOpen();
  void unregister();

  friend class ClusterRegistry;
};

class ClusterRegistry::ConnectionImpl::OutgoingMessageImpl final: public capnp::OutgoingRpcMessage,
                                                                  public kj::Refcounted {
 public:
  OutgoingMessageImpl(ConnectionImpl& conn, uint firstSegmentWordSize)
      : conn(conn),
        message(firstSegmentWordSize == 0 ? capnp::SUGGESTED_FIRST_SEGMENT_WORDS
                                          : firstSegmentWordSize) {}

  capnp::AnyPointer::Builder getBody() override {
    return message.getRoot<capnp::AnyPointer>();
  }

  void send() override {
    conn.requireOpen();
    KJ_REQUIRE(!conn.idle, "bug in RpcSystem: trying to send a message while idle");
    KJ_REQUIRE(!conn.idleTimedOut, "send() after idle period timed out");

    conn.writeQueue.add(kj::addRef(*this));
    conn.ensurePumping();
  }

  size_t sizeInWords() override {
    return message.sizeInWords();
  }

 private:
  ConnectionImpl& conn;
  capnp::MallocMessageBuilder message;

  friend class ConnectionImpl;
};

class ClusterRegistry::ConnectionImpl::IncomingMessageImpl final: public capnp::IncomingRpcMessage {
 public:
  IncomingMessageImpl(kj::Own<capnp::MessageReader> message): message(kj::mv(message)) {}

  capnp::AnyPointer::Reader getBody() override {
    return message->getRoot<capnp::AnyPointer>();
  }

  size_t sizeInWords() override {
    return message->sizeInWords();
  }

 private:
  kj::Own<capnp::MessageReader> message;
};

kj::Own<capnp::OutgoingRpcMessage> ClusterRegistry::ConnectionImpl::newOutgoingMessage(
    uint firstSegmentWordSize) {
  KJ_REQUIRE(!idle, "bug in RpcSystem: trying to send a message while idle");
  return kj::refcounted<OutgoingMessageImpl>(*this, firstSegmentWordSize);
}

kj::Promise<kj::Maybe<kj::Own<capnp::IncomingRpcMessage>>> ClusterRegistry::ConnectionImpl::
    receiveIncomingMessage() {
  KJ_IF_SOME(b, brokenReason) {
    kj::throwFatalException(b.clone());
  }
  if (idleTimedOut) {
    co_return kj::none;
  }

  if (!receivedHandshake) {
    X25519PublicKey receivedKey;
    auto readHandshake = stream->read(receivedKey.bytes);
    if (outbound && registry.nfsMode()) {
      co_await registry.timer.timeoutAfter(registry.connectTimeout, kj::mv(readHandshake));
    } else {
      co_await readHandshake;
    }
    KJ_IF_SOME(expectedKey, peerKey) {
      KJ_REQUIRE(receivedKey == expectedKey, "peer did not send expected handshake bytes");
    } else {
      peerKey = receivedKey;
    }
    receivedHandshake = true;
    if (outbound) {
      KJ_IF_SOME(entry, registry.peers.find(KJ_ASSERT_NONNULL(peerKey))) {
        entry.suspect = false;
        entry.suspectSince = kj::none;
      }
    }
  }

  auto paf = kj::newPromiseAndFulfiller<kj::Maybe<kj::Own<capnp::MessageReader>>>();
  interruptReceiveFulfiller = kj::mv(paf.fulfiller);

  auto message = co_await msgStream.tryReadMessage().exclusiveJoin(kj::mv(paf.promise));

  KJ_IF_SOME(m, message) {
    co_return kj::heap<IncomingMessageImpl>(kj::mv(m));
  } else {
    co_return kj::none;
  }
}

kj::Promise<void> ClusterRegistry::ConnectionImpl::shutdown() {
  requireOpen();
  isShutdown = true;
  ensurePumping();
  return KJ_ASSERT_NONNULL(kj::mv(pumpTask));
}

void ClusterRegistry::ConnectionImpl::setIdle(bool newIdle) {
  if (newIdle == idle) return;  // Already in the desired state.
  idle = newIdle;

  // Only outbound connections should potentially close themselves when idle. Inbound should
  // be closed at the other end.
  if (outbound) {
    if (idle) {
      // Wait for the idle timeout.
      idleTimer = registry.timer.afterDelay(registry.idleTimeout)
                      .then([this]() {
        // Wait until the event queue is empty, just in case we're concurrently receiving a
        // message that causes us to become non-idle. (But that shouldn't happen since this is
        // an outbound connection. Once idle, it should only be revived from our end. But just to
        // be safe.)
        return kj::evalLast([this]() {
          // We officially timed out. Make sure all future calls to receiveIncomingMessage() return
          // kj::none.
          idleTimedOut = true;

          // Cancel the current receiveIncomingMessage() by fulfilling the promise that is
          // joined with the `tryReadMessage()` promise.
          KJ_IF_SOME(i, interruptReceiveFulfiller) {
            i->fulfill(kj::none);
          }

          // Make sure a new call to connect() can't return this again.
          unregister();
        });
      }).eagerlyEvaluate(nullptr);
    } else {
      // Cancel the last idle wait.
      idleTimer = kj::none;
    }
  }
}

void ClusterRegistry::ConnectionImpl::ensurePumping() {
  if (!pumping) {
    pumping = true;
    pumpTask = pump();
  }
}

kj::Promise<void> ClusterRegistry::ConnectionImpl::pump() {
  KJ_TRY {
    if (!sentHandshake) {
      co_await stream->write(registry.publicKey.bytes);
      sentHandshake = true;
    }

    // Give a chance for messages to accumulate to be sent all at once.
    co_await kj::yieldUntilQueueEmpty();

    while (!writeQueue.empty()) {
      // Take ownership of all messages currently pending. `writeQueue` is left empty, but may
      // be repopulated concurrently.
      auto ownMessages = kj::mv(writeQueue);

      // Write them all at once.
      auto messageSegments =
          KJ_MAP(msg, ownMessages) { return msg->message.getSegmentsForOutput(); };
      co_await msgStream.writeMessages(messageSegments);
    }

    if (isShutdown) {
      co_await msgStream.end();
    }

    pumping = false;
  }
  KJ_CATCH(exception) {
    brokenReason = exception.clone();
    KJ_IF_SOME(i, interruptReceiveFulfiller) {
      i->reject(exception.clone());
    }
    kj::throwFatalException(kj::mv(exception));
  }
}

void ClusterRegistry::ConnectionImpl::requireOpen() {
  KJ_IF_SOME(e, brokenReason) {
    kj::throwFatalException(e.clone());
  }
  KJ_REQUIRE(!isShutdown, "bug in RpcSystem: can't send() after shutdown()");
}

void ClusterRegistry::ConnectionImpl::unregister() {
  if (outbound) {
    auto key = KJ_ASSERT_NONNULL(peerKey);  // peerKey is always set for outbounds

    KJ_TRY {
      KJ_IF_SOME(entry, registry.peers.findEntry(key)) {
        KJ_IF_SOME(conn, entry.value.outboundConnection) {
          if (&conn == this) {
            entry.value.outboundConnection = kj::none;
          }
        }

        // If we failed to receive the peer handshake, mark the peer as suspect and run the
        // dead-peer cleanup path. We may have failed to connect entirely, or we may have
        // connected to a server that wasn't the peer we expected.
        if (!receivedHandshake) {
          bool wasRefusedUnix = false;
          KJ_IF_SOME(w, wasRefused) {
            wasRefusedUnix = *w;
          }
          registry.cleanupFailedPeer(entry, wasRefusedUnix);
        }
      }
    }
    KJ_CATCH(exception) {
      KJ_LOG(WARNING, "failed to clean up cluster peer after connection failure", key.toHex(),
          exception);
    }
  }
}

// =======================================================================================
// ClusterRegistry constructor

ClusterRegistry::ClusterRegistry(kj::Own<const kj::Directory> registryDirParam,
    kj::StringPtr networkConfig,
    kj::Network& kjNetwork,
    kj::Timer& timer,
    kj::Duration idleTimeout,
    kj::Duration connectTimeout)
    : registryDir(kj::mv(registryDirParam)),
      dirFd(KJ_REQUIRE_NONNULL(
          registryDir->getFd(), "cluster registry directory must be a disk-backed directory")),
      timer(timer),
      network(kjNetwork),
      idleTimeout(idleTimeout),
      connectTimeout(connectTimeout) {
  // Generate X25519 keypair.
  X25519_keypair(publicKey.bytes, privateKey);
  publicKeyHex = publicKey.toHex();

  if (networkConfig == "unix") {
    // Unix mode: bind a Unix domain socket at <registryDir>/<publicKeyHex>.
    // Use /proc/self/fd/<dirFd>/<publicKeyHex> as the socket path.
    auto socketPath = kj::str("/proc/self/fd/", dirFd, "/", publicKeyHex);
    sockaddr_un addr{};
    KJ_REQUIRE(
        socketPath.size() < sizeof(addr.sun_path), "Unix socket path is too long", socketPath);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socketPath.begin(), socketPath.size());
    listener = kjNetwork.getSockaddr(&addr, sizeof(addr))->listen();
  } else {
    // CIDR/IP/NFS mode: find a matching local IP and bind an ephemeral port.
    auto addr = findMatchingIp(networkConfig);
    listener = kjNetwork.getSockaddr(addr.asSockaddr(), addr.length)->listen();
    auto port = listener->getPort();
    addr.setPort(port);
    boundAddress = addr;
  }
}

ClusterRegistry::~ClusterRegistry() noexcept(false) {
  registryDir->tryRemove(kj::Path({publicKeyHex}));
}

// Search all IP addresses for all interfaces available in the network namespace to find one that
// matches the given CIDR.
ClusterRegistry::NativeAddress ClusterRegistry::findMatchingIp(kj::StringPtr cidr) {
  kj::CidrRange range(cidr);

  struct ifaddrs* ifap = nullptr;
  KJ_SYSCALL(getifaddrs(&ifap));
  KJ_DEFER(freeifaddrs(ifap));

  for (auto* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if (range.matches(ifa->ifa_addr)) {
      auto result = NativeAddress::fromSockaddr(ifa->ifa_addr);
      result.setPort(0);
      return result;
    }
  }

  KJ_FAIL_REQUIRE("no local interface matches CIDR", cidr);
}

// =======================================================================================
// Registry maintenance

kj::Promise<void> ClusterRegistry::runMaintenance() {
  KJ_IF_SOME(addr, boundAddress) {
    // CIDR/IP mode: create the registry file, lock it, write our address, then periodically
    // verify that our registry entry and NFS lock lease are still valid.

    auto registryFile = registryDir->openFile(kj::Path({publicKeyHex}), kj::WriteMode::CREATE);

    // Acquire an exclusive OFD lock.
    auto lock = KJ_ASSERT_NONNULL(
        OfdLock::tryLock(KJ_ASSERT_NONNULL(registryFile->getFd()), OfdLock::EXCLUSIVE),
        "our registry entry is already locked?");

    // Write our address.
    registryFile->write(0, addr.asBytes());
    registryFile->sync();

    return maintenanceLoopCanceler.wrap(maintenanceLoop(registration.emplace(Registration{
      .file = kj::mv(registryFile),
      .lock = kj::mv(lock),
    })));
  } else {
    // Unix socket mode: the listener socket's lifetime IS the registry entry. Nothing to maintain.
    return kj::NEVER_DONE;
  }
}

kj::Promise<void> ClusterRegistry::maintenanceLoop(Registration& registration) {
  for (;;) {
    co_await timer.afterDelay(REGISTRY_MAINTENANCE_INTERVAL);

    // Verify nlink > 0 (not unlinked by another node).
    KJ_REQUIRE(registration.file->stat().linkCount > 0,
        "registry file was unlinked by another node; this instance should shut down");

    // Verify the OFD lock is still held (NFSv4 lease check).
    registration.lock.verifyHeld();
  }
}

void ClusterRegistry::verifyNfsLease() {
  KJ_IF_SOME(r, registration) {
    KJ_TRY {
      r.lock.verifyHeld();
    }
    KJ_CATCH(exception) {
      // Might as well cancel the maintenance loop immediately.
      maintenanceLoopCanceler.cancel(exception);
      kj::throwFatalException(kj::mv(exception));
    }
  }
}

// =======================================================================================
// Peer discovery

void ClusterRegistry::scanDirectory() {
  // List the registry to discover new peers and add them to the `peers` map, so that they can
  // be considered by `pickRandomPeer()`. Keep in mind that on NFS, directory listings tend to be
  // cached, and therefore we can's assume the existence or absence of a file really tells us
  // anything about whether the peer is currently alive or dead. The best we can do is populate the
  // map entries so that we know that peers exist at all, and then we'll need to probe them for
  // liveness as usual later on.

  auto names = registryDir->listNames();

  for (auto& name: names) {
    if (name.size() != 64) continue;
    if (name == publicKeyHex) continue;

    auto decoded = kj::decodeHex(name.asArray());
    if (decoded.hadErrors || decoded.size() != 32) continue;

    X25519PublicKey key;
    memcpy(key.bytes, decoded.begin(), 32);

    peers.findOrCreate(key, [&]() -> decltype(peers)::Entry { return {.key = key}; });
  }
}

void ClusterRegistry::scanDirectoryIfStale(kj::TimePoint now) {
  KJ_IF_SOME(lastScan, lastDirectoryScan) {
    if (now - lastScan < DIRECTORY_SCAN_INTERVAL) return;
  }

  scanDirectory();
  lastDirectoryScan = now;
}

bool ClusterRegistry::isPeerDead(const X25519PublicKey& key) {
  if (key == publicKey) return false;

  auto& entry =
      peers.findOrCreateEntry(key, [&]() -> decltype(peers)::Entry { return {.key = key}; });

  if (entry.value.knownDead) return true;

  auto now = timer.now();
  KJ_IF_SOME(lastLive, entry.value.lastConfirmedLiveTime) {
    if (now - lastLive < PEER_LIVENESS_CACHE_TTL) return false;
  }

  if (registryDir->exists(kj::Path({key.toHex()}))) {
    entry.value.lastConfirmedLiveTime = now;
    return false;
  }

  entry.value.knownDead = true;
  return true;
}

kj::Maybe<X25519PublicKey> ClusterRegistry::pickRandomPeer() {
  kj::Vector<X25519PublicKey> candidates;
  auto now = timer.now();
  scanDirectoryIfStale(now);

  for (auto& peer: peers) {
    if (peer.key == publicKey) continue;
    if (peer.value.knownDead) continue;
    if (peer.value.suspect) {
      KJ_IF_SOME(since, peer.value.suspectSince) {
        if (now - since < SUSPECT_PEER_AVOIDANCE_PERIOD) continue;
      }
    }
    candidates.add(peer.key);
  }

  if (candidates.empty()) return kj::none;

  // Simple pseudo-random selection using the monotonic clock as a cheap source. (Don't use
  // timer.now() since it only updates when the event loop waits for I/O.)
  auto nanos =
      (kj::systemPreciseMonotonicClock().now() - kj::origin<kj::TimePoint>()) / kj::NANOSECONDS;
  auto idx = kj::hashCode(nanos) % candidates.size();
  return candidates[idx];
}

void ClusterRegistry::cleanupFailedPeer(decltype(peers)::Entry& entry, bool wasRefusedUnix) {
  auto now = timer.now();

  entry.value.suspect = true;
  entry.value.suspectSince = now;

  auto path = kj::Path({entry.key.toHex()});

  if (!nfsMode()) {
    if (wasRefusedUnix) {
      // We failed with ECONNREFUSED, which means the listener is gone (process exited or closed
      // the listen socket), in which case we can certainly clean up the socket from disk.
      registryDir->tryRemove(path);
      entry.value.knownDead = true;
    } else {
      // We failed for some other reason. Check if the file has disappeared from disk.
      if (registryDir->exists(path)) {
        // File still exists. We may have failed with EAGAIN (the peer's listen queue is full) or
        // some other error where the peer is not necessarily gone. We've already marked it
        // suspect, but we should not actually erase it.
      } else {
        // File is gone from disk. This may be exactly why the connection failed, but regardless,
        // we should now mark the peer dead in our cache.
        entry.value.knownDead = true;
      }
    }

    return;
  }

  KJ_IF_SOME(lastProbe, entry.value.lastFailedProbeTime) {
    if (now - lastProbe < FAILED_PROBE_THROTTLE) return;
  }
  entry.value.lastFailedProbeTime = now;

  KJ_IF_SOME(file, registryDir->tryOpenFile(path)) {
    auto meta = file->stat();
    auto calendarNow = kj::systemCoarseCalendarClock().now();
    if (calendarNow - meta.lastModified < NEW_REGISTRY_ENTRY_GRACE_PERIOD) return;

    if (OfdLock::tryLock(KJ_ASSERT_NONNULL(file->getFd()), OfdLock::SHARED) != kj::none) {
      registryDir->tryRemove(path);
      entry.value.knownDead = true;
    }
  } else {
    entry.value.knownDead = true;
  }
}

// =======================================================================================
// Peer address lookup

ClusterRegistry::NativeAddress ClusterRegistry::readPeerAddress(const X25519PublicKey& key) {
  auto path = kj::Path({key.toHex()});
  auto file =
      KJ_REQUIRE_NONNULL(registryDir->tryOpenFile(path), "peer not found in registry", key.toHex());
  auto content = file->readAllBytes();
  KJ_REQUIRE(content.size() > 0, "peer registry file is empty", key.toHex());
  return NativeAddress::parse(content);
}

// =======================================================================================
// VatNetwork: connect()

kj::Maybe<kj::Own<ClusterVatNetworkBase::Connection>> ClusterRegistry::connect(
    cluster::VatId::Reader peer) {
  auto keyData = peer.getPublicKey();
  KJ_REQUIRE(keyData.size() == 32, "invalid VatId: publicKey must be 32 bytes");

  X25519PublicKey peerKey;
  memcpy(peerKey.bytes, keyData.begin(), 32);

  // Self-connect returns kj::none per VatNetwork contract.
  if (peerKey == publicKey) {
    return kj::none;
  }

  // Check for cached outbound connection.
  KJ_IF_SOME(entry, peers.find(peerKey)) {
    KJ_IF_SOME(conn, entry.outboundConnection) {
      return kj::addRef(conn);
    }
  }

  auto& entry = peers.findOrCreateEntry(
      peerKey, [&]() -> decltype(peers)::Entry { return {.key = peerKey}; });

  KJ_REQUIRE(!entry.value.knownDead, "peer is known to be dead", peerKey.toHex());

  // Need to establish a new connection. Look up the peer's address.
  kj::Own<kj::NetworkAddress> networkAddress;
  if (nfsMode()) {
    NativeAddress address;
    try {
      address = readPeerAddress(peerKey);
    } catch (...) {
      cleanupFailedPeer(entry, false);
      throw;
    }
    networkAddress = network.getSockaddr(address.asSockaddr(), address.length);
  } else {
    auto path = kj::str("/proc/self/fd/", dirFd, "/", peerKey.toHex());
    sockaddr_un addr{};
    KJ_REQUIRE(path.size() < sizeof(addr.sun_path), "Unix socket path is too long", path);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.begin(), path.size());
    networkAddress = network.getSockaddr(&addr, sizeof(addr));
  }

  auto connectPromise = networkAddress->connect();

  kj::Maybe<kj::Own<bool>> wasRefused;
  if (!nfsMode()) {
    auto wrapper = kj::refcountedWrapper<bool>(false);
    wasRefused = wrapper->addWrappedRef();
    connectPromise = connectPromise.catch_(
        [wrapper = kj::mv(wrapper)](
            kj::Exception&& e) mutable -> kj::Promise<kj::Own<kj::AsyncIoStream>> {
      if (e.getType() == kj::Exception::Type::DISCONNECTED) {
        // If connect() failed with DISCONNECTED on a unix socket, this likely means we got
        // ECONNREFUSED. Record this.
        wrapper->getWrapped() = true;
      }
      kj::throwFatalException(kj::mv(e));
    });
  }

  auto promisedStream = kj::newPromisedStream(connectPromise.attach(kj::mv(networkAddress)));

  auto conn =
      kj::refcounted<ConnectionImpl>(*this, kj::mv(promisedStream), peerKey, kj::mv(wasRefused));

  // Cache the outbound connection.
  entry.value.outboundConnection = *conn;

  return kj::Own<Connection>(kj::addRef(*conn));
}

// =======================================================================================
// VatNetwork: accept()

kj::Promise<kj::Own<ClusterVatNetworkBase::Connection>> ClusterRegistry::accept() {
  auto stream = co_await listener->accept();
  co_return kj::refcounted<ConnectionImpl>(*this, kj::mv(stream), kj::none, kj::none);
}

// =======================================================================================

#else  // #if __linux__

ClusterRegistry::ClusterRegistry(kj::Own<const kj::Directory> registryDir,
    kj::StringPtr network,
    kj::Network& kjNetwork,
    kj::Timer& timer,
    kj::Duration idleTimeout,
    kj::Duration connectTimeout) {
  KJ_UNIMPLEMENTED("cluster mode is only implemented on linux");
}
ClusterRegistry::~ClusterRegistry() noexcept(false) {}

kj::Promise<void> ClusterRegistry::runMaintenance() {
  KJ_UNREACHABLE;
}
void ClusterRegistry::verifyNfsLease() {
  KJ_UNREACHABLE;
}
bool ClusterRegistry::isPeerDead(const X25519PublicKey& key) {
  KJ_UNREACHABLE;
}
kj::Maybe<X25519PublicKey> ClusterRegistry::pickRandomPeer() {
  KJ_UNREACHABLE;
}
kj::Maybe<kj::Own<ClusterVatNetworkBase::Connection>> ClusterRegistry::connect(
    cluster::VatId::Reader peer) {
  KJ_UNREACHABLE;
}
kj::Promise<kj::Own<ClusterVatNetworkBase::Connection>> ClusterRegistry::accept() {
  KJ_UNREACHABLE;
}

#endif  // #if __linux__, #else

}  // namespace workerd
