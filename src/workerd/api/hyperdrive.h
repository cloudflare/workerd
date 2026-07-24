// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace kj {
class AsyncIoStream;
}

namespace workerd::api {

class Socket;

// A Hyperdrive resource for development integrations.
//
// Provides the same interface as Hyperdrive while sending connection
// traffic directly to postgres
class Hyperdrive: public jsg::Object {
 public:
  // `clientIndex` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.
  explicit Hyperdrive(uint clientIndex,
      kj::String database,
      kj::String user,
      kj::String password,
      kj::String scheme);
  jsg::Ref<Socket> connect(jsg::Lock& js);
  kj::StringPtr getDatabase();
  kj::StringPtr getUser();
  kj::StringPtr getPassword();
  kj::StringPtr getScheme();

  kj::StringPtr getHost();

  // A synthetic IPv4 (in the reserved 240.0.0.0/4 block) routing to this binding, for drivers that
  // require an IP literal rather than a hostname. Connecting to it reaches the same place as host.
  kj::StringPtr getIP();

  uint16_t getPort();

  kj::String getConnectionString();

  JSG_RESOURCE_TYPE(Hyperdrive) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(database, getDatabase);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(user, getUser);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(password, getPassword);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(host, getHost);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(ip, getIP);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(port, getPort);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(connectionString, getConnectionString);

    JSG_METHOD(connect);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("randomHost", randomHost);
    tracker.trackField("randomIp", randomIp);
    tracker.trackField("database", database);
    tracker.trackField("user", user);
    tracker.trackField("password", password);
    tracker.trackField("scheme", scheme);
  }

 private:
  uint clientIndex;
  kj::String randomHost;
  kj::String randomIp;
  kj::String database;
  kj::String user;
  kj::String password;
  kj::String scheme;
  bool registeredConnectOverride = false;

  // Registers the connect/dns overrides for the hostname and synthetic IP. Requires an active
  // IoContext, so it is done lazily on first access rather than in the constructor.
  void registerConnectOverride();

  kj::Promise<kj::Own<kj::AsyncIoStream>> connectToDb();
};
#define EW_HYPERDRIVE_ISOLATE_TYPES api::Hyperdrive
}  // namespace workerd::api
