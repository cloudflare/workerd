// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-channels.h>
#include <workerd/server/channel-token.capnp.h>

namespace workerd::server {

// Helper class to encode channel tokens for workerd.
//
// This is an internal implementation helper for `Server` (in `server.h`), separated out into its
// own module solely for unit testing purposes. Nobody except `Server` should use this interface
// directly.
//
// Note that all `Frankenvalue`s here are expected to contain cap tables holding live instances
// of `SubrequestChannel` and `ActorClassChannel`.
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
  };

  explicit ChannelTokenHandler(Resolver& resolver);

  // Helpers to implement `IoChannelFactory::{SubrequestChannel,ActorClassChannel}::getToken()`.
  kj::Array<byte> encodeSubrequestChannelToken(IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);
  kj::Array<byte> encodeActorClassChannelToken(IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);

  // Helpers to implement `IoChannelFactory::{subrequestChannel,actorClass}FromToken()`.
  kj::Own<IoChannelFactory::SubrequestChannel> decodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);
  kj::Own<IoChannelFactory::ActorClassChannel> decodeActorClassChannelToken(
      IoChannelFactory::ChannelTokenUsage usage, kj::ArrayPtr<const byte> token);

 private:
  // Annoyingly the OpenSSL/BoringSSL headers don't seem to define these as static constants.
  static constexpr size_t AES_KEY_SIZE = 32;
  static constexpr size_t AES_IV_SIZE = 12;
  static constexpr size_t AES_MAC_SIZE = 16;

  Resolver& resolver;
  byte tokenKey[AES_KEY_SIZE];

  // Implementation for both `encode` methods.
  kj::Array<byte> encodeChannelTokenImpl(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::StringPtr serviceName,
      kj::Maybe<kj::StringPtr> entrypoint,
      Frankenvalue& props);

  // Implementation that dynamically returns either SubrequestChannel or ActorClassChannel, which
  // both happen to inherit CapTableEntry. The caller will immediately downcast to the right type.
  kj::Own<Frankenvalue::CapTableEntry> decodeChannelTokenImpl(ChannelToken::Type type,
      IoChannelFactory::ChannelTokenUsage usage,
      kj::ArrayPtr<const byte> token);
};

}  // namespace workerd::server
