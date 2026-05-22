// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cluster-lock.h"
#include "cluster-registry.h"

#include <workerd/io/worker-interface.capnp.h>

#include <stdlib.h>

#include <capnp/capability.h>
#include <capnp/rpc.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd {
namespace {

#if __linux__

// Helper to create a temp directory on disk with a `registry/` and `locks/` subdir, mirroring
// the production layout (the cluster registry directory and the per-namespace locks directory
// are always separate).
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/cluster-lock-test-XXXXXX";
    KJ_ASSERT(mkdtemp(tmpl) != nullptr);
    pathStr = kj::str(tmpl);
    disk = kj::newDiskFilesystem();
    root = disk->getRoot().openSubdir(kj::Path::parse(pathStr.slice(1)),  // remove leading /
        kj::WriteMode::MODIFY);
    locksDir = root->openSubdir(kj::Path({"locks"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  }

  ~TempDir() noexcept(false) {
    auto p = kj::Path::parse(pathStr.slice(1));
    disk->getRoot().remove(p);
  }

  kj::Own<const kj::Directory> registry() {
    return root->openSubdir(kj::Path({"registry"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  }

  kj::Own<const kj::Directory> locks() {
    return locksDir->clone();
  }

  // Read the content of a lock file directly (for asserting that the file is in the expected state).
  kj::Array<kj::byte> readLockFile(kj::StringPtr name) {
    auto file = KJ_ASSERT_NONNULL(locksDir->tryOpenFile(kj::Path({name})));
    return file->readAllBytes();
  }

  // Write raw bytes to a lock file (for setting up test scenarios).
  void writeLockFile(kj::StringPtr name, kj::ArrayPtr<const kj::byte> data) {
    auto file = locksDir->openFile(kj::Path({name}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
    file->writeAll(data);
  }

 private:
  kj::String pathStr;
  kj::Own<kj::Filesystem> disk;
  kj::Own<const kj::Directory> root;
  kj::Own<const kj::Directory> locksDir;
};

// Stub WorkerdDebugPort server used as a per-node bootstrap. We don't actually invoke methods on
// it from these tests; we just need a capability to hand to RpcSystem.
class StubDebugPort final: public rpc::WorkerdDebugPort::Server {};

KJ_TEST("ClusterLockManager: unowned (empty file) -> acquire succeeds") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  auto result = lockManager.acquireOrRoute("a1").wait(io.waitScope);
  KJ_ASSERT(result.is<ClusterLockManager::OwnedLock>());

  // The lock file should now contain our key.
  auto content = tmpDir.readLockFile("a1");
  KJ_ASSERT(content.size() == 32);
  KJ_EXPECT(memcmp(content.begin(), reg.getPublicKey().bytes, 32) == 0);
}

KJ_TEST("ClusterLockManager: owned by self -> returns self-bootstrap, no wire traffic") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  // First acquire to populate the lock file with our own key.
  auto ownedResult = lockManager.acquireOrRoute("a2").wait(io.waitScope);
  KJ_ASSERT(ownedResult.is<ClusterLockManager::OwnedLock>());

  // Now simulate someone else also calling acquireOrRoute on the same actor (i.e. a forwarding
  // path). With the OwnedLock still held, the lock file is non-empty and our key is the owner.
  // acquireOrRoute should return a bootstrap client (routing to ourselves).
  auto routeResult = lockManager.acquireOrRoute("a2").wait(io.waitScope);
  KJ_ASSERT(routeResult.is<rpc::WorkerdDebugPort::Client>());
}

KJ_TEST("ClusterLockManager: stale owner (peer dead) -> claim path runs") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  // Manually create a lock file naming a non-existent peer as owner. Since `deadKey`'s registry
  // file doesn't exist in tmpDir, isPeerDead(deadKey) returns true and the claim path should run.
  X25519PublicKey deadKey;
  memset(deadKey.bytes, 0xAB, sizeof(deadKey.bytes));

  tmpDir.writeLockFile("a3", kj::arrayPtr(deadKey.bytes, 32));

  // Confirm isPeerDead returns true for the dead key.
  KJ_EXPECT(reg.isPeerDead(deadKey));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  // acquireOrRoute should run the claim path and succeed.
  auto result = lockManager.acquireOrRoute("a3").wait(io.waitScope);
  KJ_ASSERT(result.is<ClusterLockManager::OwnedLock>());

  // The lock file should now contain *our* key.
  auto content = tmpDir.readLockFile("a3");
  KJ_ASSERT(content.size() == 32);
  KJ_EXPECT(memcmp(content.begin(), reg.getPublicKey().bytes, 32) == 0);
}

KJ_TEST("ClusterLockManager: wrong-sized stale content + writer dead -> claim immediately") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  // Write a lock file of unexpected size (e.g. a crashed writer left a half-written file).
  // Nobody holds the lock, so the claim path should succeed.
  kj::byte garbage[10];
  memset(garbage, 0xFF, sizeof(garbage));
  tmpDir.writeLockFile("a4", kj::arrayPtr(garbage, sizeof(garbage)));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  auto result = lockManager.acquireOrRoute("a4").wait(io.waitScope);
  KJ_ASSERT(result.is<ClusterLockManager::OwnedLock>());

  // The lock file should now contain our key.
  auto content = tmpDir.readLockFile("a4");
  KJ_ASSERT(content.size() == 32);
  KJ_EXPECT(memcmp(content.begin(), reg.getPublicKey().bytes, 32) == 0);
}

KJ_TEST("ClusterLockManager: OwnedLock destructor truncates and releases") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  {
    auto result = lockManager.acquireOrRoute("a5").wait(io.waitScope);
    KJ_ASSERT(result.is<ClusterLockManager::OwnedLock>());
    // Lock file should have our key.
    auto content = tmpDir.readLockFile("a5");
    KJ_ASSERT(content.size() == 32);
  }

  // After OwnedLock destructor, the file should be truncated to 0 bytes (and the OFD lock
  // released).
  auto content = tmpDir.readLockFile("a5");
  KJ_EXPECT(content.size() == 0);

  // A second acquire should now succeed via the claim path.
  auto result2 = lockManager.acquireOrRoute("a5").wait(io.waitScope);
  KJ_ASSERT(result2.is<ClusterLockManager::OwnedLock>());
}

KJ_TEST("ClusterLockManager: owned by live peer -> returns bootstrap client") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  // Stand up two registries in the same dir (this creates registry entries for both, so neither
  // is "dead").
  ClusterRegistry reg1(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());
  ClusterRegistry reg2(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc1 = capnp::makeRpcServer(reg1, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  // reg1 owns a ClusterLockManager; we'll set up a lock file naming reg2 as the owner.
  // reg2 is alive (its registry file exists in tmpDir), so isPeerDead(reg2) returns false,
  // and acquireOrRoute should return a bootstrap client without falling through to claim.
  //
  // Note: actor IDs aren't hex of 64 chars, so they won't collide with registry filenames.
  tmpDir.writeLockFile("a7", kj::arrayPtr(reg2.getPublicKey().bytes, 32));

  KJ_EXPECT(!reg1.isPeerDead(reg2.getPublicKey()));

  ClusterLockManager lockManager(tmpDir.locks(), reg1, rpc1, io.provider->getTimer());

  auto result = lockManager.acquireOrRoute("a7").wait(io.waitScope);
  KJ_ASSERT(result.is<rpc::WorkerdDebugPort::Client>());

  // The lock file should NOT have been modified (we did not claim).
  auto content = tmpDir.readLockFile("a7");
  KJ_ASSERT(content.size() == 32);
  KJ_EXPECT(memcmp(content.begin(), reg2.getPublicKey().bytes, 32) == 0);
}

KJ_TEST("ClusterLockManager: writer alive -> retries until writer releases") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  // Open the lock file from outside the ClusterLockManager and take an exclusive lock. This
  // simulates a writer that holds the lock but hasn't yet flushed its key. The file is left
  // empty (matching the freshly-truncated state during the writer's claim path).
  auto externalFile =
      tmpDir.locks()->openFile(kj::Path({"a8"}), kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  int externalFd = KJ_REQUIRE_NONNULL(externalFile->getFd());

  auto externalLock = KJ_ASSERT_NONNULL(OfdLock::tryLock(externalFd, OfdLock::EXCLUSIVE));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  // acquireOrRoute should see an empty file, attempt to lock, fail, then retry with backoff.
  auto promise = lockManager.acquireOrRoute("a8");

  bool completed = false;
  auto trackedPromise = promise.then(
      [&](kj::OneOf<ClusterLockManager::OwnedLock, rpc::WorkerdDebugPort::Client>&& r) {
    completed = true;
    return kj::mv(r);
  });

  // Pump the event loop briefly; the implementation should be stuck in its backoff loop.
  io.provider->getTimer().afterDelay(50 * kj::MILLISECONDS).wait(io.waitScope);
  KJ_EXPECT(!completed, "acquireOrRoute should still be waiting for the external lock");

  // Now the "external writer" writes its key (our own, for simplicity) and releases the lock.
  // acquireOrRoute, on its next iteration, should see the valid key, recognize the owner as us,
  // and return a bootstrap client.
  externalFile->write(0, kj::arrayPtr(reg.getPublicKey().bytes, 32));
  { auto _ = kj::mv(externalLock); }

  auto result = trackedPromise.wait(io.waitScope);
  KJ_EXPECT(completed);
  KJ_ASSERT(result.is<rpc::WorkerdDebugPort::Client>());
}

KJ_TEST("ClusterLockManager: concurrent acquisitions, exactly one wins") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(
      tmpDir.registry(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<StubDebugPort>()));

  ClusterLockManager lockManager(tmpDir.locks(), reg, rpc, io.provider->getTimer());

  // Launch several concurrent calls.
  constexpr uint N = 5;
  kj::Vector<kj::Promise<kj::OneOf<ClusterLockManager::OwnedLock, rpc::WorkerdDebugPort::Client>>>
      promises;
  for (uint i = 0; i < N; ++i) {
    promises.add(lockManager.acquireOrRoute("a9"));
  }

  auto results = kj::joinPromises(promises.releaseAsArray()).wait(io.waitScope);

  uint ownedCount = 0;
  uint routedCount = 0;
  for (auto& r: results) {
    if (r.is<ClusterLockManager::OwnedLock>()) {
      ++ownedCount;
    } else {
      ++routedCount;
    }
  }
  // Exactly one should have won.
  KJ_EXPECT(ownedCount == 1);
  KJ_EXPECT(routedCount == N - 1);
}

#else  // #if __linux__

KJ_TEST("dummy test: platform not supported") {}

#endif  // #if __linux__, #else

}  // namespace
}  // namespace workerd
