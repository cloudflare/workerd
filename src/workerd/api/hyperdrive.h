// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "streams.h"
#include <capnp/compat/json.h>
#include <cstdint>
#include <kj/common.h>
#include <kj/async-io.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/http-util.h>

namespace workerd::api::public_beta {

// A capability to an R2 Bucket.
class Hyperdrive : public jsg::Object {
public:
  // `clientIndex` is what to pass to IoContext::getHttpClient() to get an HttpClient
  // representing this namespace.
  explicit Hyperdrive(uint clientIndex, kj::String database,
                      kj::String user, kj::String password);
  jsg::Ref<Socket> connect(jsg::Lock& js);
  kj::StringPtr getDatabase();
  kj::StringPtr getUser();
  kj::StringPtr getPassword();

  kj::StringPtr getHost();
  uint16_t getPort();

  kj::String getConnectionString();

private:
  uint clientIndex;
  kj::String randomHost;
  kj::String database;
  kj::String user;
  kj::String password;
  bool registeredConnectOverride = false;
  kj::Promise<kj::Own<kj::AsyncIoStream>> connectToDb(); 
};
} // namespace workerd::api::public_beta
