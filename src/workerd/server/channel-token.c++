// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "channel-token.h"

#include <workerd/api/restore.h>
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

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeChannelTokenImpl(ChannelToken::Type type,
        IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& props) {
  auto message = kj::heap<capnp::MallocMessageBuilder>(128);
  auto builder = message->getRoot<ChannelToken>();
  kj::Vector<kj::Promise<void>> promises;

  builder.setType(type);

  auto service = builder.getService();
  service.setName(serviceName);

  KJ_IF_SOME(e, entrypoint) {
    service.setEntrypoint(e);
  }

  encodeFrankenvalue(usage, props, service.initProps(), promises);

  if (promises.empty()) {
    return serializeTokenImpl(usage, *message);
  } else {
    return kj::joinPromisesFailFast(promises.releaseAsArray())
        .then([this, usage, message = kj::mv(message)]() mutable {
      return serializeTokenImpl(usage, *message);
    });
  }
}

void ChannelTokenHandler::encodeFrankenvalue(IoChannelFactory::ChannelTokenUsage usage,
    Frankenvalue& value,
    rpc::Frankenvalue::Builder valueBuilder,
    kj::Vector<kj::Promise<void>>& promises) {
  value.toCapnp(valueBuilder);

  auto capTable = value.getCapTable();
  if (capTable.size() > 0) {
    auto tableBuilder = valueBuilder.initCapTable().initAs<ChannelToken::FrankenvalueCapTable>();

    auto caps = tableBuilder.initCaps(capTable.size());

    for (auto i: kj::indices(capTable)) {
      KJ_IF_SOME(subreq, kj::tryDowncast<IoChannelFactory::SubrequestChannel>(*capTable[i])) {
        KJ_SWITCH_ONEOF(subreq.getTokenMaybeSync(usage)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            caps[i].setSubrequestChannel(token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            promises.add(promise.then([slot = caps[i]](kj::Array<byte> token) mutable {
              slot.setSubrequestChannel(token);
            }));
          }
        }
      } else KJ_IF_SOME(actorClass,
          kj::tryDowncast<IoChannelFactory::ActorClassChannel>(*capTable[i])) {
        KJ_SWITCH_ONEOF(actorClass.getTokenMaybeSync(usage)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            caps[i].setActorClassChannel(token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            promises.add(promise.then([slot = caps[i]](kj::Array<byte> token) mutable {
              slot.setActorClassChannel(token);
            }));
          }
        }
      } else KJ_IF_SOME(rpcChannel, kj::tryDowncast<IoChannelFactory::RpcChannel>(*capTable[i])) {
        KJ_SWITCH_ONEOF(rpcChannel.getTokenMaybeSync(usage)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            caps[i].setRpcChannel(token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            promises.add(promise.then(
                [slot = caps[i]](kj::Array<byte> token) mutable { slot.setRpcChannel(token); }));
          }
        }
      } else {
        KJ_FAIL_REQUIRE("unknown type in props");
      }
    }
  }
}

kj::Array<byte> ChannelTokenHandler::serializeTokenImpl(
    IoChannelFactory::ChannelTokenUsage usage, capnp::MessageBuilder& message) {
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

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeSubrequestChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& props) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::SUBREQUEST, usage, serviceName, entrypoint, props);
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeActorClassChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& props) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::ACTOR_CLASS, usage, serviceName, entrypoint, props);
}

kj::Array<byte> ChannelTokenHandler::encodeActorChannelToken(
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr namespaceKey,
    kj::ArrayPtr<const byte> id,
    kj::Maybe<kj::StringPtr> name) {
  capnp::word scratch[128]{};
  capnp::MallocMessageBuilder message(scratch);
  auto builder = message.getRoot<ChannelToken>();
  builder.setType(ChannelToken::Type::SUBREQUEST);

  auto actor = builder.initActor();
  actor.setNamespaceKey(namespaceKey);
  actor.setId(id);
  KJ_IF_SOME(n, name) {
    actor.setName(n);
  }

  return serializeTokenImpl(usage, message);
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::ArrayPtr<const byte> vendorToken,
        Frankenvalue restoreArg) {
  auto message = kj::heap<capnp::MallocMessageBuilder>(128);
  auto builder = message->getRoot<ChannelToken>();
  builder.setType(type);

  auto restored = builder.initRestored();
  restored.setVendor(vendorToken);

  kj::Vector<kj::Promise<void>> promises;
  encodeFrankenvalue(usage, restoreArg, restored.initRestoreArg(), promises);

  if (promises.empty()) {
    return serializeTokenImpl(usage, *message);
  } else {
    return kj::joinPromisesFailFast(promises.releaseAsArray())
        .then([this, usage, message = kj::mv(message)]() mutable {
      return serializeTokenImpl(usage, *message);
    });
  }
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::Own<IoChannelFactory::SubrequestChannel> vendor,
        Frankenvalue restoreArg) {
  auto vendorTokenMaybeSync = vendor->getTokenMaybeSync(usage);
  return encodeRestoredChannelTokenImpl(
      usage, type, kj::mv(vendorTokenMaybeSync), kj::mv(vendor), kj::mv(restoreArg));
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::Own<ServerSelfTokenFactory> vendor,
        Frankenvalue restoreArg) {
  auto vendorTokenMaybeSync = vendor->getSelfToken(usage);
  return encodeRestoredChannelTokenImpl(
      usage, type, kj::mv(vendorTokenMaybeSync), kj::mv(vendor), kj::mv(restoreArg));
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelTokenImpl(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> vendorTokenMaybeSync,
        kj::Own<void> keepVendorAlive,
        Frankenvalue restoreArg) {
  KJ_SWITCH_ONEOF(vendorTokenMaybeSync) {
    KJ_CASE_ONEOF(vendorToken, kj::Array<byte>) {
      return encodeRestoredChannelToken(usage, type, vendorToken, kj::mv(restoreArg));
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
      return promise.then([this, usage, type, keepVendorAlive = kj::mv(keepVendorAlive),
                              restoreArg = kj::mv(restoreArg)](
                              kj::Array<byte> vendorToken) mutable -> kj::Promise<kj::Array<byte>> {
        // Ugh, we need to deconstruct the variant just to wrap a sychronous token in an
        // immediate promise.
        // TODO(cleanup): This could benefit from some helper around OneOf<T, Promise<T>> that
        //   lets us write `.then()` in a way that is synchronous if possible otherwise
        //   asynchronous.
        // TODO(cleanup): Another, different way to clean this up would be to allow kj::OneOf to
        //   convert to type T if all of the OneOf's variants can convert to T.
        KJ_SWITCH_ONEOF(encodeRestoredChannelToken(usage, type, vendorToken, kj::mv(restoreArg))) {
          KJ_CASE_ONEOF(baseToken, kj::Array<byte>) {
            return kj::mv(baseToken);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            return kj::mv(promise);
          }
        }
        KJ_UNREACHABLE;
      });
    }
  }
  KJ_UNREACHABLE;
}

// SubrequestChannel representing a `restored` token, that is, where we must call `[restore]()` on
// some other service in order to create the true channel.
//
// The main purpose of this class is to delay calling `[restore]()` until a request is actually
// made, so that if the channel is merely used to create a new token (i.e. the app deserialized a
// channel token and then simply serialized it again without making a call), then we don't bother
// invoking `[restore]()`.
class ChannelTokenHandler::RestoredSubrequestChannel final
    : public IoChannelFactory::SubrequestChannel {
 public:
  // Constructor for the "fresh" path: created by `ctx.restore()` (via makeRestoredSubrequestChannel
  // -> makeRestoredSubrequestChannelResolved). Here the vendor is the current entrypoint's
  // `ServerSelfTokenFactory` (used only to construct the token, never to call `[restore]()`,
  // because `inner` is already the freshly-restored channel).
  RestoredSubrequestChannel(ChannelTokenHandler& handler,
      kj::Own<ServerSelfTokenFactory> vendor,
      Frankenvalue restoreArg,
      kj::Own<IoChannelFactory::SubrequestChannel> inner)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)),
        restored(kj::mv(inner)) {}

  // Constructor for the "decoded" path: created by decoding a stored token. Here the vendor is a
  // real channel pointing at the entrypoint whose `[restore]()` method must be called (on first
  // use) to reconstruct the live channel.
  RestoredSubrequestChannel(ChannelTokenHandler& handler,
      kj::Own<IoChannelFactory::SubrequestChannel> vendor,
      Frankenvalue restoreArg)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    // Set this channel's token factory as `restoredSelfTokenFactory` so that if the target is a
    // dynamic worker or facet, it can still use `ctx.restore()` by wrapping this channel's token.
    metadata.restoredSelfTokenFactory = kj::refcounted<SelfTokenFactory>(kj::addRef(*this));

    return ensureRestored().startRequest(kj::mv(metadata));
  }

  void requireAllowsTransfer() override {}

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_SWITCH_ONEOF(vendor) {
      KJ_CASE_ONEOF(factory, kj::Own<ServerSelfTokenFactory>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::SUBREQUEST, kj::addRef(*factory), restoreArg.clone());
      }
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::SubrequestChannel>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::SUBREQUEST, kj::addRef(*channel), restoreArg.clone());
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  ChannelTokenHandler& handler;
  kj::OneOf<kj::Own<ServerSelfTokenFactory>, kj::Own<IoChannelFactory::SubrequestChannel>> vendor;
  Frankenvalue restoreArg;
  kj::Maybe<kj::Promise<void>> eventPromise;
  kj::Maybe<kj::Own<IoChannelFactory::SubrequestChannel>> restored;

  // A `SelfTokenFactory` that exposes this RestoredSubrequestChannel's own token, so that the
  // target of a request made on this channel can chain `ctx.restore()` off of it.
  class SelfTokenFactory final: public ServerSelfTokenFactory {
   public:
    SelfTokenFactory(kj::Own<RestoredSubrequestChannel> channel): channel(kj::mv(channel)) {}

    kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getSelfToken(
        IoChannelFactory::ChannelTokenUsage usage) override {
      return channel->getTokenMaybeSync(usage);
    }

   private:
    kj::Own<RestoredSubrequestChannel> channel;
  };

  // Actually call `[restore]()` on first use.
  IoChannelFactory::SubrequestChannel& ensureRestored() {
    KJ_IF_SOME(r, restored) {
      return *r;
    }

    auto& vendorChannel =
        KJ_REQUIRE_NONNULL(vendor.tryGet<kj::Own<IoChannelFactory::SubrequestChannel>>(),
            "a freshly-created RestoredSubrequestChannel should always have `inner` set, so "
            "ensureRestored() should never need to call the vendor");

    // TODO(someday): Should we pass in some metadata here? Maybe trace spans at least?
    auto worker = vendorChannel->startRequest({});

    auto event = kj::heap<api::RestoreServiceCustomEvent>(
        api::RestoreServiceCustomEvent::RESTORE_SERVICE_EVENT_TYPE, restoreArg.clone());
    auto channel = event->getChannel();

    // Awkward: What do we do with the CustomEvent promise? Presumably if the event fails, the
    // error will propagate through the channel. But we do need to keep the promise alive so that
    // it is not canceled. So... we keep it as a member of this class.
    eventPromise = worker->customEvent(kj::mv(event))
                       .ignoreResult()
                       .eagerlyEvaluate(nullptr)
                       .attach(kj::mv(worker));

    return *restored.emplace(kj::mv(channel));
  }
};

// Like RestoredSubrequestChannel, except it's an RpcChannel, that is, this restores an RpcStub /
// RpcTarget.
class ChannelTokenHandler::RestoredRpcChannel final: public IoChannelFactory::RpcChannel {
 public:
  // Constructor for the "fresh" path: created by `ctx.restore()`. The vendor is the current
  // entrypoint's `ServerSelfTokenFactory`, used only to construct the token. `restore()` is never
  // called in this case, because the freshly-created stub is paired with a live capability.
  RestoredRpcChannel(
      ChannelTokenHandler& handler, kj::Own<ServerSelfTokenFactory> vendor, Frankenvalue restoreArg)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)) {}

  // Constructor for the "decoded" path: created by decoding a stored token. The vendor is a real
  // channel; each `restore()` calls `[restore]()` on it to open a fresh session.
  RestoredRpcChannel(ChannelTokenHandler& handler,
      kj::Own<IoChannelFactory::SubrequestChannel> vendor,
      Frankenvalue restoreArg)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)) {}

  Session restore() override {
    // Unlike RestoredSubrequestChannel, we expect the caller of restore() already caches the
    // resulting stub, rather than calling again for every request. So, we don't need to cache
    // our result.
    return restoreWith(getVendorChannel());
  }

  void requireAllowsTransfer() override {}

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_SWITCH_ONEOF(vendor) {
      KJ_CASE_ONEOF(factory, kj::Own<ServerSelfTokenFactory>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::RPC, kj::addRef(*factory), restoreArg.clone());
      }
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::SubrequestChannel>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::RPC, kj::addRef(*channel), restoreArg.clone());
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  ChannelTokenHandler& handler;
  kj::OneOf<kj::Own<ServerSelfTokenFactory>, kj::Own<IoChannelFactory::SubrequestChannel>> vendor;
  Frankenvalue restoreArg;

  // Obtain a callable channel to the vendor, i.e. the entrypoint whose `[restore]()` method must
  // be invoked to (re)create the live RPC target.
  kj::Own<IoChannelFactory::SubrequestChannel> getVendorChannel() {
    KJ_SWITCH_ONEOF(vendor) {
      KJ_CASE_ONEOF(channel, kj::Own<IoChannelFactory::SubrequestChannel>) {
        return kj::addRef(*channel);
      }
      KJ_CASE_ONEOF(factory, kj::Own<ServerSelfTokenFactory>) {
        KJ_SWITCH_ONEOF(factory->getSelfToken(IoChannelFactory::ChannelTokenUsage::RPC)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            return handler.decodeSubrequestChannelToken(
                IoChannelFactory::ChannelTokenUsage::RPC, token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            auto& handlerRef = handler;
            return newPromisedChannel<IoChannelFactory::SubrequestChannel>(
                promise.then([&handlerRef](kj::Array<byte> token) {
              return handlerRef.decodeSubrequestChannelToken(
                  IoChannelFactory::ChannelTokenUsage::RPC, token);
            }));
          }
        }
        KJ_UNREACHABLE;
      }
    }
    KJ_UNREACHABLE;
  }

  Session restoreWith(kj::Own<IoChannelFactory::SubrequestChannel> vendorChannel) {
    // TODO(someday): Should we pass in some metadata here? Maybe trace spans at least?
    auto worker = vendorChannel->startRequest({});

    auto event = kj::heap<api::RestoreRpcStubCustomEvent>(
        api::RestoreRpcStubCustomEvent::RESTORE_RPC_STUB_EVENT_TYPE, restoreArg.clone());
    auto cap = event->getCap();
    auto task = worker->customEvent(kj::mv(event))
                    .ignoreResult()
                    .attach(kj::mv(vendorChannel))
                    .eagerlyEvaluate(nullptr);

    return {
      .cap = kj::mv(cap),
      .task = task.attach(kj::mv(worker)),
    };
  }
};

kj::Own<IoChannelFactory::SubrequestChannel> ChannelTokenHandler::makeRestoredSubrequestChannel(
    kj::Own<IoChannelFactory::SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    kj::Own<IoChannelFactory::SubrequestChannel> inner) {
  return kj::refcounted<RestoredSubrequestChannel>(*this,
      kj::mv(selfTokenFactory).downcast<ServerSelfTokenFactory>(), kj::mv(restoreParams),
      kj::mv(inner));
}
kj::Own<IoChannelFactory::RpcChannel> ChannelTokenHandler::makeRestoredRpcChannel(
    kj::Own<IoChannelFactory::SelfTokenFactory> selfTokenFactory, Frankenvalue restoreParams) {
  return kj::refcounted<RestoredRpcChannel>(
      *this, kj::mv(selfTokenFactory).downcast<ServerSelfTokenFactory>(), kj::mv(restoreParams));
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

  switch (reader.which()) {
    case ChannelToken::SERVICE: {
      auto service = reader.getService();

      kj::Maybe<kj::StringPtr> entrypoint;
      if (service.hasEntrypoint()) {
        entrypoint = service.getEntrypoint();
      }

      Frankenvalue props;
      if (service.hasProps()) {
        props = decodeFrankenvalue(usage, service.getProps());
      }

      // HACK: It would be more type-safe for us to return the (name, entrypoint, props) triplet and
      //   let the caller call the appropriate resolver method. However, this would require making
      //   heap string copies of the name and entrypoint which would just be thrown way immediately.
      //   Since both types happen to subclass Frankenvalue::CapTableEntry, we just make the resolver
      //   call here, return either type, and let the caller downcast to the right type.
      switch (type) {
        case ChannelToken::Type::SUBREQUEST:
          return resolver.resolveEntrypoint(service.getName(), entrypoint, kj::mv(props));
        case ChannelToken::Type::ACTOR_CLASS:
          return resolver.resolveActorClass(service.getName(), entrypoint, kj::mv(props));
        case ChannelToken::Type::RPC:
          KJ_FAIL_REQUIRE("RPC channel tokens must have a restore chain");
      }

      KJ_UNREACHABLE;
    }

    case ChannelToken::ACTOR: {
      auto actor = reader.getActor();

      KJ_REQUIRE(type == ChannelToken::Type::SUBREQUEST, "channel token type mismatch");

      kj::Maybe<kj::StringPtr> name;
      if (actor.hasName()) {
        name = actor.getName();
      }

      return resolver.resolveActor(actor.getNamespaceKey(), actor.getId(), name);
    }

    case ChannelToken::RESTORED: {
      auto restored = reader.getRestored();
      auto vendor = decodeSubrequestChannelToken(usage, restored.getVendor());
      auto restoreArg = decodeFrankenvalue(usage, restored.getRestoreArg());

      switch (type) {
        case ChannelToken::Type::SUBREQUEST:
          return kj::refcounted<RestoredSubrequestChannel>(
              *this, kj::mv(vendor), kj::mv(restoreArg));
        case ChannelToken::Type::ACTOR_CLASS:
          KJ_FAIL_REQUIRE("actor class channel tokens cannot have a restore chain");
        case ChannelToken::Type::RPC:
          return kj::refcounted<RestoredRpcChannel>(*this, kj::mv(vendor), kj::mv(restoreArg));
      }
    }
  }

  KJ_FAIL_REQUIRE("unknown channel token kind", reader.which());
}

Frankenvalue ChannelTokenHandler::decodeFrankenvalue(
    IoChannelFactory::ChannelTokenUsage usage, rpc::Frankenvalue::Reader reader) {
  kj::Vector<kj::Own<Frankenvalue::CapTableEntry>> capTable;
  if (reader.hasCapTable()) {
    auto tableReader = reader.getCapTable().getAs<ChannelToken::FrankenvalueCapTable>();
    if (!tableReader.hasCaps()) {
      return Frankenvalue::fromCapnp(reader, kj::mv(capTable));
    }

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
        case ChannelToken::FrankenvalueCapTable::Cap::RPC_CHANNEL:
          capTable.add(decodeRpcChannelToken(usage, cap.getRpcChannel()));
          continue;
      }
      KJ_FAIL_REQUIRE("unknown cap table type", cap.which());
    }
  }

  return Frankenvalue::fromCapnp(reader, kj::mv(capTable));
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

kj::Own<IoChannelFactory::RpcChannel> ChannelTokenHandler::decodeRpcChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::RPC, usage, token)
      .downcast<IoChannelFactory::RpcChannel>();
}

}  // namespace workerd::server
