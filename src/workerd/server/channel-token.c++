// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "channel-token.h"

#include <workerd/server/channel-token.capnp.h>
#include <workerd/util/entropy.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <capnp/serialize-packed.h>
#include <kj/io.h>

// It's 2025, nobody uses big-endian anymore. But just in case someone tries, flag it here.
// Specifically, the magic number in TokenHeader is encoded in host order.
#if !defined(__LITTLE_ENDIAN__) || __BYTE_ORDER != __LITTLE_ENDIAN
#error "This code assumes little-endian architecture."
#endif

namespace workerd::server {

ChannelTokenHandler::ChannelTokenHandler(Resolver& resolver): resolver(resolver) {
  getEntropy(tokenKey);

  SHA256_CTX ctx{};
  KJ_ASSERT(SHA256_Init(&ctx));
  KJ_ASSERT(SHA256_Update(&ctx, tokenKey, sizeof(tokenKey)));

  byte hash[SHA256_DIGEST_LENGTH]{};
  KJ_ASSERT(SHA256_Final(hash, &ctx));

  static_assert(KEY_ID_SIZE <= SHA256_DIGEST_LENGTH);
  kj::arrayPtr(keyId).copyFrom(kj::arrayPtr(hash).first(KEY_ID_SIZE));
}

kj::Array<byte> ChannelTokenHandler::encodeChannelTokenImpl(ChannelToken::Type type,
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    Frankenvalue& props) {
  capnp::word scratch[128]{};
  capnp::MallocMessageBuilder message(scratch);
  auto builder = message.getRoot<ChannelToken>();

  builder.setType(type);

  builder.setName(serviceName);

  KJ_IF_SOME(e, entrypoint) {
    builder.setEntrypoint(e);
  }

  {
    auto propsBuilder = builder.initProps();
    props.toCapnp(propsBuilder);

    auto capTable = props.getCapTable();
    if (capTable.size() > 0) {
      auto tableBuilder = propsBuilder.initCapTable().initAs<ChannelToken::FrankenvalueCapTable>();

      auto caps = tableBuilder.initCaps(capTable.size());

      for (auto i: kj::indices(capTable)) {
        KJ_IF_SOME(subreq, kj::tryDowncast<IoChannelFactory::SubrequestChannel>(*capTable[i])) {
          caps[i].setSubrequestChannel(subreq.getToken(usage));
        } else KJ_IF_SOME(actorClass,
            kj::tryDowncast<IoChannelFactory::ActorClassChannel>(*capTable[i])) {
          caps[i].setActorClassChannel(actorClass.getToken(usage));
        } else {
          KJ_FAIL_REQUIRE("unknown type in props");
        }
      }
    }
  }

  kj::VectorOutputStream out;
  capnp::writePackedMessage(out, message);

  auto plaintext = out.getArray();

  switch (usage) {
    case IoChannelFactory::ChannelTokenUsage::RPC: {
      static_assert(alignof(TokenHeader) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
      auto result = kj::heapArray<byte>(sizeof(TokenHeader) + plaintext.size() + AES_MAC_SIZE);
      auto& header = *reinterpret_cast<TokenHeader*>(result.begin());

      header.magic = ChannelToken::RPC_TOKEN_MAGIC;
      getEntropy(header.iv);
      kj::arrayPtr(header.keyId).copyFrom(keyId);

      EVP_CIPHER_CTX* aesCtx = EVP_CIPHER_CTX_new();
      KJ_ASSERT(aesCtx != nullptr);
      KJ_DEFER(EVP_CIPHER_CTX_free(aesCtx));

      KJ_ASSERT(EVP_EncryptInit(aesCtx, EVP_aes_256_gcm(), tokenKey, header.iv));

      // Add header as AAD first.
      {
        int outSize = 0;
        KJ_ASSERT(
            EVP_EncryptUpdate(aesCtx, nullptr, &outSize, result.begin(), sizeof(TokenHeader)));
        KJ_ASSERT(outSize == sizeof(TokenHeader));
      }

      // Encrypt the body.
      {
        int outSize = 0;
        KJ_ASSERT(EVP_EncryptUpdate(aesCtx, result.begin() + sizeof(TokenHeader), &outSize,
            plaintext.begin(), plaintext.size()));
        KJ_ASSERT(outSize == plaintext.size());  // because AES-GCM is a stream cipher
      }

      int out = 0;
      KJ_ASSERT(EVP_EncryptFinal_ex(aesCtx, nullptr, &out));
      KJ_ASSERT(out == 0);  // No padding for stream ciphers like AES-GCM.

      // Get the MAC.
      KJ_ASSERT(EVP_CIPHER_CTX_ctrl(
          aesCtx, EVP_CTRL_GCM_GET_TAG, AES_MAC_SIZE, result.end() - AES_MAC_SIZE));

      return result;
    }

    case IoChannelFactory::ChannelTokenUsage::STORAGE: {
      auto magic = kj::asBytes(ChannelToken::STORAGE_TOKEN_MAGIC);
      auto result = kj::heapArray<byte>(magic.size() + plaintext.size());
      result.slice(0, magic.size()).copyFrom(magic);
      result.slice(magic.size()).copyFrom(plaintext);
      return result;
    }
  }

  KJ_UNREACHABLE;
}

kj::Array<byte> ChannelTokenHandler::encodeSubrequestChannelToken(
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    Frankenvalue& props) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::SUBREQUEST, usage, serviceName, entrypoint, props);
}

kj::Array<byte> ChannelTokenHandler::encodeActorClassChannelToken(
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    Frankenvalue& props) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::ACTOR_CLASS, usage, serviceName, entrypoint, props);
}

kj::Own<Frankenvalue::CapTableEntry> ChannelTokenHandler::decodeChannelTokenImpl(
    ChannelToken::Type type,
    IoChannelFactory::ChannelTokenUsage usage,
    kj::ArrayPtr<const byte> token) {
  kj::ArrayPtr<const byte> plaintext;
  kj::Array<byte> ownPlaintext;

  switch (usage) {
    case IoChannelFactory::ChannelTokenUsage::RPC: {
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

      // Add header as AAD first.
      {
        int outSize = 0;
        KJ_ASSERT(EVP_DecryptUpdate(aesCtx, nullptr, &outSize, token.begin(), sizeof(header)));
        KJ_ASSERT(outSize == sizeof(TokenHeader));
      }

      // Decrypt the body.
      ownPlaintext = kj::heapArray<byte>(ciphertext.size());
      {
        int outSize = 0;
        KJ_ASSERT(EVP_DecryptUpdate(
            aesCtx, ownPlaintext.begin(), &outSize, ciphertext.begin(), ciphertext.size()));
        KJ_ASSERT(outSize == ownPlaintext.size());
      }
      plaintext = ownPlaintext;

      // Check MAC.
      KJ_ASSERT(EVP_CIPHER_CTX_ctrl(aesCtx, EVP_CTRL_GCM_SET_TAG, AES_MAC_SIZE,
          // const_cast needed for the EVP_CIPHER_CTX_ctrl() interface, but this won't actually
          // modify the buffer.
          const_cast<byte*>(mac.begin())));

      int out;
      KJ_REQUIRE(EVP_DecryptFinal_ex(aesCtx, nullptr, &out), "channel token failed authentication");
      KJ_ASSERT(out == 0);

      break;
    }

    case IoChannelFactory::ChannelTokenUsage::STORAGE: {
      uint32_t magic;
      KJ_REQUIRE(token.size() >= sizeof(magic), "invalid channel token for storage");

      kj::asBytes(magic).copyFrom(token.first(sizeof(magic)));
      KJ_REQUIRE(magic == ChannelToken::STORAGE_TOKEN_MAGIC, "invalid channel token for storage");

      plaintext = token.slice(sizeof(magic));
      break;
    }
  }

  kj::ArrayInputStream input(plaintext);
  capnp::word scratch[128]{};
  capnp::PackedMessageReader message(input, {}, scratch);
  auto reader = message.getRoot<ChannelToken>();

  KJ_REQUIRE(reader.getType() == type, "channel token type mismatch");

  kj::Maybe<kj::StringPtr> entrypoint;
  if (reader.hasEntrypoint()) {
    entrypoint = reader.getEntrypoint();
  }

  Frankenvalue props;
  if (reader.hasProps()) {
    auto propsReader = reader.getProps();
    auto tableReader = propsReader.getCapTable().getAs<ChannelToken::FrankenvalueCapTable>();

    kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> capTable;
    if (tableReader.hasCaps()) {
      auto caps = tableReader.getCaps();
      capTable.reserve(caps.size());

      for (auto cap: caps) {
        switch (cap.which()) {
          case ChannelToken::FrankenvalueCapTable::Cap::UNKNOWN:
            break;
          case ChannelToken::FrankenvalueCapTable::Cap::SUBREQUEST_CHANNEL:
            capTable.add(decodeSubrequestChannelToken(usage, cap.getSubrequestChannel()));
            continue;
          case ChannelToken::FrankenvalueCapTable::Cap::ACTOR_CLASS_CHANNEL:
            capTable.add(decodeActorClassChannelToken(usage, cap.getActorClassChannel()));
            continue;
        }
        KJ_FAIL_REQUIRE("unknown cap table type", cap.which());
      }
    }

    props = Frankenvalue::fromCapnp(propsReader, kj::mv(capTable));
  }

  // HACK: It would be more type-safe for us to return the (name, entrypoint, props) triplet and
  //   let the caller call the appropriate resolver method. However, this would require making
  //   heap string copies of the name and entrypoint which would just be thrown way immediately.
  //   Since both types happen to subclass Frankenvalue::CapTableEntry, we just make the resolver
  //   call here, return either type, and let the caller downcast to the right type.
  switch (type) {
    case ChannelToken::Type::SUBREQUEST:
      return resolver.resolveEntrypoint(reader.getName(), entrypoint, kj::mv(props));
    case ChannelToken::Type::ACTOR_CLASS:
      return resolver.resolveActorClass(reader.getName(), entrypoint, kj::mv(props));
  }

  KJ_UNREACHABLE;
}

kj::Own<IoChannelFactory::SubrequestChannel> ChannelTokenHandler::decodeSubrequestChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::SUBREQUEST, usage, token)
      .downcast<IoChannelFactory::SubrequestChannel>();
}

kj::Own<IoChannelFactory::ActorClassChannel> ChannelTokenHandler::decodeActorClassChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::ACTOR_CLASS, usage, token)
      .downcast<IoChannelFactory::ActorClassChannel>();
}

}  // namespace workerd::server
