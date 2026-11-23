// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <workerd/jsg/setup.h>
#include <workerd/server/factory/worker-factory.h>
#include <workerd/server/workerd.capnp.h>

#include <kj-rs/convert.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/async-io.h>
#include <kj/filesystem.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

jsg::V8System v8System;

class NullEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    buffer.fill(0);
  }
};

// A factory over an empty config: enough for RPC clients, which run no worker here.
struct TestFactory {
  explicit TestFactory(kj::AsyncIoProvider& provider) {
    capnp::MallocMessageBuilder message;
    message.initRoot<config::Config>();
    auto words = capnp::messageToFlatArray(message);
    auto config = kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), words.size())
                      .as<kj_rs::RustCopy>();
    factory = kj::heap<WorkerFactory>(v8System, provider.getTimer(),
        kj::systemPreciseMonotonicClock(), provider.getNetwork(), entropy, *fs,
        kj::heap<WorkerFactory::Options>(), kj::mv(config));
  }

  NullEntropySource entropy;
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  kj::Own<WorkerFactory> factory;
};

// Bootstrapping the session again over the failed connection would throw from capnp ("expected
// idle != this->idle") instead of failing the actor's calls as disconnected.
KJ_TEST("a peer session whose connection failed with a call out fails later actors' calls") {
  auto io = kj::setupAsyncIo();
  TestFactory test(*io.provider);
  auto pipe = io.provider->newTwoWayPipe();
  auto session = new_rpc_client(*test.factory, kj::mv(pipe.ends[0]));

  // A getActor on the session, which the peer reads and never answers.
  auto first = rpc_client_debug_actor(*session, "service", "Class", "id");
  auto buffer = kj::heapArray<kj::byte>(4096);
  KJ_ASSERT(pipe.ends[1]->tryRead(buffer.begin(), 1, buffer.size()).wait(io.waitScope) > 0);

  // The peer goes away with that call out, and the session sees its connection fail.
  pipe.ends[1] = nullptr;
  auto lost = kj::runCatchingExceptions(
      [&]() { first->getImpl().target.whenResolved().wait(io.waitScope); });
  KJ_ASSERT(KJ_ASSERT_NONNULL(lost).getType() == kj::Exception::Type::DISCONNECTED);

  // An actor obtained over the session afterwards fails its calls as disconnected too.
  auto second = rpc_client_debug_actor(*session, "service", "Class", "id");
  auto failed = kj::runCatchingExceptions(
      [&]() { second->getImpl().target.whenResolved().wait(io.waitScope); });
  KJ_EXPECT(KJ_ASSERT_NONNULL(failed).getType() == kj::Exception::Type::DISCONNECTED);
}

}  // namespace
}  // namespace workerd::server
