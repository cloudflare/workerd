// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "channel-token.h"

#include <capnp/message.h>
#include <kj/async.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

kj::String strProps(const Frankenvalue& props) {
  capnp::MallocMessageBuilder message;
  auto builder = message.getRoot<rpc::Frankenvalue>();
  props.threadSafeClone().toCapnp(builder);
  return kj::str(builder.asReader());
}

struct ServiceTriplet {
  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Frankenvalue props;

  ServiceTriplet(kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props)
      : serviceName(kj::str(serviceName)),
        entrypoint(entrypoint.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)) {}
  ServiceTriplet(ServiceTriplet&&) = default;

  bool operator==(const ServiceTriplet& other) const {
    return serviceName == other.serviceName && entrypoint == other.entrypoint &&
        strProps(props) == strProps(other.props);
  }

  kj::String toString() const {
    return kj::str('(', serviceName, ", ", entrypoint, ", ", strProps(props), ')');
  }
};

kj::Array<byte> expectSync(kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> variant) {
  return KJ_ASSERT_NONNULL(
      kj::mv(variant).tryGet<kj::Array<byte>>(), "expected token to be rendered synchronously");
}

class MockSubrequestChannel: public IoChannelFactory::SubrequestChannel {
 public:
  // Simple mock used by the resolver when decoding tokens. Its getTokenMaybeSync() is never
  // called in that context.
  MockSubrequestChannel(ServiceTriplet triplet): triplet(kj::mv(triplet)) {}

  // Mock used as a nested cap inside a parent channel's props. It generates its own token by
  // calling back into the ChannelTokenHandler. If `readyPromise` is provided, the token is only
  // produced asynchronously after `readyPromise` resolves.
  MockSubrequestChannel(ChannelTokenHandler& handler,
      ServiceTriplet triplet,
      kj::Maybe<kj::Promise<void>> readyPromise = kj::none)
      : handler(handler),
        triplet(kj::mv(triplet)),
        readyPromise(kj::mv(readyPromise)) {}

  kj::Maybe<ChannelTokenHandler&> handler;
  ServiceTriplet triplet;
  kj::Maybe<kj::Promise<void>> readyPromise;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    auto& h = KJ_ASSERT_NONNULL(handler, "this mock was not constructed with a handler ref");
    KJ_IF_SOME(p, readyPromise) {
      auto promise = kj::mv(p);
      readyPromise = kj::none;
      return promise.then([&h, usage, this]() mutable -> kj::Array<byte> {
        return expectSync(h.encodeSubrequestChannelToken(usage, triplet.serviceName,
            triplet.entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }),
            triplet.props));
      });
    } else {
      return expectSync(h.encodeSubrequestChannelToken(usage, triplet.serviceName,
          triplet.entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }), triplet.props));
    }
  }
};

class MockActorClassChannel: public IoChannelFactory::ActorClassChannel {
 public:
  MockActorClassChannel(ServiceTriplet triplet): triplet(kj::mv(triplet)) {}

  MockActorClassChannel(ChannelTokenHandler& handler,
      ServiceTriplet triplet,
      kj::Maybe<kj::Promise<void>> readyPromise = kj::none)
      : handler(handler),
        triplet(kj::mv(triplet)),
        readyPromise(kj::mv(readyPromise)) {}

  kj::Maybe<ChannelTokenHandler&> handler;
  ServiceTriplet triplet;
  kj::Maybe<kj::Promise<void>> readyPromise;

  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    auto& h = KJ_ASSERT_NONNULL(handler, "this mock was not constructed with a handler ref");
    KJ_IF_SOME(p, readyPromise) {
      auto promise = kj::mv(p);
      readyPromise = kj::none;
      return promise.then([&h, usage, this]() mutable -> kj::Array<byte> {
        return expectSync(h.encodeActorClassChannelToken(usage, triplet.serviceName,
            triplet.entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }),
            triplet.props));
      });
    } else {
      return expectSync(h.encodeActorClassChannelToken(usage, triplet.serviceName,
          triplet.entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }), triplet.props));
    }
  }
};

class MockResolver: public ChannelTokenHandler::Resolver {
 public:
  kj::Own<IoChannelFactory::SubrequestChannel> resolveEntrypoint(
      kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) override {
    return kj::refcounted<MockSubrequestChannel>(
        ServiceTriplet(serviceName, entrypoint, kj::mv(props)));
  }

  kj::Own<IoChannelFactory::ActorClassChannel> resolveActorClass(
      kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) override {
    return kj::refcounted<MockActorClassChannel>(
        ServiceTriplet(serviceName, entrypoint, kj::mv(props)));
  }
};

using Usage = IoChannelFactory::ChannelTokenUsage;

// Build a Frankenvalue whose capTable contains the given entries. The base value is an empty
// object. This goes through the capnp serialization path since Frankenvalue doesn't otherwise
// expose a way to construct a cap table directly.
Frankenvalue propsWithCaps(kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> caps) {
  capnp::MallocMessageBuilder message;
  auto builder = message.getRoot<rpc::Frankenvalue>();
  builder.setEmptyObject();
  builder.setCapTableSize(caps.size());
  return Frankenvalue::fromCapnp(builder.asReader(), kj::mv(caps));
}

KJ_TEST("channel token basics") {
  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  auto props = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto token =
      expectSync(handler.encodeSubrequestChannelToken(Usage::RPC, "foo", "MyEntry"_kj, props));

  // Decoding works.
  {
    auto channel =
        handler.decodeSubrequestChannelToken(Usage::RPC, token).downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet == ServiceTriplet("foo", "MyEntry"_kj, props.clone()));
  }

  auto corruptedToken = [&](uint index) {
    auto copy = kj::heapArray(token.asPtr());
    copy[index] ^= 1;
    return copy;
  };

  // Corrupting any byte of the token should make it invalid.
  {
    // Corrupt the magic number.
    KJ_EXPECT_THROW_MESSAGE(
        "RPC_TOKEN_MAGIC", handler.decodeSubrequestChannelToken(Usage::RPC, corruptedToken(2)));

    // Corrupt the MAC.
    KJ_EXPECT_THROW_MESSAGE("failed authentication",
        handler.decodeSubrequestChannelToken(Usage::RPC, corruptedToken(token.size() - 2)));

    // Corrupt the IV.
    KJ_EXPECT_THROW_MESSAGE("failed authentication",
        handler.decodeSubrequestChannelToken(Usage::RPC, corruptedToken(7)));

    // Corrupt the key ID.
    KJ_EXPECT_THROW_MESSAGE("failed authentication",
        handler.decodeSubrequestChannelToken(Usage::RPC, corruptedToken(20)));

    // Corrupt the message body.
    KJ_EXPECT_THROW_MESSAGE("failed authentication",
        handler.decodeSubrequestChannelToken(Usage::RPC, corruptedToken(37)));
  }

  // Can't parse as a storage token.
  KJ_EXPECT_THROW_MESSAGE(
      "STORAGE_TOKEN_MAGIC", handler.decodeSubrequestChannelToken(Usage::STORAGE, token));

  // Can't use as wrong type.
  KJ_EXPECT_THROW_MESSAGE(
      "channel token type mismatch", handler.decodeActorClassChannelToken(Usage::RPC, token));
}

KJ_TEST("channel tokens for storage") {
  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  auto props = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto token =
      expectSync(handler.encodeSubrequestChannelToken(Usage::STORAGE, "foo", "MyEntry"_kj, props));

  // Decoding works.
  {
    auto channel = handler.decodeSubrequestChannelToken(Usage::STORAGE, token)
                       .downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet == ServiceTriplet("foo", "MyEntry"_kj, props.clone()));
  }

  auto corruptedToken = [&](uint index) {
    auto copy = kj::heapArray(token.asPtr());
    copy[index] ^= 1;
    return copy;
  };

  // Corrupting the magic number breaks the token.
  KJ_EXPECT_THROW_MESSAGE("STORAGE_TOKEN_MAGIC",
      handler.decodeSubrequestChannelToken(Usage::STORAGE, corruptedToken(2)));

  // Can't parse as an RPC token.
  KJ_EXPECT_THROW_MESSAGE(
      "RPC_TOKEN_MAGIC", handler.decodeSubrequestChannelToken(Usage::RPC, token));

  // Can't use as wrong type.
  KJ_EXPECT_THROW_MESSAGE(
      "channel token type mismatch", handler.decodeActorClassChannelToken(Usage::STORAGE, token));
}

KJ_TEST("actor class channel tokens") {
  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  auto props = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto token =
      expectSync(handler.encodeActorClassChannelToken(Usage::RPC, "foo", "MyEntry"_kj, props));

  // Decoding works.
  {
    auto channel =
        handler.decodeActorClassChannelToken(Usage::RPC, token).downcast<MockActorClassChannel>();
    KJ_EXPECT(channel->triplet == ServiceTriplet("foo", "MyEntry"_kj, props.clone()));
  }

  // Decoding as the wrong type fails.
  KJ_EXPECT_THROW_MESSAGE(
      "channel token type mismatch", handler.decodeSubrequestChannelToken(Usage::RPC, token));
}

KJ_TEST("channel token with nested channels (all synchronous)") {
  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  // Build a props cap table containing a SubrequestChannel and an ActorClassChannel, both of
  // which produce their tokens synchronously.
  kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> caps;
  caps.add(kj::refcounted<MockSubrequestChannel>(handler,
      ServiceTriplet(
          "nested-subreq", "NestedEntry"_kj, Frankenvalue::fromJson(kj::str("{\"inner\": 1}")))));
  caps.add(kj::refcounted<MockActorClassChannel>(handler,
      ServiceTriplet("nested-actor", kj::Maybe<kj::StringPtr>(kj::none),
          Frankenvalue::fromJson(kj::str("{\"inner\": 2}")))));
  auto props = propsWithCaps(kj::mv(caps));

  // Encoding is synchronous.
  auto token =
      expectSync(handler.encodeSubrequestChannelToken(Usage::RPC, "outer", "OuterEntry"_kj, props));

  // Decoding works and restores the nested channels.
  {
    auto channel =
        handler.decodeSubrequestChannelToken(Usage::RPC, token).downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet.serviceName == "outer");
    KJ_EXPECT(channel->triplet.entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }) ==
        "OuterEntry"_kj);

    auto capTable = channel->triplet.props.getCapTable();
    KJ_ASSERT(capTable.size() == 2);

    auto& nestedSub = KJ_ASSERT_NONNULL(kj::tryDowncast<MockSubrequestChannel>(*capTable[0]),
        "expected nested cap 0 to be a SubrequestChannel");
    KJ_EXPECT(nestedSub.triplet ==
        ServiceTriplet(
            "nested-subreq", "NestedEntry"_kj, Frankenvalue::fromJson(kj::str("{\"inner\": 1}"))));

    auto& nestedActor = KJ_ASSERT_NONNULL(kj::tryDowncast<MockActorClassChannel>(*capTable[1]),
        "expected nested cap 1 to be an ActorClassChannel");
    KJ_EXPECT(nestedActor.triplet ==
        ServiceTriplet("nested-actor", kj::Maybe<kj::StringPtr>(kj::none),
            Frankenvalue::fromJson(kj::str("{\"inner\": 2}"))));
  }

  // Also works with STORAGE usage.
  auto storageToken = expectSync(
      handler.encodeSubrequestChannelToken(Usage::STORAGE, "outer", "OuterEntry"_kj, props));
  {
    auto channel = handler.decodeSubrequestChannelToken(Usage::STORAGE, storageToken)
                       .downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet.props.getCapTable().size() == 2);
  }

  // And the outer channel can itself be an ActorClassChannel.
  auto actorToken =
      expectSync(handler.encodeActorClassChannelToken(Usage::RPC, "outer", "OuterEntry"_kj, props));
  {
    auto channel = handler.decodeActorClassChannelToken(Usage::RPC, actorToken)
                       .downcast<MockActorClassChannel>();
    KJ_EXPECT(channel->triplet.props.getCapTable().size() == 2);
  }
}

KJ_TEST("channel token with nested channel that generates token asynchronously") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  // One nested channel is ready synchronously, the other only becomes ready when we fulfill a
  // paf. The whole encoding therefore cannot complete until we fulfill the paf.
  auto paf = kj::newPromiseAndFulfiller<void>();

  kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> caps;
  caps.add(kj::refcounted<MockSubrequestChannel>(handler,
      ServiceTriplet("sync-subreq", "SyncEntry"_kj,
          Frankenvalue::fromJson(kj::str("{\"inner\": \"sync\"}")))));
  caps.add(kj::refcounted<MockActorClassChannel>(handler,
      ServiceTriplet("async-actor", "AsyncEntry"_kj,
          Frankenvalue::fromJson(kj::str("{\"inner\": \"async\"}"))),
      kj::mv(paf.promise)));
  auto props = propsWithCaps(kj::mv(caps));

  // Encoding returns a promise rather than a synchronous result, because one of the nested
  // channels needs to wait before generating its token.
  auto tokenOneOf =
      handler.encodeSubrequestChannelToken(Usage::RPC, "outer", "OuterEntry"_kj, props);
  auto tokenPromise = KJ_ASSERT_NONNULL(kj::mv(tokenOneOf).tryGet<kj::Promise<kj::Array<byte>>>(),
      "expected token to be rendered asynchronously when a nested channel is pending");

  // The promise should not be ready yet.
  KJ_EXPECT(!tokenPromise.poll(waitScope));

  // Once we fulfill the pending promise, the token is produced.
  paf.fulfiller->fulfill();
  auto token = tokenPromise.wait(waitScope);

  // Decoding works and restores the nested channels.
  auto channel =
      handler.decodeSubrequestChannelToken(Usage::RPC, token).downcast<MockSubrequestChannel>();
  KJ_EXPECT(channel->triplet.serviceName == "outer");

  auto capTable = channel->triplet.props.getCapTable();
  KJ_ASSERT(capTable.size() == 2);

  auto& nestedSub = KJ_ASSERT_NONNULL(kj::tryDowncast<MockSubrequestChannel>(*capTable[0]),
      "expected nested cap 0 to be a SubrequestChannel");
  KJ_EXPECT(nestedSub.triplet ==
      ServiceTriplet(
          "sync-subreq", "SyncEntry"_kj, Frankenvalue::fromJson(kj::str("{\"inner\": \"sync\"}"))));

  auto& nestedActor = KJ_ASSERT_NONNULL(kj::tryDowncast<MockActorClassChannel>(*capTable[1]),
      "expected nested cap 1 to be an ActorClassChannel");
  KJ_EXPECT(nestedActor.triplet ==
      ServiceTriplet("async-actor", "AsyncEntry"_kj,
          Frankenvalue::fromJson(kj::str("{\"inner\": \"async\"}"))));
}

}  // namespace
}  // namespace workerd::server
