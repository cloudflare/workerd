// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token-codec/ffi.rs.h>
#include <workerd/server/channel-token.capnp.h>

#include <capnp/message.h>

namespace workerd::server {

// Helper class to encode channel tokens for workerd.
//
// This is an internal implementation helper for `WorkerFactory` (in `factory/worker-factory.h`),
// separated out into its own module solely for unit testing purposes. Nobody except
// `WorkerFactory` should use this interface directly.
//
// Note that all `Frankenvalue`s here are expected to contain cap tables holding live instances
// of `SubrequestChannel`, `ActorClassChannel`, and `RpcChannel`.
//
// The encoding and crypto of a token, the `ChannelToken` message and its envelope, are the Rust
// codec's (channel-token-codec/). The handler serializes the cap tables of the Frankenvalues in a
// token, which hold live channels, resolves what a token points at, and implements the restored
// channels.
class ChannelTokenHandler: public kj::PtrTarget {
 public:
  kj::Weak<ChannelTokenHandler> addWeak() {
    return addWeakToThis();
  }

  // Callbacks implemented by `WorkerFactory` to resolve entrypoint designators to live
  // objects.
  //
  // (In theory, we could have a decodeChannelToken() method that returns the service name,
  // entrypoint name, and props as a struct, but this would require extra string copies and would
  // also make abstractions a little messier in the factory.)
  class Resolver {
   public:
    virtual kj::Rc<IoChannelFactory::SubrequestChannel> resolveEntrypoint(kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue props,
        Persistent persistent) = 0;

    virtual kj::Rc<IoChannelFactory::ActorClassChannel> resolveActorClass(kj::StringPtr serviceName,
        kj::Maybe<kj::StringPtr> entrypoint,
        Frankenvalue props,
        Persistent persistent) = 0;

    virtual kj::Rc<IoChannelFactory::ActorChannel> resolveActor(kj::StringPtr namespaceKey,
        kj::ArrayPtr<const byte> id,
        kj::Maybe<kj::StringPtr> name,
        Persistent persistent) = 0;
  };

  // workerd's implementation of `IoChannelFactory::SelfTokenFactory`. Produces the encoded
  // "vendor" token to embed in a restored channel token (i.e. the token referring to the
  // entrypoint whose `[restore]()` method must be called). Concrete implementations live in
  // `factory/worker-factory.c++` (one for static workers, one for actors).
  class ServerSelfTokenFactory: public IoChannelFactory::SelfTokenFactory {
   public:
    // Get the token referring to "self", to be embedded as the vendor of a restored token.
    virtual kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getSelfToken(
        IoChannelFactory::ChannelTokenUsage usage) = 0;
  };

  // Instances of a cluster share `clusterKey`, so each decodes the others' tokens.
  explicit ChannelTokenHandler(Resolver& resolver, kj::Maybe<kj::StringPtr> clusterKey = kj::none);

  // Helpers to implement `IoChannelFactory::{SubrequestChannel,ActorClassChannel}::getToken()`.
  //
  // `persistent` records whether the channel's target is a storable (persistent) target. If `usage`
  // is STORAGE and `persistent` is `Persistent::NO`, these throw `DOMDataCloneError`.
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props,
      Persistent persistent);
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeActorClassChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props,
      Persistent persistent);
  kj::Array<byte> encodeActorChannelToken(IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name,
      Persistent persistent);
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeRestoredChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      ChannelToken::Type type,
      kj::ArrayPtr<const byte> vendorToken,
      Frankenvalue restoreArg,
      Persistent persistent);
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeRestoredChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      ChannelToken::Type type,
      kj::Rc<IoChannelFactory::SubrequestChannel> vendor,
      Frankenvalue restoreArg,
      Persistent persistent);
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeRestoredChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      ChannelToken::Type type,
      kj::Rc<ServerSelfTokenFactory> vendor,
      Frankenvalue restoreArg,
      Persistent persistent);

  // Implements the respective methods of IoChannelFactory. `persistent` is the
  // `allow_irrevocable_stub_storage` flag of the worker invoking `ctx.restore()`.
  kj::Rc<IoChannelFactory::SubrequestChannel> makeRestoredSubrequestChannel(
      kj::Rc<IoChannelFactory::SelfTokenFactory> selfTokenFactory,
      Frankenvalue restoreParams,
      kj::Rc<IoChannelFactory::SubrequestChannel> inner,
      Persistent persistent);
  kj::Rc<IoChannelFactory::RpcChannel> makeRestoredRpcChannel(
      kj::Rc<IoChannelFactory::SelfTokenFactory> selfTokenFactory,
      Frankenvalue restoreParams,
      Persistent persistent);

  // Helpers to implement `IoChannelFactory::{subrequestChannel,actorClass}FromToken()`.
  kj::Rc<IoChannelFactory::SubrequestChannel> decodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  kj::Rc<IoChannelFactory::ActorClassChannel> decodeActorClassChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  kj::Rc<IoChannelFactory::RpcChannel> decodeRpcChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);

 private:
  Resolver& resolver;
  ::rust::Box<channel_token_codec::TokenKey> key;

  // Throws `DOMDataCloneError` if `usage` is STORAGE but `persistent` is `Persistent::NO`.
  static void requireStorable(IoChannelFactory::ChannelTokenUsage usage, Persistent persistent);

  // Implementation for both `encode` methods.
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeChannelTokenImpl(
      ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props,
      Persistent persistent);
  void encodeFrankenvalue(IoChannelFactory::ChannelTokenUsage usage,
      Frankenvalue& value,
      rpc::Frankenvalue::Builder valueBuilder,
      kj::Vector<kj::Promise<void>>& promises);

  // Encode a token through the codec. `props` and `restoreArg` are messages whose root is the
  // Frankenvalue, its cap table complete.
  kj::Array<byte> encodeServiceToken(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      capnp::MessageBuilder& props,
      Persistent persistent);
  kj::Array<byte> encodeRestoredToken(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::ArrayPtr<const byte> vendorToken,
      capnp::MessageBuilder& restoreArg,
      Persistent persistent);

  // Implementation that dynamically returns a SubrequestChannel, ActorClassChannel, or RpcChannel,
  // which all inherit TokenizableChannel. The caller will immediately downcast to the right type.
  kj::Rc<IoChannelFactory::TokenizableChannel> decodeChannelTokenImpl(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::ArrayPtr<const byte> token);

  // Decode a Frankenvalue the codec read, a flat capnp message, decoding its cap table.
  Frankenvalue decodeFrankenvalue(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> flat);
  Frankenvalue decodeFrankenvalue(
      IoChannelFactory::ChannelTokenUsage usage, rpc::Frankenvalue::Reader reader);

  // Shared implementation of the `kj::Rc<SubrequestChannel>` and `kj::Rc<ServerSelfTokenFactory>`
  // overloads of `encodeRestoredChannelToken()`. `vendorTokenMaybeSync` is the (possibly async)
  // vendor token; `keepVendorAlive` keeps the object that produced it alive while we await.
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeRestoredChannelTokenImpl(
      IoChannelFactory::ChannelTokenUsage usage,
      ChannelToken::Type type,
      kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> vendorTokenMaybeSync,
      kj::Own<void> keepVendorAlive,
      Frankenvalue restoreArg,
      Persistent persistent);

  class RestoredSubrequestChannel;
  class RestoredRpcChannel;
};

}  // namespace workerd::server
