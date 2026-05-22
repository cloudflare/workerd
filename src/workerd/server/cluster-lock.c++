// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cluster-lock.h"

#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/time.h>

namespace workerd {

namespace {

// The lock file contains exactly one X25519 public key (the owner). No checksum is needed: a
// single write of `sizeof(X25519PublicKey::bytes)` bytes is atomic with respect to concurrent
// readers on Linux (and likewise on NFS, where it's a single RPC to the server). The writer's
// protocol is truncate(0) → write(full key) → sync, so a reader interleaving sees either size
// 0 (treat as unowned, run claim path) or the full key.

constexpr kj::Duration INITIAL_BACKOFF = 10 * kj::MILLISECONDS;
constexpr kj::Duration MAX_BACKOFF = 1 * kj::SECONDS;

// Build a `cluster::VatId` Reader for a given public key, backed by an internal `MessageBuilder`.
class VatIdHolder {
 public:
  explicit VatIdHolder(const X25519PublicKey& key): msg(scratch) {
    auto vatId = msg.initRoot<cluster::VatId>();
    vatId.setPublicKey(kj::ArrayPtr<const byte>(key.bytes));
  }

  cluster::VatId::Reader getReader() {
    return msg.getRoot<cluster::VatId>().asReader();
  }

 private:
  capnp::word scratch[8]{};
  capnp::MallocMessageBuilder msg;
};

// Compute the next backoff delay with ±25% jitter. `attempt` starts at 0.
kj::Duration computeBackoff(uint attempt) {
  // Exponential: 10ms, 20ms, 40ms, ..., capped at 1s.
  kj::Duration base = INITIAL_BACKOFF;
  for (uint i = 0; i < attempt && base < MAX_BACKOFF; ++i) {
    base = base * 2;
  }
  if (base > MAX_BACKOFF) base = MAX_BACKOFF;

  // Jitter: multiply by a random value in [0.75, 1.25]. Use a cheap pseudo-random source.
  auto nanos =
      (kj::systemPreciseMonotonicClock().now() - kj::origin<kj::TimePoint>()) / kj::NANOSECONDS;
  uint32_t r = kj::hashCode(nanos);
  double jitter = 0.75 + (r / static_cast<double>(0xFFFFFFFFu)) * 0.5;
  return base * static_cast<int64_t>(jitter * 1000) / 1000;
}

}  // namespace

// =======================================================================================
// ClusterLockManager::OwnedLock

ClusterLockManager::OwnedLock::~OwnedLock() noexcept(false) {
  if (file.get() != nullptr) {
    // Truncate the file back to zero so the next claimant sees an empty file. We then drop the
    // OFD lock (via OfdLock's destructor) which makes the file available for other nodes to claim.
    //
    // Best-effort: don't throw from the destructor.
    KJ_TRY {
      file->truncate(0);
    }
    KJ_CATCH(e) {
      KJ_LOG(WARNING, "truncate(0) on cluster lock file failed during release", e);
    }
  }
}

// =======================================================================================
// ClusterLockManager

ClusterLockManager::ClusterLockManager(kj::Own<const kj::Directory> dir,
    ClusterRegistry& registry,
    capnp::RpcSystem<cluster::VatId>& clusterRpc,
    kj::Timer& timer)
    : dir(kj::mv(dir)),
      registry(registry),
      clusterRpc(clusterRpc),
      timer(timer) {}

kj::Promise<kj::OneOf<ClusterLockManager::OwnedLock, rpc::WorkerdDebugPort::Client>>
ClusterLockManager::acquireOrRoute(kj::StringPtr actorId) {
  // Validate that the actor ID is a hex string. This is the only form of actor ID we expect on
  // this path (durable actors named by a 64-char hex SHA-256 hash, or by `idFromName` which also
  // produces hex). Enforcing hex incidentally guarantees the ID is safe to use as a filename.
  for (char c: actorId) {
    KJ_REQUIRE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'),
        "actor ID is not a hex string", actorId);
  }
  KJ_REQUIRE(actorId.size() > 0, "actor ID is empty");

  kj::Path path({actorId});

  for (uint attempt = 0;; ++attempt) {
    if (attempt > 0) {
      co_await timer.afterDelay(computeBackoff(attempt - 1));
    }

    // Open or create the lock file.
    auto file = dir->openFile(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    // Read the owner key directly. read() returns:
    //   0                       → file is empty (freshly created, or previous owner cleanly
    //                             released)
    //   sizeof(ownerKey.bytes)  → full key present
    //   anything else           → unexpected partial state (e.g. writer crashed mid-protocol)
    //
    // Since the key has a fixed width, we can assume that if we read the entire key, it is the
    // complete and valid key. If the key is not done being written, we could get a short read
    // (unlikely, since you'd expect a 32-byte read to always be atomic, but it's not guaranteed).
    X25519PublicKey ownerKey;
    auto n = file->read(0, ownerKey.bytes);

    if (n == sizeof(ownerKey.bytes)) {
      if (!registry.isPeerDead(ownerKey)) {
        // Trust the lock file. Build a VatId and ask the RpcSystem to bootstrap.
        // The owner might be us — clusterRpc.bootstrap() returns the local bootstrap directly
        // without going through ClusterRegistry::connect() in that case.
        //
        // If the owner is actually unreachable, the bootstrap call will fail later. Internally,
        // ClusterRegistry::ConnectionImpl::unregister() runs the dead-peer cleanup probe;
        // if the peer's registry file is unlocked, the file is unlinked. Whoever retries
        // acquireOrRoute() will then see isPeerDead(ownerKey) return true and fall through to
        // the claim path.
        VatIdHolder holder(ownerKey);
        co_return clusterRpc.bootstrap(holder.getReader()).castAs<rpc::WorkerdDebugPort>();
      }

      // Owner is confirmed dead. Fall through to the claim path.
    }
    // else: n == 0 (empty file) or n is some unexpected partial size. In either case, the claim
    //       path is the right next step: if a live writer is currently producing the file, they
    //       hold the exclusive lock and our tryLock will fail; if no one holds the lock, we'll
    //       succeed and overwrite whatever was there.

    // Claim path. Try to acquire an exclusive OFD lock.
    int fd = KJ_REQUIRE_NONNULL(file->getFd(), "lock directory must be disk-backed");
    KJ_IF_SOME(lock, OfdLock::tryLock(fd, OfdLock::EXCLUSIVE)) {
      // We got the exclusive lock. Clear any stale content and write our identity.
      file->truncate(0);

      const auto& myKey = registry.getPublicKey();
      file->write(0, myKey.bytes);
      file->sync();

      co_return OwnedLock(kj::mv(file), kj::mv(lock));
    }

    // tryLock failed — someone else owns it (or is racing us to claim it). Retry.
  }
}

}  // namespace workerd
