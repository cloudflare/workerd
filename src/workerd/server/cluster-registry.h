// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/server/cluster.capnp.h>
#include <workerd/util/ofd-lock.h>

#if __linux__
#include <sys/socket.h>
#endif

#include <capnp/rpc.h>
#include <capnp/serialize-async.h>
#include <kj/async-io.h>
#include <kj/filesystem.h>
#include <kj/hash.h>
#include <kj/map.h>
#include <kj/timer.h>

namespace workerd {

using kj::byte;
using kj::uint;

struct X25519PublicKey {
  kj::byte bytes[32];

  kj::String toHex() const;
  static X25519PublicKey fromHex(kj::StringPtr hex);

  bool operator==(const X25519PublicKey& other) const {
    return memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
  }
  kj::uint hashCode() const {
    return kj::hashCode(kj::ArrayPtr<const byte>(bytes));
  }
};

// Type alias for the VatNetwork base that ClusterRegistry implements.
using ClusterVatNetworkBase = capnp::VatNetwork<cluster::VatId,
    cluster::ThirdPartyCompletion,
    cluster::ThirdPartyToAwait,
    cluster::ThirdPartyToContact,
    cluster::JoinResult>;

class ClusterRegistry final: public ClusterVatNetworkBase {
  // Manages this node's cluster identity, registry presence, peer discovery, and the
  // VatNetwork that the cluster's RpcSystem sits on top of.
 public:
  ClusterRegistry(kj::Own<const kj::Directory> registryDir,
      kj::StringPtr network,
      kj::Network& kjNetwork,
      kj::Timer& timer,
      kj::Duration idleTimeout = 60 * kj::SECONDS,
      kj::Duration connectTimeout = 1 * kj::SECONDS);
  ~ClusterRegistry() noexcept(false);

  const X25519PublicKey& getPublicKey() {
    return publicKey;
  }

  // Run the registry maintenance loop. Returns a promise that must be held alive for
  // the lifetime of the server.
  kj::Promise<void> runMaintenance();

  // Verify that the NFS lease is still valid on the registration file, or throw an exception if
  // not (no-op if not in NFS mode).
  //
  // More specifically: If you call this immediately after open()ing a file, and it returns
  // successfully, then the newly-opened file is guaranteed to be on the same lease as the
  // registration. (Technically, the lease could have been lost already, but if so, the
  // newly-opened file will not be writable.)
  void verifyNfsLease();

  // Definitive proof-of-death query. Returns true only when the peer's registry entry is absent.
  bool isPeerDead(const X25519PublicKey& key);

  // Pick a random peer's key from the cached peer list, biased away from recent failures.
  kj::Maybe<X25519PublicKey> pickRandomPeer();

  // Returns true if this registry uses IP sockets (not unix sockets) and is therefore in
  // "NFS mode" where it tries to be NFS-friendly.
  bool nfsMode() {
    return boundAddress != kj::none;
  }

  // implements VatNetwork -----------------------------------------------------
  kj::Maybe<kj::Own<Connection>> connect(cluster::VatId::Reader peer) override;
  kj::Promise<kj::Own<Connection>> accept() override;

 private:
#if __linux__
  class ConnectionImpl;

  struct NativeAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;

    const sockaddr* asSockaddr() const {
      return reinterpret_cast<const sockaddr*>(&storage);
    }

    kj::ArrayPtr<const byte> asBytes() const {
      return kj::arrayPtr(reinterpret_cast<const byte*>(&storage), length);
    }

    void setPort(uint port);

    static NativeAddress fromSockaddr(const sockaddr* addr);
    static NativeAddress parse(kj::ArrayPtr<const byte> bytes);
  };

  kj::Own<const kj::Directory> registryDir;
  int dirFd;  // raw fd of registryDir, for POSIX ops
  kj::Timer& timer;
  kj::Network& network;
  kj::Duration idleTimeout;
  kj::Duration connectTimeout;

  // Identity
  kj::byte privateKey[32];
  X25519PublicKey publicKey;
  kj::String publicKeyHex;

  // Address we wrote to the registry file (CIDR/IP/NFS mode). Null in Unix socket mode.
  kj::Maybe<NativeAddress> boundAddress;

  struct Registration {
    kj::Own<const kj::File> file;
    OfdLock lock;
  };
  kj::Maybe<Registration> registration;  // only in NFS mode

  kj::Canceler maintenanceLoopCanceler;

  // Listener
  kj::Own<kj::ConnectionReceiver> listener;

  // Peer cache
  struct PeerInfo {
    kj::Maybe<ConnectionImpl&> outboundConnection;
    kj::Maybe<kj::TimePoint> lastConfirmedLiveTime;
    kj::Maybe<kj::TimePoint> lastFailedProbeTime;
    bool suspect = false;
    kj::Maybe<kj::TimePoint> suspectSince;
    bool knownDead = false;
  };
  kj::HashMap<X25519PublicKey, PeerInfo> peers;
  kj::Maybe<kj::TimePoint> lastDirectoryScan;

  void scanDirectory();
  void scanDirectoryIfStale(kj::TimePoint now);
  kj::Promise<void> maintenanceLoop(Registration& registration);
  void cleanupFailedPeer(decltype(peers)::Entry& entry, bool wasRefusedUnix);

  NativeAddress findMatchingIp(kj::StringPtr cidr);

  // Read the address for a peer from its registry file. CIDR/IP mode only.
  NativeAddress readPeerAddress(const X25519PublicKey& key);

#else  // #if __linux__
  // Just so inline methods compile
  X25519PublicKey publicKey;
  kj::Maybe<int> boundAddress;

#endif  // #if __linux__, #else
};

}  // namespace workerd
