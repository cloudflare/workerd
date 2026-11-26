// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests that each channel Rust holds as an io_channels newtype is the C++ object it came from: a
// channel handed to Rust (test_bridge.rs) and back is the same object, and the newtype and its
// clones are references in the object's own count.

#include <workerd/io/io-channels/test_bridge.rs.h>

#include <kj/test.h>

namespace workerd::io_channels::test {
namespace {

class TestSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
};

class TestActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
};

class TestRpcChannel final: public IoChannelFactory::RpcChannel {
 public:
  Session restore() override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_UNIMPLEMENTED("not used by these tests");
  }
};

// Sends a `Derived`, as its base `Channel`, through each of the bridge's functions for it.
template <typename Channel, typename Derived>
void checkRoundTrips(kj::Rc<Channel> (*intoKj)(kj::Rc<Channel>),
    kj::Rc<Channel> (*asKj)(kj::Rc<Channel>),
    ::rust::Box<Held> (*hold)(kj::Rc<Channel>)) {
  auto channel = kj::rc<Derived>();
  auto ref = [&]() -> kj::Rc<Channel> { return channel.addRef(); };

  // The same object comes back, holding the one reference that was handed over.
  {
    auto back = intoKj(ref());
    KJ_EXPECT(back.get() == channel.get());
    KJ_EXPECT(channel->isShared());
  }
  KJ_EXPECT(!channel->isShared());

  {
    auto back = asKj(ref());
    KJ_EXPECT(back.get() == channel.get());
    KJ_EXPECT(channel->isShared());
  }
  KJ_EXPECT(!channel->isShared());

  // The newtype and its clone are two references in the object's count.
  auto held = hold(ref());
  KJ_EXPECT(channel->isShared());
  held->release_one();
  KJ_EXPECT(channel->isShared());
  held->release_one();
  KJ_EXPECT(!channel->isShared());
}

KJ_TEST("a SubrequestChannel is the same object in Rust") {
  checkRoundTrips<IoChannelFactory::SubrequestChannel, TestSubrequestChannel>(
      &subrequest_into_kj, &subrequest_as_kj, &subrequest_hold);
}

KJ_TEST("an ActorClassChannel is the same object in Rust") {
  checkRoundTrips<IoChannelFactory::ActorClassChannel, TestActorClassChannel>(
      &actor_class_into_kj, &actor_class_as_kj, &actor_class_hold);
}

KJ_TEST("an RpcChannel is the same object in Rust") {
  checkRoundTrips<IoChannelFactory::RpcChannel, TestRpcChannel>(
      &rpc_into_kj, &rpc_as_kj, &rpc_hold);
}

}  // namespace
}  // namespace workerd::io_channels::test
