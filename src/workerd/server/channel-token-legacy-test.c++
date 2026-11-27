// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The handler against `LegacyCodec`, a copy of the C++ encoding and decoding of channel tokens
// that the Rust codec (channel-token-codec/) replaced: the key derivation and AES-256-GCM envelope
// in BoringSSL, and the `ChannelToken` message built and read by capnp-cpp. Workers of earlier
// versions run that code, so it is kept here, unchanged, as the reference.
//
// For every shape of token, including those too large for one segment of their message:
// - the legacy code reads the handler's token as the same message it would write itself;
// - the handler reads the legacy code's token as written;
// - a token whose message fits in one segment is the same bytes as the legacy code's.
//
// A larger token can be different bytes, as capnp-rust and capnp-cpp size and fill the segments
// after the first differently, which can show once a message has three or more segments; its
// message reads the same.
//
// This file uses only the handler's public interface, so it also builds against an earlier
// version of the handler, which checks `LegacyCodec` against the code it copies.

#include "channel-token.h"

#include <workerd/server/channel-token.capnp.h>
#include <workerd/util/entropy.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

using Usage = IoChannelFactory::ChannelTokenUsage;

constexpr auto CLUSTER_KEY = "channel-token-legacy-test-cluster-key"_kj;

kj::Array<byte> expectSync(kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> variant) {
  return KJ_ASSERT_NONNULL(kj::mv(variant).tryGet<kj::Array<byte>>());
}

// =======================================================================================
// The legacy code: the token envelope and message of channel-token.c++ before the Rust codec,
// for the synchronous case.

class LegacyCodec {
 public:
  explicit LegacyCodec(kj::StringPtr clusterKey) {
    auto label = "workerd channel token v1"_kj;
    uint outLen = 0;
    KJ_ASSERT(HMAC(EVP_sha256(), label.begin(), label.size(), clusterKey.asBytes().begin(),
                  clusterKey.size(), tokenKey, &outLen) != nullptr);
    KJ_ASSERT(outLen == sizeof(tokenKey));

    SHA256_CTX ctx{};
    KJ_ASSERT(SHA256_Init(&ctx));
    KJ_ASSERT(SHA256_Update(&ctx, tokenKey, sizeof(tokenKey)));
    byte hash[SHA256_DIGEST_LENGTH]{};
    KJ_ASSERT(SHA256_Final(hash, &ctx));
    kj::arrayPtr(keyId).copyFrom(kj::arrayPtr(hash).first(KEY_ID_SIZE));
  }

  // A message as the legacy code builds it, with the first segment it gives it.
  static kj::Own<capnp::MallocMessageBuilder> newMessage() {
    return kj::heap<capnp::MallocMessageBuilder>(128);
  }

  static void buildService(capnp::MessageBuilder& message,
      ChannelToken::Type type,
      Usage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props,
      Persistent persistent) {
    auto builder = message.getRoot<ChannelToken>();
    builder.setType(type);
    encodePersistent(builder, persistent);
    auto service = builder.getService();
    service.setName(serviceName);
    KJ_IF_SOME(e, entrypoint) {
      service.setEntrypoint(e);
    }
    encodeFrankenvalue(usage, props, service.initProps());
  }

  static void buildActor(capnp::MessageBuilder& message,
      kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent) {
    auto builder = message.getRoot<ChannelToken>();
    builder.setType(ChannelToken::Type::SUBREQUEST);
    encodePersistent(builder, persistent);
    auto actor = builder.initActor();
    actor.setNamespaceKey(namespaceKey);
    actor.setId(id);
    KJ_IF_SOME(n, name) {
      actor.setName(n);
    }
  }

  static void buildRestored(capnp::MessageBuilder& message,
      ChannelToken::Type type,
      Usage usage,
      kj::ArrayPtr<const byte> vendorToken,
      Frankenvalue& restoreArg,
      Persistent persistent) {
    auto builder = message.getRoot<ChannelToken>();
    builder.setType(type);
    encodePersistent(builder, persistent);
    auto restored = builder.initRestored();
    restored.setVendor(vendorToken);
    encodeFrankenvalue(usage, restoreArg, restored.initRestoreArg());
  }

  static kj::Array<byte> pack(capnp::MessageBuilder& message) {
    kj::VectorOutputStream out;
    capnp::writePackedMessage(out, message);
    return kj::heapArray(out.getArray());
  }

  kj::Array<byte> serialize(Usage usage, capnp::MessageBuilder& message) {
    auto plaintext = pack(message);

    switch (usage) {
      case Usage::RPC: {
        auto result = kj::heapArray<byte>(sizeof(TokenHeader) + plaintext.size() + AES_MAC_SIZE);
        auto& header = *reinterpret_cast<TokenHeader*>(result.begin());

        header.magic = ChannelToken::RPC_TOKEN_MAGIC;
        getEntropy(header.iv);
        kj::arrayPtr(header.keyId).copyFrom(keyId);

        EVP_CIPHER_CTX* aesCtx = EVP_CIPHER_CTX_new();
        KJ_ASSERT(aesCtx != nullptr);
        KJ_DEFER(EVP_CIPHER_CTX_free(aesCtx));

        KJ_ASSERT(EVP_EncryptInit(aesCtx, EVP_aes_256_gcm(), tokenKey, header.iv));
        {
          int outSize = 0;
          KJ_ASSERT(
              EVP_EncryptUpdate(aesCtx, nullptr, &outSize, result.begin(), sizeof(TokenHeader)));
          KJ_ASSERT(outSize == sizeof(TokenHeader));
        }
        {
          int outSize = 0;
          KJ_ASSERT(EVP_EncryptUpdate(aesCtx, result.begin() + sizeof(TokenHeader), &outSize,
              plaintext.begin(), plaintext.size()));
          KJ_ASSERT(outSize == plaintext.size());
        }
        int out = 0;
        KJ_ASSERT(EVP_EncryptFinal_ex(aesCtx, nullptr, &out));
        KJ_ASSERT(out == 0);
        KJ_ASSERT(EVP_CIPHER_CTX_ctrl(
            aesCtx, EVP_CTRL_GCM_GET_TAG, AES_MAC_SIZE, result.end() - AES_MAC_SIZE));
        return result;
      }

      case Usage::STORAGE: {
        auto magic = kj::asBytes(ChannelToken::STORAGE_TOKEN_MAGIC);
        auto result = kj::heapArray<byte>(magic.size() + plaintext.size());
        result.first(magic.size()).copyFrom(magic);
        result.slice(magic.size()).copyFrom(plaintext);
        return result;
      }
    }
    KJ_UNREACHABLE;
  }

  // The packed message of a token, which the legacy code then reads with
  // `capnp::PackedMessageReader`.
  kj::Array<byte> plaintext(Usage usage, kj::ArrayPtr<const byte> token) {
    switch (usage) {
      case Usage::RPC: {
        TokenHeader header;
        KJ_REQUIRE(token.size() >= sizeof(header) + AES_MAC_SIZE, "invalid channel token for RPC");
        kj::asBytes(header).copyFrom(token.first(sizeof(TokenHeader)));
        KJ_REQUIRE(header.magic == ChannelToken::RPC_TOKEN_MAGIC, "invalid channel token for RPC");

        auto mac = token.slice(token.size() - AES_MAC_SIZE);
        auto ciphertext = token.slice(sizeof(header), token.size() - AES_MAC_SIZE);

        EVP_CIPHER_CTX* aesCtx = EVP_CIPHER_CTX_new();
        KJ_ASSERT(aesCtx != nullptr);
        KJ_DEFER(EVP_CIPHER_CTX_free(aesCtx));

        KJ_ASSERT(EVP_DecryptInit(aesCtx, EVP_aes_256_gcm(), tokenKey, header.iv));
        {
          int outSize = 0;
          KJ_ASSERT(EVP_DecryptUpdate(aesCtx, nullptr, &outSize, token.begin(), sizeof(header)));
          KJ_ASSERT(outSize == sizeof(TokenHeader));
        }
        auto result = kj::heapArray<byte>(ciphertext.size());
        {
          int outSize = 0;
          KJ_ASSERT(EVP_DecryptUpdate(
              aesCtx, result.begin(), &outSize, ciphertext.begin(), ciphertext.size()));
          KJ_ASSERT(outSize == result.size());
        }
        KJ_ASSERT(EVP_CIPHER_CTX_ctrl(
            aesCtx, EVP_CTRL_GCM_SET_TAG, AES_MAC_SIZE, const_cast<byte*>(mac.begin())));
        int out = 0;
        KJ_REQUIRE(
            EVP_DecryptFinal_ex(aesCtx, nullptr, &out), "channel token failed authentication");
        KJ_ASSERT(out == 0);
        return result;
      }

      case Usage::STORAGE: {
        uint32_t magic = 0;
        KJ_REQUIRE(token.size() >= sizeof(magic), "invalid channel token for storage");
        kj::asBytes(magic).copyFrom(token.first(sizeof(magic)));
        KJ_REQUIRE(magic == ChannelToken::STORAGE_TOKEN_MAGIC, "invalid channel token for storage");
        return kj::heapArray(token.slice(sizeof(magic)));
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  static constexpr uint AES_KEY_SIZE = 32;
  static constexpr uint AES_IV_SIZE = 12;
  static constexpr uint AES_MAC_SIZE = 16;
  static constexpr uint KEY_ID_SIZE = 16;

  struct TokenHeader {
    uint32_t magic;
    byte iv[AES_IV_SIZE];
    byte keyId[KEY_ID_SIZE];
  };
  static_assert(sizeof(TokenHeader) == 32);

  byte tokenKey[AES_KEY_SIZE]{};
  byte keyId[KEY_ID_SIZE]{};

  static void encodePersistent(ChannelToken::Builder builder, Persistent persistent) {
    if (persistent.toBool()) {
      builder.getPersistence().setIrrevocable();
    } else {
      builder.getPersistence().setNone();
    }
  }

  static void encodeFrankenvalue(
      Usage usage, Frankenvalue& value, rpc::Frankenvalue::Builder valueBuilder) {
    value.toCapnp(valueBuilder);
    auto capTable = value.getCapTable();
    if (capTable.size() > 0) {
      auto tableBuilder = valueBuilder.initCapTable().initAs<ChannelToken::FrankenvalueCapTable>();
      auto caps = tableBuilder.initCaps(capTable.size());
      for (auto i: kj::indices(capTable)) {
        auto& subreq =
            KJ_ASSERT_NONNULL(kj::tryDowncast<IoChannelFactory::SubrequestChannel>(*capTable[i]));
        caps[i].setSubrequestChannel(expectSync(subreq.getTokenMaybeSync(usage)));
      }
    }
  }
};

// The message of a packed token, as text: two messages with the same text read the same.
kj::String messageText(kj::ArrayPtr<const byte> packed) {
  kj::ArrayInputStream input(packed);
  capnp::PackedMessageReader reader(input);
  return kj::str(reader.getRoot<ChannelToken>());
}

kj::String messageText(capnp::MessageBuilder& message) {
  return kj::str(message.getRoot<ChannelToken>().asReader());
}

// =======================================================================================
// Mocks

kj::String strProps(Frankenvalue& props) {
  capnp::MallocMessageBuilder message;
  props.clone().toCapnp(message.getRoot<rpc::Frankenvalue>());
  return kj::str(message.getRoot<rpc::Frankenvalue>().asReader());
}

// A subrequest channel, nested in props or resolved from a token. It encodes its token with
// `handler` once per usage and returns the same bytes after that, so that the legacy code and the
// handler embed the same nested token.
class MockSubrequestChannel final: public IoChannelFactory::SubrequestChannel {
 public:
  MockSubrequestChannel(kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue props,
      Persistent persistent,
      kj::Maybe<ChannelTokenHandler&> handler)
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
  kj::Maybe<kj::Array<byte>> tokens[2];

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_UNREACHABLE;
  }
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    auto& slot = tokens[usage == Usage::RPC ? 0 : 1];
    KJ_IF_SOME(token, slot) {
      return kj::heapArray(token.asPtr());
    }
    auto token =
        expectSync(KJ_ASSERT_NONNULL(handler).encodeSubrequestChannelToken(usage, serviceName,
            entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }), props, persistent));
    auto result = kj::heapArray(token.asPtr());
    slot = kj::mv(token);
    return result;
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

  void requireAllowsTransfer() override {}
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
  void requireAllowsTransfer() override {}
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(Usage usage) override {
    KJ_UNREACHABLE;
  }
};

class MockResolver final: public ChannelTokenHandler::Resolver {
 public:
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
  LegacyCodec legacy{CLUSTER_KEY};

  // How many tokens of each segment count the checks saw, so that a test can require that it
  // covered tokens of more than one segment.
  size_t bySegments[4]{};

  // Checks the handler's token against the legacy code's message for the same contents.
  void check(kj::StringPtr what,
      Usage usage,
      kj::ArrayPtr<const byte> token,
      capnp::MallocMessageBuilder& legacyMessage) {
    auto plaintext = legacy.plaintext(usage, token);
    KJ_EXPECT(messageText(plaintext) == messageText(legacyMessage), what, usage);

    auto segments = legacyMessage.getSegmentsForOutput().size();
    ++bySegments[kj::min(segments, static_cast<size_t>(3))];
    if (segments == 1) {
      KJ_EXPECT(plaintext.asPtr() == LegacyCodec::pack(legacyMessage).asPtr(), what, usage);
    }
  }
};

// =======================================================================================
// Shapes of tokens

kj::String repeat(char c, size_t n) {
  auto result = kj::heapString(n);
  for (auto& ch: result) ch = c;
  return result;
}

kj::Array<byte> pattern(size_t n) {
  auto result = kj::heapArray<byte>(n);
  for (auto i: kj::indices(result)) result[i] = static_cast<byte>(i * 7 + 3);
  return result;
}

Frankenvalue v8Serialized(size_t n) {
  capnp::MallocMessageBuilder message;
  auto builder = message.getRoot<rpc::Frankenvalue>();
  builder.setV8Serialized(pattern(n));
  return Frankenvalue::fromCapnp(builder.asReader(), {});
}

kj::Own<MockSubrequestChannel> nested(
    ChannelTokenHandler& handler, kj::StringPtr name, Frankenvalue props) {
  return kj::refcounted<MockSubrequestChannel>(
      name, "NestedEntry"_kj, kj::mv(props), Persistent::YES, handler);
}

struct Shape {
  kj::StringPtr name;
  Frankenvalue value;
};

// Props, and restore arguments, of each shape, some too large for one segment.
kj::Array<Shape> shapes(ChannelTokenHandler& handler) {
  kj::Vector<Shape> result;

  result.add(Shape{"empty", Frankenvalue()});
  result.add(Shape{"json", Frankenvalue::fromJson(kj::str("{\"foo\": 123}"))});
  result.add(Shape{"v8-serialized", v8Serialized(64)});
  result.add(Shape{"array-buffer", Frankenvalue::fromBytes(pattern(16))});
  result.add(Shape{"wrapped-capability",
    Frankenvalue::fromWrappedCapability(7,
        nested(handler, "wrapped", Frankenvalue::fromJson(kj::str("{}"))),
        kj::str("my-wrapper-module"))});

  {
    auto value = Frankenvalue::fromJson(kj::str("{\"a\": 1}"));
    auto outer = Frankenvalue::fromJson(kj::str("{\"b\": 2}"));
    outer.setProperty(kj::str("inner"), Frankenvalue::fromJson(kj::str("[1]")));
    value.setProperty(kj::str("outer"), kj::mv(outer));
    value.setProperty(kj::str("plain"), Frankenvalue::fromJson(kj::str("\"text\"")));
    result.add(Shape{"nested-properties", kj::mv(value)});
  }

  {
    auto value = Frankenvalue::fromJson(kj::str("{\"a\": 1}"));
    value.setProperty(kj::str("nested"),
        Frankenvalue::fromCapability(
            7, nested(handler, "nested-service", Frankenvalue::fromJson(kj::str("{\"x\": 1}")))));
    value.setProperty(kj::str("plain"), Frankenvalue::fromJson(kj::str("[1, 2, 3]")));
    result.add(Shape{"caps", kj::mv(value)});
  }

  // Past the first segment.
  result.add(Shape{"large-json", Frankenvalue::fromJson(kj::str('"', repeat('j', 3000), '"'))});
  result.add(Shape{"large-v8-serialized", v8Serialized(6000)});
  result.add(Shape{"large-array-buffer", Frankenvalue::fromBytes(pattern(3000))});

  // About 1 KiB of JSON and a 1 KiB nested token: the case where capnp-rust puts the token in
  // the second segment and capnp-cpp starts a third.
  {
    auto value = Frankenvalue::fromJson(kj::str('"', repeat('k', 1000), '"'));
    value.setProperty(kj::str("nested"),
        Frankenvalue::fromCapability(7,
            nested(handler, "nested-service",
                Frankenvalue::fromJson(kj::str('"', repeat('n', 900), '"')))));
    result.add(Shape{"large-json-and-nested-token", kj::mv(value)});
  }

  // Many properties and nested tokens of various sizes, over several segments.
  {
    auto value = Frankenvalue::fromJson(kj::str("{}"));
    for (auto i: kj::range(0, 12)) {
      auto size = 50 + i * 211;
      if (i % 3 == 0) {
        value.setProperty(kj::str("cap", i),
            Frankenvalue::fromCapability(7,
                nested(handler, kj::str("nested-", i),
                    Frankenvalue::fromJson(
                        kj::str('"', repeat(static_cast<char>('a' + i), size), '"')))));
      } else if (i % 3 == 1) {
        value.setProperty(kj::str("bytes", i), Frankenvalue::fromBytes(pattern(size)));
      } else {
        value.setProperty(kj::str("json", i),
            Frankenvalue::fromJson(kj::str('"', repeat(static_cast<char>('a' + i), size), '"')));
      }
    }
    result.add(Shape{"many-segments", kj::mv(value)});
  }

  return result.releaseAsArray();
}

// =======================================================================================
// Tests

KJ_TEST("service tokens: the legacy code and the handler read each other's") {
  Fixture f;
  auto longName = repeat('s', 2000);
  auto longEntrypoint = repeat('e', 2000);
  struct Target {
    kj::StringPtr name;
    kj::Maybe<kj::StringPtr> entrypoint;
  };
  auto targets = kj::arr(Target{"my-service"_kj, "MyEntry"_kj}, Target{"my-service"_kj, kj::none},
      Target{longName, longEntrypoint.asPtr()});

  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    for (auto type: {ChannelToken::Type::SUBREQUEST, ChannelToken::Type::ACTOR_CLASS}) {
      for (auto& target: targets) {
        for (auto& shape: shapes(f.handler)) {
          auto what = kj::str(shape.name, " ", type, " ", target.name.size());
          auto token = expectSync(type == ChannelToken::Type::SUBREQUEST
                  ? f.handler.encodeSubrequestChannelToken(
                        usage, target.name, target.entrypoint, shape.value, Persistent::YES)
                  : f.handler.encodeActorClassChannelToken(
                        usage, target.name, target.entrypoint, shape.value, Persistent::YES));

          auto message = LegacyCodec::newMessage();
          LegacyCodec::buildService(
              *message, type, usage, target.name, target.entrypoint, shape.value, Persistent::YES);
          f.check(what, usage, token, *message);

          auto legacyToken = f.legacy.serialize(usage, *message);
          Frankenvalue* props;
          kj::Own<void> keep;
          if (type == ChannelToken::Type::SUBREQUEST) {
            auto channel = f.handler.decodeSubrequestChannelToken(usage, legacyToken)
                               .downcast<MockSubrequestChannel>();
            KJ_EXPECT(channel->serviceName == target.name, what);
            KJ_EXPECT(channel->entrypoint.map([](kj::String& s) -> kj::StringPtr { return s; }) ==
                    target.entrypoint,
                what);
            props = &channel->props;
            keep = channel.toOwn();
          } else {
            auto channel = f.handler.decodeActorClassChannelToken(usage, legacyToken)
                               .downcast<MockActorClassChannel>();
            KJ_EXPECT(channel->serviceName == target.name, what);
            props = &channel->props;
            keep = channel.toOwn();
          }
          // The value and the size of the cap table read back as written. (The nested tokens are
          // compared above, as part of the message.)
          KJ_EXPECT(strProps(*props) == strProps(shape.value), what);
        }
      }
    }
  }

  // The shapes include tokens of one segment, of two, and of more.
  KJ_EXPECT(f.bySegments[1] > 0);
  KJ_EXPECT(f.bySegments[2] > 0);
  KJ_EXPECT(f.bySegments[3] > 0);
}

KJ_TEST("actor tokens: the legacy code and the handler read each other's") {
  Fixture f;
  auto longNamespace = repeat('n', 2000);
  auto longName = repeat('a', 3000);
  auto longId = pattern(1500);
  const byte shortId[] = {12, 34, 56, 78};
  struct Target {
    kj::StringPtr namespaceKey;
    kj::ArrayPtr<const byte> id;
    kj::Maybe<kj::StringPtr> name;
  };
  auto targets = kj::arr(Target{"my-namespace"_kj, shortId, "my-actor"_kj},
      Target{"my-namespace"_kj, shortId, kj::none},
      Target{longNamespace, longId, longName.asPtr()});

  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    for (auto persistent: {Persistent::NO, Persistent::YES}) {
      if (usage == Usage::STORAGE && !persistent) continue;
      for (auto& target: targets) {
        auto what = kj::str(target.namespaceKey.size(), " ", persistent.toBool());
        auto token = f.handler.encodeActorChannelToken(
            usage, target.namespaceKey, target.id, target.name, persistent);

        auto message = LegacyCodec::newMessage();
        LegacyCodec::buildActor(*message, target.namespaceKey, target.id, target.name, persistent);
        f.check(what, usage, token, *message);

        auto channel =
            f.handler.decodeSubrequestChannelToken(usage, f.legacy.serialize(usage, *message))
                .downcast<MockActorChannel>();
        KJ_EXPECT(channel->namespaceKey == target.namespaceKey, what);
        KJ_EXPECT(channel->id.asPtr() == target.id, what);
        KJ_EXPECT(
            channel->name.map([](kj::String& s) -> kj::StringPtr { return s; }) == target.name,
            what);
        KJ_EXPECT(
            channel->persistent == (usage == Usage::STORAGE ? Persistent::YES : persistent), what);
      }
    }
  }

  KJ_EXPECT(f.bySegments[1] > 0);
  KJ_EXPECT(f.bySegments[2] + f.bySegments[3] > 0);
}

KJ_TEST("restored tokens: the legacy code and the handler read each other's") {
  Fixture f;
  for (auto usage: {Usage::RPC, Usage::STORAGE}) {
    auto smallVendorProps = Frankenvalue::fromJson(kj::str("{}"));
    auto largeVendorProps = Frankenvalue::fromJson(kj::str('"', repeat('v', 1500), '"'));
    auto smallVendor = expectSync(f.handler.encodeSubrequestChannelToken(
        usage, "vendor", "Vendor"_kj, smallVendorProps, Persistent::YES));
    auto largeVendor = expectSync(f.handler.encodeSubrequestChannelToken(
        usage, "vendor", "Vendor"_kj, largeVendorProps, Persistent::YES));

    for (auto type: {ChannelToken::Type::SUBREQUEST, ChannelToken::Type::RPC}) {
      for (kj::ArrayPtr<const byte> vendor: kj::arr(smallVendor.asPtr(), largeVendor.asPtr())) {
        for (auto& shape: shapes(f.handler)) {
          auto what = kj::str(shape.name, " ", type, " ", vendor.size());
          auto token = expectSync(f.handler.encodeRestoredChannelToken(
              usage, type, vendor, shape.value.clone(), Persistent::YES));

          auto message = LegacyCodec::newMessage();
          LegacyCodec::buildRestored(*message, type, usage, vendor, shape.value, Persistent::YES);
          f.check(what, usage, token, *message);

          // A restored channel the handler read from the legacy token writes it again with the
          // same contents: the vendor channel it read encodes the same vendor token as before,
          // as long as the vendor's encoding is deterministic (storage).
          auto legacyToken = f.legacy.serialize(usage, *message);
          kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> again = kj::Array<byte>();
          if (type == ChannelToken::Type::RPC) {
            again = f.handler.decodeRpcChannelToken(usage, legacyToken)->getTokenMaybeSync(usage);
          } else {
            again = f.handler.decodeSubrequestChannelToken(usage, legacyToken)
                        ->getTokenMaybeSync(usage);
          }
          auto againToken = expectSync(kj::mv(again));
          if (usage == Usage::STORAGE) {
            KJ_EXPECT(
                messageText(f.legacy.plaintext(usage, againToken)) == messageText(*message), what);
          }
        }
      }
    }
  }

  KJ_EXPECT(f.bySegments[1] > 0);
  KJ_EXPECT(f.bySegments[2] > 0);
  KJ_EXPECT(f.bySegments[3] > 0);
}

}  // namespace
}  // namespace workerd::server
