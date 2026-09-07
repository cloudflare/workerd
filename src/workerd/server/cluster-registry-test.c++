// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "cluster-registry.h"

#include <stdlib.h>

#if !_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <capnp/capability.h>
#include <capnp/rpc.h>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd {
namespace {

#if __linux__

// A trivial capability server that counts calls.
class CallCountServer final: public capnp::Capability::Server {
 public:
  int callCount = 0;

  DispatchCallResult dispatchCall(uint64_t interfaceId,
      uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    ++callCount;
    return {kj::READY_NOW, false, true};
  }
};

// Helper to create a temp directory on disk.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/cluster-registry-test-XXXXXX";
    KJ_ASSERT(mkdtemp(tmpl) != nullptr);
    pathStr = kj::str(tmpl);
    disk = kj::newDiskFilesystem();
    dir = disk->getRoot().openSubdir(kj::Path::parse(pathStr.slice(1)),  // remove leading /
        kj::WriteMode::MODIFY);
  }

  ~TempDir() noexcept(false) {
    auto p = kj::Path::parse(pathStr.slice(1));
    disk->getRoot().remove(p);
  }

  kj::Own<const kj::Directory> get() {
    return dir->clone();
  }

 private:
  kj::String pathStr;
  kj::Own<kj::Filesystem> disk;
  kj::Own<const kj::Directory> dir;
};

// Helper to make a simple RPC call on a capability client.
void makeCall(capnp::Capability::Client& client, kj::WaitScope& ws) {
  auto req = client.typelessRequest(
      0, 0, capnp::MessageSize{4, 0}, capnp::Capability::Client::CallHints());
  req.send().wait(ws);
}

// A kj::Network wrapper whose outbound connects never complete, simulating a peer address that
// blackholes our SYNs. Listening is delegated unchanged.
class BlackholeNetwork final: public kj::Network {
 public:
  BlackholeNetwork(kj::Network& inner): inner(inner) {}

  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
      kj::StringPtr addr, uint portHint) override {
    return inner.parseAddress(addr, portHint)
        .then([](kj::Own<kj::NetworkAddress> a) -> kj::Own<kj::NetworkAddress> {
      return kj::heap<Address>(kj::mv(a));
    });
  }

  kj::Own<kj::NetworkAddress> getSockaddr(const void* sockaddr, uint len) override {
    return kj::heap<Address>(inner.getSockaddr(sockaddr, len));
  }

  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override {
    KJ_UNIMPLEMENTED("not needed in test");
  }

 private:
  class Address final: public kj::NetworkAddress {
   public:
    Address(kj::Own<kj::NetworkAddress> inner): inner(kj::mv(inner)) {}

    kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
      return kj::NEVER_DONE;
    }
    kj::Own<kj::ConnectionReceiver> listen() override {
      return inner->listen();
    }
    kj::Own<kj::NetworkAddress> clone() override {
      return kj::heap<Address>(inner->clone());
    }
    kj::String toString() override {
      return inner->toString();
    }

   private:
    kj::Own<kj::NetworkAddress> inner;
  };

  kj::Network& inner;
};

KJ_TEST("X25519PublicKey: hex roundtrip") {
  X25519PublicKey key;
  memset(key.bytes, 0xAB, sizeof(key.bytes));
  auto hex = key.toHex();
  KJ_EXPECT(hex.size() == 64);
  auto decoded = X25519PublicKey::fromHex(hex);
  KJ_EXPECT(decoded == key);
}

KJ_TEST("X25519PublicKey: equality") {
  X25519PublicKey a, b;
  memset(a.bytes, 1, sizeof(a.bytes));
  memset(b.bytes, 1, sizeof(b.bytes));
  KJ_EXPECT(a == b);

  b.bytes[0] = 2;
  KJ_EXPECT(!(a == b));
}

KJ_TEST("ClusterRegistry: self is never dead") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  KJ_EXPECT(!reg.isPeerDead(reg.getPublicKey()));
}

KJ_TEST("ClusterRegistry: self-connect returns none") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  capnp::MallocMessageBuilder msg;
  auto vatId = msg.initRoot<cluster::VatId>();
  vatId.setPublicKey(kj::arrayPtr(reg.getPublicKey().bytes, 32));

  KJ_EXPECT(reg.connect(vatId.asReader()) == kj::none);
}

KJ_TEST("ClusterRegistry: self-bootstrap returns local capability") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto server = kj::heap<CallCountServer>();
  auto& serverRef = *server;
  auto bootstrap = capnp::Capability::Client(kj::mv(server));
  auto rpc = capnp::makeRpcServer(reg, kj::mv(bootstrap));

  capnp::MallocMessageBuilder msg;
  auto vatId = msg.initRoot<cluster::VatId>();
  vatId.setPublicKey(kj::arrayPtr(reg.getPublicKey().bytes, 32));

  auto client = rpc.bootstrap(vatId);
  makeCall(client, io.waitScope);
  KJ_EXPECT(serverRef.callCount == 1);
}

KJ_TEST("ClusterRegistry: peer discovery via directory scan") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg1(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());
  ClusterRegistry reg2(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  // In unix mode, socket files in the directory are registry entries.
  KJ_EXPECT(!reg1.isPeerDead(reg2.getPublicKey()));
  KJ_EXPECT(!reg2.isPeerDead(reg1.getPublicKey()));
}

KJ_TEST("ClusterRegistry: isPeerDead caches live peers briefly") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg1(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());
  ClusterRegistry reg2(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto peerPath = kj::Path({reg2.getPublicKey().toHex()});
  KJ_EXPECT(!reg1.isPeerDead(reg2.getPublicKey()));

  tmpDir.get()->tryRemove(peerPath);
  KJ_EXPECT(!reg1.isPeerDead(reg2.getPublicKey()));
}

KJ_TEST("ClusterRegistry: isPeerDead caches dead peers permanently") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  X25519PublicKey peerKey;
  memset(peerKey.bytes, 0xCA, sizeof(peerKey.bytes));
  auto peerPath = kj::Path({peerKey.toHex()});

  KJ_EXPECT(reg.isPeerDead(peerKey));
  tmpDir.get()->openFile(peerPath, kj::WriteMode::CREATE)->writeAll("placeholder");
  KJ_EXPECT(reg.isPeerDead(peerKey));
}

KJ_TEST("ClusterRegistry: end-to-end RPC between two registries") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg1(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());
  ClusterRegistry reg2(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto maint1 = reg1.runMaintenance();
  auto maint2 = reg2.runMaintenance();

  auto server1 = kj::heap<CallCountServer>();
  auto& server1Ref = *server1;
  auto server2 = kj::heap<CallCountServer>();
  auto& server2Ref = *server2;

  auto rpc1 = capnp::makeRpcServer(reg1, capnp::Capability::Client(kj::mv(server1)));
  auto rpc2 = capnp::makeRpcServer(reg2, capnp::Capability::Client(kj::mv(server2)));

  auto run1 = rpc1.run();
  auto run2 = rpc2.run();

  // reg1 -> reg2
  {
    capnp::MallocMessageBuilder msg;
    auto vatId = msg.initRoot<cluster::VatId>();
    vatId.setPublicKey(kj::arrayPtr(reg2.getPublicKey().bytes, 32));

    auto client = rpc1.bootstrap(vatId);
    makeCall(client, io.waitScope);
    KJ_EXPECT(server2Ref.callCount == 1);
  }

  // reg2 -> reg1
  {
    capnp::MallocMessageBuilder msg;
    auto vatId = msg.initRoot<cluster::VatId>();
    vatId.setPublicKey(kj::arrayPtr(reg1.getPublicKey().bytes, 32));

    auto client = rpc2.bootstrap(vatId);
    makeCall(client, io.waitScope);
    KJ_EXPECT(server1Ref.callCount == 1);
  }
}

KJ_TEST("ClusterRegistry: end-to-end RPC with binary IP registry entries") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg1(
      tmpDir.get(), "127.0.0.0/8"_kj, io.provider->getNetwork(), io.provider->getTimer());
  ClusterRegistry reg2(
      tmpDir.get(), "127.0.0.0/8"_kj, io.provider->getNetwork(), io.provider->getTimer());

  auto maint1 = reg1.runMaintenance();
  auto maint2 = reg2.runMaintenance();

  auto server2 = kj::heap<CallCountServer>();
  auto& server2Ref = *server2;

  auto rpc1 = capnp::makeRpcServer(reg1, capnp::Capability::Client(kj::heap<CallCountServer>()));
  auto rpc2 = capnp::makeRpcServer(reg2, capnp::Capability::Client(kj::mv(server2)));

  auto run1 = rpc1.run();
  auto run2 = rpc2.run();

  capnp::MallocMessageBuilder msg;
  auto vatId = msg.initRoot<cluster::VatId>();
  vatId.setPublicKey(kj::arrayPtr(reg2.getPublicKey().bytes, 32));

  auto client = rpc1.bootstrap(vatId);
  makeCall(client, io.waitScope);
  KJ_EXPECT(server2Ref.callCount == 1);
}

KJ_TEST("ClusterRegistry: idle timeout closes connections") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  auto shortTimeout = 100 * kj::MILLISECONDS;

  ClusterRegistry reg1(
      tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer(), shortTimeout);
  ClusterRegistry reg2(
      tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer(), shortTimeout);

  auto server2 = kj::heap<CallCountServer>();
  auto& server2Ref = *server2;

  auto rpc1 = capnp::makeRpcServer(reg1, capnp::Capability::Client(kj::heap<CallCountServer>()));
  auto rpc2 = capnp::makeRpcServer(reg2, capnp::Capability::Client(kj::mv(server2)));

  auto run1 = rpc1.run();
  auto run2 = rpc2.run();

  capnp::MallocMessageBuilder msg;
  auto vatId = msg.initRoot<cluster::VatId>();
  vatId.setPublicKey(kj::arrayPtr(reg2.getPublicKey().bytes, 32));

  {
    auto client = rpc1.bootstrap(vatId);
    makeCall(client, io.waitScope);
    KJ_EXPECT(server2Ref.callCount == 1);
  }
  // Client dropped — connection should become idle.

  // Wait for the idle timeout to fire.
  io.provider->getTimer().afterDelay(shortTimeout + 200 * kj::MILLISECONDS).wait(io.waitScope);

  // After idle close, a new bootstrap should still work (new connection).
  auto client2 = rpc1.bootstrap(vatId);
  makeCall(client2, io.waitScope);
  KJ_EXPECT(server2Ref.callCount == 2);
}

KJ_TEST("ClusterRegistry: hung connect is torn down and dead peer cleaned up within timeout") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  auto connectTimeout = 100 * kj::MILLISECONDS;
  BlackholeNetwork network(io.provider->getNetwork());

  ClusterRegistry reg(tmpDir.get(), "127.0.0.0/8"_kj, network, io.provider->getTimer(),
      60 * kj::SECONDS, connectTimeout);
  auto maint = reg.runMaintenance();

  // Forge a registry entry for a peer that is gone: a valid address (copied from our own entry),
  // not locked by anyone, and old enough to be past the new-entry grace period.
  X25519PublicKey peerKey;
  memset(peerKey.bytes, 0xBE, sizeof(peerKey.bytes));
  auto peerPath = kj::Path({peerKey.toHex()});
  {
    auto dir = tmpDir.get();
    auto ownEntry = dir->openFile(kj::Path({reg.getPublicKey().toHex()}))->readAllBytes();
    auto peerFile = dir->openFile(peerPath, kj::WriteMode::CREATE);
    peerFile->writeAll(ownEntry);
    timespec old[2] = {{.tv_sec = 1, .tv_nsec = 0}, {.tv_sec = 1, .tv_nsec = 0}};
    KJ_SYSCALL(futimens(KJ_ASSERT_NONNULL(peerFile->getFd()), old));
  }

  auto rpc = capnp::makeRpcServer(reg, capnp::Capability::Client(kj::heap<CallCountServer>()));
  auto run = rpc.run();

  capnp::MallocMessageBuilder msg;
  auto vatId = msg.initRoot<cluster::VatId>();
  vatId.setPublicKey(kj::arrayPtr(peerKey.bytes, 32));

  auto& timer = io.provider->getTimer();
  auto start = timer.now();
  {
    auto client = rpc.bootstrap(vatId);
    KJ_EXPECT_THROW_MESSAGE("timed out", makeCall(client, io.waitScope));
  }

  // Give the RPC system a chance to finish dropping the connection.
  timer.afterDelay(connectTimeout).wait(io.waitScope);
  KJ_EXPECT(timer.now() - start < 10 * connectTimeout);

  // The dead-peer probe must have run: the unlocked, stale entry is removed and the peer is dead.
  KJ_EXPECT(!tmpDir.get()->exists(peerPath));
  KJ_EXPECT(reg.isPeerDead(peerKey));
}

KJ_TEST("ClusterRegistry: verifyNfsLease() succeeds when lock is held") {
  auto io = kj::setupAsyncIo();
  TempDir tmpDir;

  ClusterRegistry reg(tmpDir.get(), "unix"_kj, io.provider->getNetwork(), io.provider->getTimer());

  reg.verifyNfsLease();
}

#else  // #if __linux__

KJ_TEST("dummy test: platform not supported") {}

#endif  // #if __linux__, #else

}  // namespace
}  // namespace workerd
