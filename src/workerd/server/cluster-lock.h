// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/worker-interface.capnp.h>
#include <workerd/server/cluster-registry.h>
#include <workerd/server/cluster.capnp.h>
#include <workerd/util/ofd-lock.h>

#include <capnp/rpc.h>
#include <kj/async.h>
#include <kj/filesystem.h>
#include <kj/one-of.h>
#include <kj/timer.h>

namespace workerd {

// Manages a directory of lock files for DO ownership. Each lock file is named after the actor ID
// and contains the owning node's public key (or is empty if unowned).
//
// The ownership protocol:
//   1. Open/create the lock file.
//   2. Read it. If it holds a full key, that's the owner.
//      - If it's another node and !registry.isPeerDead(ownerKey): route via
//        clusterRpc.bootstrap(vatIdFor(ownerKey)).
//      - If it's another node and registry.isPeerDead(ownerKey): fall through to the claim path.
//      - If it's this node: the file alone is not authoritative (a failed claim or release can
//        leave our key behind). Fall through to the claim path; if tryLock() is refused, an
//        OwnedLock on this node holds the actor and we route to ourselves.
//   3. Claim path: tryLock(EXCLUSIVE). If granted, ftruncate, write our key, fsync,
//      return OwnedLock. If refused, retry with backoff.
class ClusterLockManager {
 public:
  ClusterLockManager(kj::Own<const kj::Directory> dir,
      ClusterRegistry& registry,
      capnp::RpcSystem<cluster::VatId>& clusterRpc,
      kj::Timer& timer);

  // RAII ownership handle. Holding this object means this node owns the DO: it holds the
  // exclusive OFD lock on the lock file. Destruction truncates the lock file (best-effort) and
  // releases the OFD lock, making the DO available for other nodes to claim.
  class OwnedLock {
   public:
    ~OwnedLock() noexcept(false);
    OwnedLock(OwnedLock&&) = default;
    OwnedLock& operator=(OwnedLock&&) = default;
    KJ_DISALLOW_COPY(OwnedLock);

   private:
    OwnedLock(kj::Own<const kj::File> file, OfdLock lock): file(kj::mv(file)), lock(kj::mv(lock)) {}

    kj::Own<const kj::File> file;
    OfdLock lock;
    friend class ClusterLockManager;
  };

  // The core routing operation. Either acquires local ownership or returns a capability for
  // reaching the current owner.
  //
  // Encapsulates the full protocol including retries with backoff, registry liveness
  // cross-referencing, and capability resolution (with retry on stale owners).
  //
  // Returns:
  //   OwnedLock — this node has newly acquired ownership (caller should start the DO).
  //   rpc::WorkerdClusterPort::Client — the DO has an existing owner. The caller sends the
  //       actor's channel token to this to reach the actor. The owner might be this node, in
  //       which case the RpcSystem returns the local self-bootstrap directly (no wire traffic).
  //       The caller does not need to distinguish local vs remote.
  kj::Promise<kj::OneOf<OwnedLock, rpc::WorkerdClusterPort::Client>> acquireOrRoute(
      kj::StringPtr actorId);

 private:
  kj::Own<const kj::Directory> dir;
  ClusterRegistry& registry;
  capnp::RpcSystem<cluster::VatId>& clusterRpc;
  kj::Timer& timer;
};

}  // namespace workerd
