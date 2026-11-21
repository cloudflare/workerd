// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The C++ `ChannelTokenHandler` and the Rust codec (channel-token-codec/) against each other: each
// redeems the other's tokens, and the Rust codec writes the same bytes as the handler for the same
// contents and IV.

#include "channel-token.h"

#include <workerd/server/channel-token-codec/ffi.rs.h>

#include <kj-rs/convert.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

namespace codec = channel_token_codec;
using Usage = IoChannelFactory::ChannelTokenUsage;

constexpr auto CLUSTER_KEY = "channel-token-codec-test-cluster-key"_kj;
const byte OTHER_IV[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

kj::String strProps(const Frankenvalue& props) {
  capnp::MallocMessageBuilder message;
  props.threadSafeClone().toCapnp(message.getRoot<rpc::Frankenvalue>());
  return kj::str(message.getRoot<rpc::Frankenvalue>().asReader());
}

kj::Array<byte> copy(const ::rust::Vec<uint8_t>& vec) {
  return kj::heapArray(kj::from<kj_rs::Rust>(vec));
}

kj::String str(const ::rust::Vec<uint8_t>& vec) {
  return kj::heapString(kj::from<kj_rs::Rust>(vec).asChars());
}

// The structure of a Frankenvalue that crossed from Rust, cap table included.
kj::String strFlat(const ::rust::Vec<uint8_t>& flat) {
  auto bytes = kj::from<kj_rs::Rust>(flat);
  auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
  words.asBytes().copyFrom(bytes);
  capnp::FlatArrayMessageReader reader(words);
  return kj::str(reader.getRoot<rpc::Frankenvalue>());
}

kj::Array<byte> expectSync(kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> variant) {
  return KJ_ASSERT_NONNULL(kj::mv(variant).tryGet<kj::Array<byte>>());
}

codec::TokenUsage rustUsage(Usage usage) {
  return usage == Usage::RPC ? codec::TokenUsage::Rpc : codec::TokenUsage::Storage;
}

// The IV of a token of `usage`: none for storage.
kj::ArrayPtr<const byte> ivOf(Usage usage, kj::ArrayPtr<const byte> token) {
  return usage == Usage::RPC ? token.slice(4, 16) : nullptr;
}

kj::ArrayPtr<const byte> otherIv(Usage usage) {
  return usage == Usage::RPC ? kj::arrayPtr(OTHER_IV) : nullptr;
}

class MockSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  MockSubrequestChannel(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent,
      kj::Maybe<ChannelTokenHandler&> handler = kj::none)
      : serviceName(kj::str(serviceName)),
        entrypoint(entrypoint.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)),
        persistent(persistent),
        handler(handler) {}

  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Frankenvalue props;
  Persistent persistent;
  kj::Maybe<ChannelTokenHandler&> handler;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    return KJ_ASSERT_NONNULL(handler).encodeSubrequestChannelToken(usage, serviceName,
        entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }), props, persistent);
  }
};

class MockActorClassChannel final: public IoChannelFactory::ActorClassChannel {
 public:
  MockActorClassChannel(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent)
      : serviceName(kj::str(serviceName)),
        entrypoint(entrypoint.map([](kj::StringPtr s) { return kj::str(s); })),
        props(kj::mv(props)),
        persistent(persistent) {}

  kj::String serviceName;
  kj::Maybe<kj::String> entrypoint;
  Frankenvalue props;
  Persistent persistent;

  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    KJ_UNREACHABLE;
  }
};

class MockActorChannel final: public IoChannelFactory::ActorChannel {
 public:
  MockActorChannel(kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent)
      : namespaceKey(kj::str(namespaceKey)),
        id(kj::heapArray(id)),
        name(name.map([](kj::StringPtr s) { return kj::str(s); })),
        persistent(persistent) {}

  kj::String namespaceKey;
  kj::Array<byte> id;
  kj::Maybe<kj::String> name;
  Persistent persistent;

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {
    KJ_UNREACHABLE;
  }
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    KJ_UNREACHABLE;
  }
};

class MockResolver final: public ChannelTokenHandler::Resolver {
 public:
  // The handler that the subrequest channels it resolves encode their tokens with.
  kj::Maybe<ChannelTokenHandler&> handler;

  kj::Rc<IoChannelFactory::SubrequestChannel> resolveEntrypoint(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override {
    return kj::rc<MockSubrequestChannel>(
        serviceName, entrypoint, kj::mv(props), persistent, handler);
  }
  kj::Rc<IoChannelFactory::ActorClassChannel> resolveActorClass(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent) override {
    return kj::rc<MockActorClassChannel>(serviceName, entrypoint, kj::mv(props), persistent);
  }
  kj::Rc<IoChannelFactory::ActorChannel> resolveActor(kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent) override {
    return kj::rc<MockActorChannel>(namespaceKey, id, name, persistent);
  }
};

struct Fixture {
  MockResolver resolver;
  ChannelTokenHandler handler{resolver, CLUSTER_KEY};
  bool init = (resolver.handler = handler, true);
  ::rust::Box<codec::TokenKey> key =
      codec::new_cluster_token_key(CLUSTER_KEY.asBytes().as<kj_rs::Rust>());

  codec::DecodedToken rustDecode(
      Usage usage, codec::TokenType type, kj::ArrayPtr<const byte> token) {
    return codec::decode_token(*key, rustUsage(usage), type, token.as<kj_rs::Rust>());
  }
};

// Props of each shape: JSON; a cap table, whose entries are nested tokens; and properties.
kj::Array<Frankenvalue> propsCases(ChannelTokenHandler& handler) {
  auto withCaps = Frankenvalue::fromJson(kj::str("{\"a\": 1}"));
  withCaps.setProperty(kj::str("nested"),
      Frankenvalue::fromCapability(7,
          kj::refcounted<MockSubrequestChannel>("nested-service", "NestedEntry"_kj,
              Frankenvalue::fromJson(kj::str("{\"inner\": true}")), Persistent::YES, handler)));
  withCaps.setProperty(kj::str("plain"), Frankenvalue::fromJson(kj::str("[1, 2, 3]")));

  return kj::arr(Frankenvalue::fromJson(kj::str("{\"foo\": 123}")),
      Frankenvalue::fromBytes(kj::heapArray<byte>({0, 1, 2, 3})), kj::mv(withCaps));
}

KJ_TEST("service tokens: Rust writes the handler's bytes, and each reads the other's") {
  Fixture f;
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    for (auto type: {ChannelToken::Type::SUBREQUEST, ChannelToken::Type::ACTOR_CLASS}) {
      auto rustType = static_cast<codec::TokenType>(type);
      auto cases = propsCases(f.handler);
      for (auto propsIndex: kj::indices(cases)) {
        auto& props = cases[propsIndex];
        for (auto entrypoint:
            {kj::Maybe<kj::StringPtr>("MyEntry"_kj), kj::Maybe<kj::StringPtr>()}) {
          auto cppToken = expectSync(type == ChannelToken::Type::SUBREQUEST
                  ? f.handler.encodeSubrequestChannelToken(
                        usage, "my-service", entrypoint, props, Persistent::YES)
                  : f.handler.encodeActorClassChannelToken(
                        usage, "my-service", entrypoint, props, Persistent::YES));

          // Rust reads the handler's token.
          auto decoded = f.rustDecode(usage, rustType, cppToken);
          KJ_EXPECT(decoded.kind == codec::DecodedKind::Service);
          KJ_EXPECT(decoded.persistent == codec::TokenPersistence::Yes);
          KJ_EXPECT(str(decoded.name) == "my-service");
          KJ_EXPECT(decoded.has_entrypoint == (entrypoint != kj::none));
          KJ_IF_SOME(e, entrypoint) {
            KJ_EXPECT(str(decoded.entrypoint) == e);
          }
          KJ_ASSERT(decoded.has_value);

          // With the same contents and IV, Rust writes the same bytes.
          auto rustSame = codec::encode_service_token(*f.key, rustUsage(usage),
              ivOf(usage, cppToken).as<kj_rs::Rust>(), rustType, codec::TokenPersistence::Yes,
              "my-service"_kj.asBytes().as<kj_rs::Rust>(),
              entrypoint.orDefault(""_kj).asBytes().as<kj_rs::Rust>(), entrypoint != kj::none,
              kj::from<kj_rs::Rust>(decoded.value).as<kj_rs::Rust>());
          KJ_EXPECT(copy(rustSame).asPtr() == cppToken.asPtr(), usage, type, propsIndex);

          // The handler reads a Rust token, with another IV.
          auto rustToken = copy(codec::encode_service_token(*f.key, rustUsage(usage),
              otherIv(usage).as<kj_rs::Rust>(), rustType, codec::TokenPersistence::Yes,
              "my-service"_kj.asBytes().as<kj_rs::Rust>(),
              entrypoint.orDefault(""_kj).asBytes().as<kj_rs::Rust>(), entrypoint != kj::none,
              kj::from<kj_rs::Rust>(decoded.value).as<kj_rs::Rust>()));
          if (usage == Usage::RPC) KJ_EXPECT(rustToken.asPtr() != cppToken.asPtr());

          Frankenvalue* readProps;
          kj::Own<void> keep;
          kj::StringPtr serviceName;
          kj::Maybe<kj::StringPtr> readEntrypoint;
          Persistent persistent = Persistent::NO;
          if (type == ChannelToken::Type::SUBREQUEST) {
            auto channel = f.handler.decodeSubrequestChannelToken(usage, rustToken)
                               .downcast<MockSubrequestChannel>();
            readProps = &channel->props;
            serviceName = channel->serviceName;
            readEntrypoint =
                channel->entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; });
            persistent = channel->persistent;
            keep = channel.toOwn();
          } else {
            auto channel = f.handler.decodeActorClassChannelToken(usage, rustToken)
                               .downcast<MockActorClassChannel>();
            readProps = &channel->props;
            serviceName = channel->serviceName;
            readEntrypoint =
                channel->entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; });
            persistent = channel->persistent;
            keep = channel.toOwn();
          }
          KJ_EXPECT(serviceName == "my-service");
          KJ_EXPECT(readEntrypoint == entrypoint);
          KJ_EXPECT(persistent == Persistent::YES);
          KJ_EXPECT(readProps->getCapTable().size() == props.getCapTable().size());
          for (auto i: kj::indices(props.getCapTable())) {
            auto& nested = KJ_ASSERT_NONNULL(
                kj::tryDowncast<MockSubrequestChannel>(*readProps->getCapTable()[i]));
            KJ_EXPECT(nested.serviceName == "nested-service");
            KJ_EXPECT(strProps(nested.props) ==
                strProps(Frankenvalue::fromJson(kj::str("{\"inner\": true}"))));
          }
          // Without caps, the props read back as they were written. (Caps read back as
          // different objects, so compare only their count above.)
          if (props.getCapTable().size() == 0) {
            KJ_EXPECT(strProps(*readProps) == strProps(props));
            KJ_EXPECT(strFlat(decoded.value) == strProps(props));
          }
        }
      }
    }
  }
}

KJ_TEST("actor tokens: Rust writes the handler's bytes, and each reads the other's") {
  Fixture f;
  const byte id[] = {12, 34, 56, 78};
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    for (auto persistent: {Persistent::NO, Persistent::YES}) {
      if (usage == Usage::STORAGE && !persistent) continue;
      for (auto name: {kj::Maybe<kj::StringPtr>("my-actor"_kj), kj::Maybe<kj::StringPtr>()}) {
        auto cppToken =
            f.handler.encodeActorChannelToken(usage, "my-namespace", id, name, persistent);
        auto rustPersistent =
            persistent ? codec::TokenPersistence::Yes : codec::TokenPersistence::No;

        auto decoded = f.rustDecode(usage, codec::TokenType::Subrequest, cppToken);
        KJ_EXPECT(decoded.kind == codec::DecodedKind::Actor);
        KJ_EXPECT(decoded.persistent == rustPersistent);
        KJ_EXPECT(str(decoded.name) == "my-namespace");
        KJ_EXPECT(copy(decoded.id).asPtr() == kj::arrayPtr(id));
        KJ_EXPECT(decoded.has_entrypoint == (name != kj::none));
        KJ_IF_SOME(n, name) {
          KJ_EXPECT(str(decoded.entrypoint) == n);
        }

        auto encode = [&](kj::ArrayPtr<const byte> iv) {
          return copy(codec::encode_actor_token(*f.key, rustUsage(usage), iv.as<kj_rs::Rust>(),
              rustPersistent, "my-namespace"_kj.asBytes().as<kj_rs::Rust>(),
              kj::arrayPtr(id).as<kj_rs::Rust>(), name.orDefault(""_kj).asBytes().as<kj_rs::Rust>(),
              name != kj::none));
        };
        KJ_EXPECT(encode(ivOf(usage, cppToken)).asPtr() == cppToken.asPtr(), usage, persistent);

        auto channel = f.handler.decodeSubrequestChannelToken(usage, encode(otherIv(usage)))
                           .downcast<MockActorChannel>();
        KJ_EXPECT(channel->namespaceKey == "my-namespace");
        KJ_EXPECT(channel->id.asPtr() == kj::arrayPtr(id));
        KJ_EXPECT(channel->name.map([](kj::String& s) -> kj::StringPtr { return s; }) == name);
        KJ_EXPECT(channel->persistent == persistent);
      }
    }
  }
}

KJ_TEST("restored tokens: Rust writes the handler's bytes, and each reads the other's") {
  Fixture f;
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    auto emptyProps = Frankenvalue::fromJson(kj::str("{}"));
    auto vendor = expectSync(f.handler.encodeSubrequestChannelToken(
        usage, "vendor", "Vendor"_kj, emptyProps, Persistent::YES));
    for (auto type: {ChannelToken::Type::SUBREQUEST, ChannelToken::Type::RPC}) {
      auto rustType = static_cast<codec::TokenType>(type);
      auto cases = propsCases(f.handler);
      for (auto argIndex: kj::indices(cases)) {
        auto& arg = cases[argIndex];
        auto cppToken = expectSync(f.handler.encodeRestoredChannelToken(
            usage, type, vendor.asPtr(), arg.clone(), Persistent::YES));

        auto decoded = f.rustDecode(usage, rustType, cppToken);
        KJ_EXPECT(decoded.kind == codec::DecodedKind::Restored);
        KJ_EXPECT(decoded.persistent == codec::TokenPersistence::Yes);
        KJ_EXPECT(copy(decoded.vendor).asPtr() == vendor.asPtr());
        KJ_ASSERT(decoded.has_value);
        if (arg.getCapTable().size() == 0) {
          KJ_EXPECT(strFlat(decoded.value) == strProps(arg));
        }

        auto encode = [&](kj::ArrayPtr<const byte> iv) {
          return copy(codec::encode_restored_token(*f.key, rustUsage(usage), iv.as<kj_rs::Rust>(),
              rustType, codec::TokenPersistence::Yes, vendor.as<kj_rs::Rust>(),
              kj::from<kj_rs::Rust>(decoded.value).as<kj_rs::Rust>()));
        };
        KJ_EXPECT(encode(ivOf(usage, cppToken)).asPtr() == cppToken.asPtr(), usage, type, argIndex);

        // The handler reads the Rust token.
        auto rustToken = encode(otherIv(usage));
        if (type == ChannelToken::Type::RPC) {
          f.handler.decodeRpcChannelToken(usage, rustToken);
        } else {
          f.handler.decodeSubrequestChannelToken(usage, rustToken);
        }
      }
    }
  }
}

KJ_TEST("restored channels the handler reads from Rust tokens re-encode the same contents") {
  Fixture f;
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    auto emptyProps = Frankenvalue::fromJson(kj::str("{}"));
    auto vendor = expectSync(f.handler.encodeSubrequestChannelToken(
        usage, "vendor", "Vendor"_kj, emptyProps, Persistent::YES));
    auto arg = Frankenvalue::fromJson(kj::str("{\"restore\": \"me\"}"));
    auto cppToken = expectSync(f.handler.encodeRestoredChannelToken(
        usage, ChannelToken::Type::SUBREQUEST, vendor.asPtr(), arg.clone(), Persistent::YES));
    auto decoded = f.rustDecode(usage, codec::TokenType::Subrequest, cppToken);
    auto rustToken = copy(
        codec::encode_restored_token(*f.key, rustUsage(usage), otherIv(usage).as<kj_rs::Rust>(),
            codec::TokenType::Subrequest, codec::TokenPersistence::Yes, vendor.as<kj_rs::Rust>(),
            kj::from<kj_rs::Rust>(decoded.value).as<kj_rs::Rust>()));

    auto channel = f.handler.decodeSubrequestChannelToken(usage, rustToken);
    auto again = expectSync(channel->getTokenMaybeSync(usage));
    auto redecoded = f.rustDecode(usage, codec::TokenType::Subrequest, again);
    KJ_EXPECT(redecoded.kind == codec::DecodedKind::Restored);
    KJ_EXPECT(strFlat(redecoded.value) == strProps(arg));
    // The vendor was decoded and encoded again, so compare what it points at.
    auto vendorDecoded = f.rustDecode(usage, codec::TokenType::Subrequest, copy(redecoded.vendor));
    KJ_EXPECT(str(vendorDecoded.name) == "vendor");
    KJ_EXPECT(str(vendorDecoded.entrypoint) == "Vendor");
  }
}

// The description of the exception that `func` throws.
template <typename Func>
kj::String failure(Func&& func) {
  KJ_IF_SOME(e, kj::runCatchingExceptions(kj::fwd<Func>(func))) {
    KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED, e);
    return kj::str(e.getDescription());
  }
  KJ_FAIL_EXPECT("expected an exception");
  return kj::str();
}

KJ_TEST("the handler and Rust reject the same broken tokens with the same errors") {
  Fixture f;
  MockResolver strangerResolver;
  ChannelTokenHandler stranger(strangerResolver, "another-cluster-key"_kj);
  auto strangerKey =
      codec::new_cluster_token_key("another-cluster-key"_kj.asBytes().as<kj_rs::Rust>());

  auto props = Frankenvalue::fromJson(kj::str("{\"foo\": 123}"));
  auto token = expectSync(f.handler.encodeSubrequestChannelToken(
      Usage::RPC, "my-service", "MyEntry"_kj, props, Persistent::NO));
  auto storageToken = expectSync(f.handler.encodeSubrequestChannelToken(
      Usage::STORAGE, "my-service", "MyEntry"_kj, props, Persistent::YES));

  auto changed = [&](kj::ArrayPtr<const byte> t, uint index) {
    auto copy = kj::heapArray(t);
    copy[index] ^= 1;
    return copy;
  };

  struct Case {
    kj::StringPtr what;
    Usage usage;
    ChannelToken::Type type;
    kj::Array<byte> token;
    kj::StringPtr expected;
  };
  auto cases = kj::arr(Case{"changed magic", Usage::RPC, ChannelToken::Type::SUBREQUEST,
                         changed(token, 2), "RPC_TOKEN_MAGIC"_kj},
      Case{"changed IV", Usage::RPC, ChannelToken::Type::SUBREQUEST, changed(token, 7),
        "channel token failed authentication"_kj},
      Case{"another key ID", Usage::RPC, ChannelToken::Type::SUBREQUEST, changed(token, 20),
        "channel token failed authentication"_kj},
      Case{"changed ciphertext", Usage::RPC, ChannelToken::Type::SUBREQUEST, changed(token, 37),
        "channel token failed authentication"_kj},
      Case{"changed MAC", Usage::RPC, ChannelToken::Type::SUBREQUEST,
        changed(token, token.size() - 2), "channel token failed authentication"_kj},
      Case{"truncated by a byte", Usage::RPC, ChannelToken::Type::SUBREQUEST,
        kj::heapArray(token.first(token.size() - 1)), "channel token failed authentication"_kj},
      Case{"truncated to the header", Usage::RPC, ChannelToken::Type::SUBREQUEST,
        kj::heapArray(token.first(47)), "invalid channel token for RPC"_kj},
      Case{"empty", Usage::RPC, ChannelToken::Type::SUBREQUEST, kj::heapArray<byte>(0),
        "invalid channel token for RPC"_kj},
      Case{"wrong type", Usage::RPC, ChannelToken::Type::ACTOR_CLASS, kj::heapArray(token.asPtr()),
        "channel token type mismatch"_kj},
      Case{"RPC token as storage", Usage::STORAGE, ChannelToken::Type::SUBREQUEST,
        kj::heapArray(token.asPtr()), "STORAGE_TOKEN_MAGIC"_kj},
      Case{"storage token as RPC", Usage::RPC, ChannelToken::Type::SUBREQUEST,
        kj::heapArray(storageToken.asPtr()), "RPC_TOKEN_MAGIC"_kj},
      Case{"storage, changed magic", Usage::STORAGE, ChannelToken::Type::SUBREQUEST,
        changed(storageToken, 1), "STORAGE_TOKEN_MAGIC"_kj},
      Case{"storage, truncated magic", Usage::STORAGE, ChannelToken::Type::SUBREQUEST,
        kj::heapArray(storageToken.first(3)), "invalid channel token for storage"_kj});

  for (auto& c: cases) {
    auto cppError = failure([&]() {
      switch (c.type) {
        case ChannelToken::Type::SUBREQUEST:
          f.handler.decodeSubrequestChannelToken(c.usage, c.token);
          break;
        case ChannelToken::Type::ACTOR_CLASS:
          f.handler.decodeActorClassChannelToken(c.usage, c.token);
          break;
        case ChannelToken::Type::RPC:
          f.handler.decodeRpcChannelToken(c.usage, c.token);
          break;
      }
    });
    auto rustError =
        failure([&]() { f.rustDecode(c.usage, static_cast<codec::TokenType>(c.type), c.token); });
    KJ_EXPECT(cppError.contains(c.expected), c.what, cppError);
    KJ_EXPECT(rustError.contains(c.expected), c.what, rustError);
  }

  // Another cluster's key fails authentication in both.
  KJ_EXPECT(failure([&]() {
    stranger.decodeSubrequestChannelToken(Usage::RPC, token);
  }).contains("failed authentication"));
  KJ_EXPECT(failure([&]() {
    codec::decode_token(*strangerKey, codec::TokenUsage::Rpc, codec::TokenType::Subrequest,
        token.as<kj_rs::Rust>());
  }).contains("failed authentication"));
}

}  // namespace
}  // namespace workerd::server
