// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token.capnp.h>

#include <capnp/message.h>

namespace workerd::server {

// Helper class to encode channel tokens for workerd.
//
// This is an internal implementation helper for `Server` (in `server.h`), separated out into its
// own module solely for unit testing purposes. Nobody except `Server` should use this interface
// directly.
//
// Note that all `Frankenvalue`s here are expected to contain cap tables holding live instances
// of `SubrequestChannel`, `ActorClassChannel`, and `RpcChannel`.
class ChannelTokenHandler {
 public:
  // Callbacks implemented by `Server` (in `server.h`) to resolve entrypoint designators to live
  // objects.
  //
  // (In theory, we could have a decodeChannelToken() method that returns the service name,
  // entrypoint name, and props as a struct, but this would require extra string copies and would
  // also make abstractions a little messier in server.c++.)
  class Resolver {
   public:
    virtual kj::Own<IoChannelFactory::SubrequestChannel> resolveEntrypoint(
        kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) = 0;

    virtual kj::Own<IoChannelFactory::ActorClassChannel> resolveActorClass(
        kj::StringPtr serviceName, kj::Maybe<kj::StringPtr> entrypoint, Frankenvalue props) = 0;

    virtual kj::Own<IoChannelFactory::ActorChannel> resolveActor(
        kj::StringPtr namespaceKey, kj::ArrayPtr<const byte> id, kj::Maybe<kj::StringPtr> name) = 0;

    // Start a request on `channel`, arranging for the target IoContext's self-token to be set
    // to `selfToken`. Used by the restore-chain replay code: after RestoreServiceCustomEvent
    // returns a SubrequestChannel, the chain walker calls this to create the WorkerInterface with
    // the correct self-token injected. The Server implementation handles the concrete type dispatch
    // (Service vs ActorChannelImpl) and threads the token through the internal startRequest()
    // overloads to newWorkerEntrypoint() → IoContext.
    virtual kj::Own<WorkerInterface> startSubrequestWithSelfToken(
        IoChannelFactory::SubrequestChannel& channel,
        IoChannelFactory::SubrequestMetadata metadata,
        kj::Array<byte> selfToken) = 0;
  };

  explicit ChannelTokenHandler(Resolver& resolver);

  // Helpers to implement `IoChannelFactory::{SubrequestChannel,ActorClassChannel}::getToken()`.
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeActorClassChannelToken(
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);
  kj::Array<byte> encodeActorChannelToken(IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr namespaceKey,
      kj::ArrayPtr<const byte> id,
      kj::Maybe<kj::StringPtr> name);

  // Helpers to implement `IoChannelFactory::{subrequestChannel,actorClass}FromToken()`.
  kj::Own<IoChannelFactory::SubrequestChannel> decodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  kj::Own<IoChannelFactory::ActorClassChannel> decodeActorClassChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  kj::Own<IoChannelFactory::RpcChannel> decodeRpcChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);

  kj::Own<IoChannelFactory::SubrequestChannel> makeRestoredSubrequestChannel(
      kj::ArrayPtr<const byte> baseToken, Frankenvalue restoreParams);
  kj::Own<IoChannelFactory::RpcChannel> makeRestoredRpcChannel(
      kj::ArrayPtr<const byte> baseToken, Frankenvalue restoreParams);

 private:
  // Annoyingly the OpenSSL/BoringSSL headers don't seem to define these as static constants.
  static constexpr uint AES_KEY_SIZE = 32;
  static constexpr uint AES_IV_SIZE = 12;
  static constexpr uint AES_MAC_SIZE = 16;

  // The key ID is the 16-byte prefix of a SHA-256 hash of the secret key.
  static constexpr uint KEY_ID_SIZE = 16;

  Resolver& resolver;
  byte tokenKey[AES_KEY_SIZE];
  byte keyId[KEY_ID_SIZE];

  struct TokenHeader {
    uint32_t magic;
    byte iv[AES_IV_SIZE];
    byte keyId[KEY_ID_SIZE];
  };
  static_assert(sizeof(TokenHeader) == 32);

  // Implementation for both `encode` methods.
  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> encodeChannelTokenImpl(
      ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);
  kj::Array<byte> serializeTokenImpl(
      IoChannelFactory::ChannelTokenUsage usage, capnp::MessageBuilder& message);

  // Implementation that dynamically returns either SubrequestChannel or ActorClassChannel, which
  // both happen to inherit CapTableEntry. The caller will immediately downcast to the right type.
  kj::Own<Frankenvalue::CapTableEntry> decodeChannelTokenImpl(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::ArrayPtr<const byte> token);
  kj::Own<capnp::MallocMessageBuilder> parseChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  Frankenvalue decodeFrankenvalue(
      IoChannelFactory::ChannelTokenUsage usage, rpc::Frankenvalue::Reader reader);

  class RestoreChainSubrequestChannel;
  class RestoreChainRpcChannel;
};

}  // namespace workerd::server
