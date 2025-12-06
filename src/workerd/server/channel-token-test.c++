// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "channel-token.h"

#include <capnp/message.h>
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

class MockSubrequestChannel: public IoChannelFactory::SubrequestChannel {
 public:
  MockSubrequestChannel(ServiceTriplet triplet): triplet(kj::mv(triplet)) {}
  ServiceTriplet triplet;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
};

class MockActorClassChannel: public IoChannelFactory::ActorClassChannel {
 public:
  MockActorClassChannel(ServiceTriplet triplet): triplet(kj::mv(triplet)) {}
  ServiceTriplet triplet;

  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
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

KJ_TEST("channel token basics") {
  MockResolver resolver;
  ChannelTokenHandler handler(resolver);

  auto props = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto token = handler.encodeSubrequestChannelToken(Usage::RPC, "foo", "MyEntry"_kj, props);

  // Decoding works.
  {
    auto channel =
        handler.decodeSubrequestChannelToken(Usage::RPC, token).downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet == ServiceTriplet("foo", "MyEntry"_kj, props.clone()));
  }

  auto corruptedToken = [&](uint index) {
    auto copy = kj::heapArray(token.asPtr());
    copy[index] = 0;
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
  auto token = handler.encodeSubrequestChannelToken(Usage::STORAGE, "foo", "MyEntry"_kj, props);

  // Decoding works.
  {
    auto channel = handler.decodeSubrequestChannelToken(Usage::STORAGE, token)
                       .downcast<MockSubrequestChannel>();
    KJ_EXPECT(channel->triplet == ServiceTriplet("foo", "MyEntry"_kj, props.clone()));
  }

  auto corruptedToken = [&](uint index) {
    auto copy = kj::heapArray(token.asPtr());
    copy[index] = 0;
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
  auto token = handler.encodeActorClassChannelToken(Usage::RPC, "foo", "MyEntry"_kj, props);

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

}  // namespace
}  // namespace workerd::server
