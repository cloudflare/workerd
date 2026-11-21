// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "channel-token.h"

#include <workerd/api/restore.h>
#include <workerd/server/channel-token.capnp.h>
#include <workerd/util/entropy.h>

#include <kj-rs/convert.h>

#include <capnp/serialize.h>

namespace workerd::server {

namespace codec = channel_token_codec;

namespace {

static_assert(static_cast<uint16_t>(codec::TokenType::Subrequest) ==
    static_cast<uint16_t>(ChannelToken::Type::SUBREQUEST));
static_assert(static_cast<uint16_t>(codec::TokenType::ActorClass) ==
    static_cast<uint16_t>(ChannelToken::Type::ACTOR_CLASS));
static_assert(
    static_cast<uint16_t>(codec::TokenType::Rpc) == static_cast<uint16_t>(ChannelToken::Type::RPC));

codec::TokenType toCodec(ChannelToken::Type type) {
  return static_cast<codec::TokenType>(type);
}

codec::TokenUsage toCodec(IoChannelFactory::ChannelTokenUsage usage) {
  switch (usage) {
    case IoChannelFactory::ChannelTokenUsage::RPC:
      return codec::TokenUsage::Rpc;
    case IoChannelFactory::ChannelTokenUsage::STORAGE:
      return codec::TokenUsage::Storage;
  }
  KJ_UNREACHABLE;
}

codec::TokenPersistence toCodec(Persistent persistent) {
  return persistent ? codec::TokenPersistence::Yes : codec::TokenPersistence::No;
}

kj::String toString(const ::rust::Vec<uint8_t>& bytes) {
  return kj::heapString(kj::from<kj_rs::Rust>(bytes).asChars());
}

kj::Array<byte> toArray(const ::rust::Vec<uint8_t>& bytes) {
  return kj::heapArray(kj::from<kj_rs::Rust>(bytes));
}

// The IV of a token: random for RPC, none for storage.
class TokenIv {
 public:
  explicit TokenIv(IoChannelFactory::ChannelTokenUsage usage) {
    if (usage == IoChannelFactory::ChannelTokenUsage::RPC) {
      getEntropy(bytes);
      size = sizeof(bytes);
    }
  }

  ::rust::Slice<const uint8_t> asRust() const {
    return kj::arrayPtr(bytes, size).as<kj_rs::Rust>();
  }

 private:
  byte bytes[12]{};
  size_t size = 0;
};

::rust::Box<codec::TokenKey> newTokenKey(kj::Maybe<kj::StringPtr> clusterKey) {
  KJ_IF_SOME(k, clusterKey) {
    return codec::new_cluster_token_key(k.asBytes().as<kj_rs::Rust>());
  }
  byte secret[32]{};
  getEntropy(secret);
  return codec::new_token_key(kj::arrayPtr(secret).as<kj_rs::Rust>());
}

// The first segment is the size of a typical token's.
kj::Own<capnp::MallocMessageBuilder> newFrankenvalueMessage() {
  return kj::heap<capnp::MallocMessageBuilder>(128);
}

}  // namespace

ChannelTokenHandler::ChannelTokenHandler(Resolver& resolver, kj::Maybe<kj::StringPtr> clusterKey)
    : resolver(resolver),
      key(newTokenKey(clusterKey)) {}

void ChannelTokenHandler::requireStorable(
    IoChannelFactory::ChannelTokenUsage usage, Persistent persistent) {
  if (usage == IoChannelFactory::ChannelTokenUsage::STORAGE) {
    JSG_REQUIRE(persistent.toBool(), DOMDataCloneError,
        "Cannot store this stub because the target worker does not have the "
        "allow_irrevocable_stub_storage compatibility flag enabled.");
  }
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeChannelTokenImpl(ChannelToken::Type type,
        IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& propsValue,
        Persistent persistent) {
  requireStorable(usage, persistent);

  auto props = newFrankenvalueMessage();
  kj::Vector<kj::Promise<void>> promises;
  encodeFrankenvalue(usage, propsValue, props->getRoot<rpc::Frankenvalue>(), promises);

  if (promises.empty()) {
    return encodeServiceToken(type, usage, serviceName, entrypoint, *props, persistent);
  } else {
    return kj::joinPromisesFailFast(promises.releaseAsArray())
        .then([weakThis = addWeakToThis(), type, usage, serviceName = kj::str(serviceName),
                  entrypoint = entrypoint.map([](kj::StringPtr e) { return kj::str(e); }),
                  props = kj::mv(props), persistent]() mutable {
      return KJ_REQUIRE_NONNULL(weakThis.tryGet(), "ChannelTokenHandler destroyed during encoding")
          .encodeServiceToken(type, usage, serviceName,
              entrypoint.map([](kj::String& e) -> kj::StringPtr { return e; }), *props, persistent);
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

kj::Array<byte> ChannelTokenHandler::encodeServiceToken(ChannelToken::Type type,
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr serviceName,
    kj::Maybe<kj::StringPtr> entrypoint,
    capnp::MessageBuilder& props,
    Persistent persistent) {
  auto flatProps = capnp::messageToFlatArray(props);
  return toArray(codec::encode_service_token(*key, toCodec(usage), TokenIv(usage).asRust(),
      toCodec(type), toCodec(persistent), serviceName.asBytes().as<kj_rs::Rust>(),
      entrypoint.orDefault(""_kj).asBytes().as<kj_rs::Rust>(), entrypoint != kj::none,
      flatProps.asBytes().as<kj_rs::Rust>()));
}

kj::Array<byte> ChannelTokenHandler::encodeRestoredToken(ChannelToken::Type type,
    IoChannelFactory::ChannelTokenUsage usage,
    kj::ArrayPtr<const byte> vendorToken,
    capnp::MessageBuilder& restoreArg,
    Persistent persistent) {
  auto flatRestoreArg = capnp::messageToFlatArray(restoreArg);
  return toArray(codec::encode_restored_token(*key, toCodec(usage), TokenIv(usage).asRust(),
      toCodec(type), toCodec(persistent), vendorToken.as<kj_rs::Rust>(),
      flatRestoreArg.asBytes().as<kj_rs::Rust>()));
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeSubrequestChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& props,
        Persistent persistent) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::SUBREQUEST, usage, serviceName, entrypoint, props, persistent);
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeActorClassChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue& props,
        Persistent persistent) {
  return encodeChannelTokenImpl(
      ChannelToken::Type::ACTOR_CLASS, usage, serviceName, entrypoint, props, persistent);
}

kj::Array<byte> ChannelTokenHandler::encodeActorChannelToken(
    IoChannelFactory::ChannelTokenUsage usage,
    kj::StringPtr namespaceKey,
    kj::ArrayPtr<const byte> id,
    kj::Maybe<kj::StringPtr> name,
    Persistent persistent) {
  requireStorable(usage, persistent);

  return toArray(codec::encode_actor_token(*key, toCodec(usage), TokenIv(usage).asRust(),
      toCodec(persistent), namespaceKey.asBytes().as<kj_rs::Rust>(), id.as<kj_rs::Rust>(),
      name.orDefault(""_kj).asBytes().as<kj_rs::Rust>(), name != kj::none));
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::ArrayPtr<const byte> vendorToken,
        Frankenvalue restoreArg,
        Persistent persistent) {
  requireStorable(usage, persistent);

  auto message = newFrankenvalueMessage();
  kj::Vector<kj::Promise<void>> promises;
  encodeFrankenvalue(usage, restoreArg, message->getRoot<rpc::Frankenvalue>(), promises);

  if (promises.empty()) {
    return encodeRestoredToken(type, usage, vendorToken, *message, persistent);
  } else {
    return kj::joinPromisesFailFast(promises.releaseAsArray())
        .then([weakThis = addWeakToThis(), type, usage, vendorToken = kj::heapArray(vendorToken),
                  message = kj::mv(message), persistent]() mutable {
      return KJ_REQUIRE_NONNULL(weakThis.tryGet(), "ChannelTokenHandler destroyed during encoding")
          .encodeRestoredToken(type, usage, vendorToken, *message, persistent);
    });
  }
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::Rc<IoChannelFactory::SubrequestChannel> vendor,
        Frankenvalue restoreArg,
        Persistent persistent) {
  auto vendorTokenMaybeSync = vendor->getTokenMaybeSync(usage);
  return encodeRestoredChannelTokenImpl(
      usage, type, kj::mv(vendorTokenMaybeSync), vendor.toOwn(), kj::mv(restoreArg), persistent);
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelToken(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::Rc<ServerSelfTokenFactory> vendor,
        Frankenvalue restoreArg,
        Persistent persistent) {
  auto vendorTokenMaybeSync = vendor->getSelfToken(usage);
  return encodeRestoredChannelTokenImpl(
      usage, type, kj::mv(vendorTokenMaybeSync), vendor.toOwn(), kj::mv(restoreArg), persistent);
}

kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> ChannelTokenHandler::
    encodeRestoredChannelTokenImpl(IoChannelFactory::ChannelTokenUsage usage,
        ChannelToken::Type type,
        kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> vendorTokenMaybeSync,
        kj::Own<void> keepVendorAlive,
        Frankenvalue restoreArg,
        Persistent persistent) {
  KJ_SWITCH_ONEOF(vendorTokenMaybeSync) {
    KJ_CASE_ONEOF(vendorToken, kj::Array<byte>) {
      return encodeRestoredChannelToken(usage, type, vendorToken, kj::mv(restoreArg), persistent);
    }
    KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
      return promise.then(
          [weakThis = addWeakToThis(), usage, type, keepVendorAlive = kj::mv(keepVendorAlive),
              restoreArg = kj::mv(restoreArg),
              persistent](kj::Array<byte> vendorToken) mutable -> kj::Promise<kj::Array<byte>> {
        // Ugh, we need to deconstruct the variant just to wrap a sychronous token in an
        // immediate promise.
        // TODO(cleanup): This could benefit from some helper around OneOf<T, Promise<T>> that
        //   lets us write `.then()` in a way that is synchronous if possible otherwise
        //   asynchronous.
        // TODO(cleanup): Another, different way to clean this up would be to allow kj::OneOf to
        //   convert to type T if all of the OneOf's variants can convert to T.
        auto& self =
            KJ_REQUIRE_NONNULL(weakThis.tryGet(), "ChannelTokenHandler destroyed during encoding");
        KJ_SWITCH_ONEOF(self.encodeRestoredChannelToken(
                            usage, type, vendorToken, kj::mv(restoreArg), persistent)) {
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
      kj::Rc<ServerSelfTokenFactory> vendor,
      Frankenvalue restoreArg,
      kj::Rc<IoChannelFactory::SubrequestChannel> inner,
      Persistent persistent)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)),
        persistent(persistent),
        restored(kj::mv(inner)) {}

  // Constructor for the "decoded" path: created by decoding a stored token. Here the vendor is a
  // real channel pointing at the entrypoint whose `[restore]()` method must be called (on first
  // use) to reconstruct the live channel.
  RestoredSubrequestChannel(ChannelTokenHandler& handler,
      kj::Rc<IoChannelFactory::SubrequestChannel> vendor,
      Frankenvalue restoreArg,
      Persistent persistent)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)),
        persistent(persistent) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    // Set this channel's token factory as `restoredSelfTokenFactory` so that if the target is a
    // dynamic worker or facet, it can still use `ctx.restore()` by wrapping this channel's token.
    metadata.restoredSelfTokenFactory = kj::rc<SelfTokenFactory>(addRefToThis());

    // Note: We do NOT want to modify `metadata.fromPersistentStub` here. Our own `persistent` flag
    // indicates whether the Worker whose `[restore]()` method was called allows persistence. The
    // metadata forwarded here is for the request to the target worker that [restore]() returned.
    // That worker is not required to have opted into stub storage, because this token does not
    // encode anything private to the target worker (e.g. props).
    //
    // Unless, of course, the restore chain continues *through* the target worker, in which case,
    // `metadata` will already have `persistent = YES` set by its caller. No need for us to modify.
    return ensureRestored().startRequest(kj::mv(metadata));
  }

  void requireAllowsTransfer() override {}

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage usage) override {
    KJ_SWITCH_ONEOF(vendor) {
      KJ_CASE_ONEOF(factory, kj::Rc<ServerSelfTokenFactory>) {
        return handler.encodeRestoredChannelToken(usage, ChannelToken::Type::SUBREQUEST,
            factory.addRef(), restoreArg.clone(), persistent);
      }
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::SubrequestChannel>) {
        return handler.encodeRestoredChannelToken(usage, ChannelToken::Type::SUBREQUEST,
            channel.addRef(), restoreArg.clone(), persistent);
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  ChannelTokenHandler& handler;
  kj::OneOf<kj::Rc<ServerSelfTokenFactory>, kj::Rc<IoChannelFactory::SubrequestChannel>> vendor;
  Frankenvalue restoreArg;
  Persistent persistent;
  kj::Maybe<kj::Promise<void>> eventPromise;
  kj::Maybe<kj::Rc<IoChannelFactory::SubrequestChannel>> restored;

  // A `SelfTokenFactory` that exposes this RestoredSubrequestChannel's own token, so that the
  // target of a request made on this channel can chain `ctx.restore()` off of it.
  class SelfTokenFactory final: public ServerSelfTokenFactory {
   public:
    SelfTokenFactory(kj::Rc<RestoredSubrequestChannel> channel): channel(kj::mv(channel)) {}

    kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getSelfToken(
        IoChannelFactory::ChannelTokenUsage usage) override {
      return channel->getTokenMaybeSync(usage);
    }

   private:
    kj::Rc<RestoredSubrequestChannel> channel;
  };

  // Actually call `[restore]()` on first use.
  IoChannelFactory::SubrequestChannel& ensureRestored() {
    KJ_IF_SOME(r, restored) {
      return *r;
    }

    auto& vendorChannel =
        KJ_REQUIRE_NONNULL(vendor.tryGet<kj::Rc<IoChannelFactory::SubrequestChannel>>(),
            "a freshly-created RestoredSubrequestChannel should always have `inner` set, so "
            "ensureRestored() should never need to call the vendor");

    // When the `persistent` flag is set on a restore token, it means that the worker on which we
    // are calling `[restore]()` is required to support persistence. Specifically, the
    // allow_irrevocable_stub_storage flag in this case is protecting the restore params. So, we
    // propogate our `persistent` flag to the SubrequestMetadata passed into the restore request.
    // Note that worker targeted by the stub *returned by* [restore]() is *not* required to support
    // persistence. This can be a bit confusing compared to root (non-restore-based) tokens, where
    // the root token is persistable when the Worker it points to is persistable.
    // TODO(someday): Should we pass in some more metadata here? Maybe trace spans at least?
    IoChannelFactory::SubrequestMetadata vendorMetadata;
    vendorMetadata.fromPersistentStub = persistent;
    auto worker = vendorChannel->startRequest(kj::mv(vendorMetadata));

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
  RestoredRpcChannel(ChannelTokenHandler& handler,
      kj::Rc<ServerSelfTokenFactory> vendor,
      Frankenvalue restoreArg,
      Persistent persistent)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)),
        persistent(persistent) {}

  // Constructor for the "decoded" path: created by decoding a stored token. The vendor is a real
  // channel; each `restore()` calls `[restore]()` on it to open a fresh session.
  RestoredRpcChannel(ChannelTokenHandler& handler,
      kj::Rc<IoChannelFactory::SubrequestChannel> vendor,
      Frankenvalue restoreArg,
      Persistent persistent)
      : handler(handler),
        vendor(kj::mv(vendor)),
        restoreArg(kj::mv(restoreArg)),
        persistent(persistent) {}

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
      KJ_CASE_ONEOF(factory, kj::Rc<ServerSelfTokenFactory>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::RPC, factory.addRef(), restoreArg.clone(), persistent);
      }
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::SubrequestChannel>) {
        return handler.encodeRestoredChannelToken(
            usage, ChannelToken::Type::RPC, channel.addRef(), restoreArg.clone(), persistent);
      }
    }
    KJ_UNREACHABLE;
  }

 private:
  ChannelTokenHandler& handler;
  kj::OneOf<kj::Rc<ServerSelfTokenFactory>, kj::Rc<IoChannelFactory::SubrequestChannel>> vendor;
  Frankenvalue restoreArg;
  Persistent persistent;

  // Obtain a callable channel to the vendor, i.e. the entrypoint whose `[restore]()` method must
  // be invoked to (re)create the live RPC target.
  kj::Rc<IoChannelFactory::SubrequestChannel> getVendorChannel() {
    KJ_SWITCH_ONEOF(vendor) {
      KJ_CASE_ONEOF(channel, kj::Rc<IoChannelFactory::SubrequestChannel>) {
        return channel.addRef();
      }
      KJ_CASE_ONEOF(factory, kj::Rc<ServerSelfTokenFactory>) {
        KJ_SWITCH_ONEOF(factory->getSelfToken(IoChannelFactory::ChannelTokenUsage::RPC)) {
          KJ_CASE_ONEOF(token, kj::Array<byte>) {
            return handler.decodeSubrequestChannelToken(
                IoChannelFactory::ChannelTokenUsage::RPC, token);
          }
          KJ_CASE_ONEOF(promise, kj::Promise<kj::Array<byte>>) {
            return newPromisedChannel<IoChannelFactory::SubrequestChannel>(
                promise.then([weakHandler = handler.addWeak()](kj::Array<byte> token) {
              return KJ_REQUIRE_NONNULL(
                  weakHandler.tryGet(), "ChannelTokenHandler destroyed before token was resolved")
                  .decodeSubrequestChannelToken(IoChannelFactory::ChannelTokenUsage::RPC, token);
            }));
          }
        }
        KJ_UNREACHABLE;
      }
    }
    KJ_UNREACHABLE;
  }

  Session restoreWith(kj::Rc<IoChannelFactory::SubrequestChannel> vendorChannel) {
    // Just like RestoredSubrequestChannel::ensureRestored(), we must verify that the worker on
    // which we call `[restore]()` allows persistence, if we are marked persistent.
    // TODO(someday): Should we pass in some more metadata here? Maybe trace spans at least?
    IoChannelFactory::SubrequestMetadata vendorMetadata;
    vendorMetadata.fromPersistentStub = persistent;
    auto worker = vendorChannel->startRequest(kj::mv(vendorMetadata));

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

kj::Rc<IoChannelFactory::SubrequestChannel> ChannelTokenHandler::makeRestoredSubrequestChannel(
    kj::Rc<IoChannelFactory::SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    kj::Rc<IoChannelFactory::SubrequestChannel> inner,
    Persistent persistent) {
  return kj::rc<RestoredSubrequestChannel>(*this,
      kj::mv(selfTokenFactory).downcast<ServerSelfTokenFactory>(), kj::mv(restoreParams),
      kj::mv(inner), persistent);
}
kj::Rc<IoChannelFactory::RpcChannel> ChannelTokenHandler::makeRestoredRpcChannel(
    kj::Rc<IoChannelFactory::SelfTokenFactory> selfTokenFactory,
    Frankenvalue restoreParams,
    Persistent persistent) {
  return kj::rc<RestoredRpcChannel>(*this,
      kj::mv(selfTokenFactory).downcast<ServerSelfTokenFactory>(), kj::mv(restoreParams),
      persistent);
}

kj::Rc<IoChannelFactory::TokenizableChannel> ChannelTokenHandler::decodeChannelTokenImpl(
    ChannelToken::Type type,
    IoChannelFactory::ChannelTokenUsage usage,
    kj::ArrayPtr<const byte> token) {
  // The codec checks that a token of `type` may point at what it points at.
  auto decoded = codec::decode_token(*key, toCodec(usage), toCodec(type), token.as<kj_rs::Rust>());
  Persistent persistent(decoded.persistent == codec::TokenPersistence::Yes);

  switch (decoded.kind) {
    case codec::DecodedKind::Service: {
      auto serviceName = toString(decoded.name);
      kj::Maybe<kj::String> entrypoint;
      if (decoded.has_entrypoint) {
        entrypoint = toString(decoded.entrypoint);
      }
      auto entrypointPtr = entrypoint.map([](kj::String& e) -> kj::StringPtr { return e; });

      Frankenvalue props;
      if (decoded.has_value) {
        props = decodeFrankenvalue(usage, kj::from<kj_rs::Rust>(decoded.value));
      }

      // HACK: It would be more type-safe for us to return the (name, entrypoint, props) triplet and
      //   let the caller call the appropriate resolver method. Since both types happen to subclass
      //   Frankenvalue::CapTableEntry, we just make the resolver call here, return either type,
      //   and let the caller downcast to the right type.
      switch (type) {
        case ChannelToken::Type::SUBREQUEST:
          return resolver.resolveEntrypoint(serviceName, entrypointPtr, kj::mv(props), persistent);
        case ChannelToken::Type::ACTOR_CLASS:
          return resolver.resolveActorClass(serviceName, entrypointPtr, kj::mv(props), persistent);
        case ChannelToken::Type::RPC:
          break;
      }
      break;
    }

    case codec::DecodedKind::Actor: {
      KJ_ASSERT(type == ChannelToken::Type::SUBREQUEST);
      auto namespaceKey = toString(decoded.name);
      kj::Maybe<kj::String> name;
      if (decoded.has_entrypoint) {
        name = toString(decoded.entrypoint);
      }
      return resolver.resolveActor(namespaceKey, kj::from<kj_rs::Rust>(decoded.id),
          name.map([](kj::String& n) -> kj::StringPtr { return n; }), persistent);
    }

    case codec::DecodedKind::Restored: {
      auto vendor = decodeSubrequestChannelToken(usage, kj::from<kj_rs::Rust>(decoded.vendor));
      auto restoreArg = decoded.has_value
          ? decodeFrankenvalue(usage, kj::from<kj_rs::Rust>(decoded.value))
          : decodeFrankenvalue(usage, rpc::Frankenvalue::Reader());

      switch (type) {
        case ChannelToken::Type::SUBREQUEST:
          return kj::rc<RestoredSubrequestChannel>(
              *this, kj::mv(vendor), kj::mv(restoreArg), persistent);
        case ChannelToken::Type::ACTOR_CLASS:
          break;
        case ChannelToken::Type::RPC:
          return kj::rc<RestoredRpcChannel>(*this, kj::mv(vendor), kj::mv(restoreArg), persistent);
      }
      break;
    }
  }

  KJ_FAIL_ASSERT("the channel token codec returned a token of a kind its type cannot have",
      static_cast<uint8_t>(decoded.kind), type);
}

Frankenvalue ChannelTokenHandler::decodeFrankenvalue(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> flat) {
  // A capnp message must be word-aligned.
  auto words = kj::heapArray<capnp::word>(flat.size() / sizeof(capnp::word));
  KJ_ASSERT(words.asBytes().size() == flat.size());
  words.asBytes().copyFrom(flat);
  capnp::FlatArrayMessageReader message(words);
  return decodeFrankenvalue(usage, message.getRoot<rpc::Frankenvalue>());
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
          capTable.add(decodeSubrequestChannelToken(usage, cap.getSubrequestChannel()).toOwn());
          continue;
        case ChannelToken::FrankenvalueCapTable::Cap::ACTOR_CLASS_CHANNEL:
          capTable.add(decodeActorClassChannelToken(usage, cap.getActorClassChannel()).toOwn());
          continue;
        case ChannelToken::FrankenvalueCapTable::Cap::RPC_CHANNEL:
          capTable.add(decodeRpcChannelToken(usage, cap.getRpcChannel()).toOwn());
          continue;
      }
      KJ_FAIL_REQUIRE("unknown cap table type", cap.which());
    }
  }

  return Frankenvalue::fromCapnp(reader, kj::mv(capTable));
}

kj::Rc<IoChannelFactory::SubrequestChannel> ChannelTokenHandler::decodeSubrequestChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::SUBREQUEST, usage, token)
      .downcast<IoChannelFactory::SubrequestChannel>();
}

kj::Rc<IoChannelFactory::ActorClassChannel> ChannelTokenHandler::decodeActorClassChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::ACTOR_CLASS, usage, token)
      .downcast<IoChannelFactory::ActorClassChannel>();
}

kj::Rc<IoChannelFactory::RpcChannel> ChannelTokenHandler::decodeRpcChannelToken(
    IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token) {
  return decodeChannelTokenImpl(ChannelToken::Type::RPC, usage, token)
      .downcast<IoChannelFactory::RpcChannel>();
}

}  // namespace workerd::server
