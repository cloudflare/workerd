// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hyperdrive.h"
#include "sockets.h"
#include "http.h"
#include <openssl/rand.h>
#include <kj/compat/http.h>
#include <kj/encoding.h>
#include <kj/string.h>

namespace workerd::api {
Hyperdrive::Hyperdrive(uint clientIndex, kj::String database,
                       kj::String user, kj::String password, kj::String scheme)
    : clientIndex(clientIndex), database(kj::mv(database)),
      user(kj::mv(user)), password(kj::mv(password)), scheme(kj::mv(scheme)) {
        kj::byte randomBytes[16];
        KJ_ASSERT(RAND_bytes(randomBytes, sizeof(randomBytes)) == 1);
        randomHost = kj::str(kj::encodeHex(randomBytes), ".hyperdrive.local");
      }

jsg::Ref<Socket> Hyperdrive::connect(jsg::Lock& js) {
  auto connPromise = connectToDb();

  auto paf = kj::newPromiseAndFulfiller<kj::Maybe<kj::Exception>>();
  auto conn = kj::newPromisedStream(connPromise.then(
      [&f = *paf.fulfiller](kj::Own<kj::AsyncIoStream> stream) {
    f.fulfill(kj::none);
    return kj::mv(stream);
  }, [&f = *paf.fulfiller](kj::Exception e) {
    KJ_LOG(WARNING, "failed to connect to local database", e);
    f.fulfill(kj::cp(e));
    return kj::mv(e);
  }).attach(kj::mv(paf.fulfiller)));

  // TODO(someday): Support TLS? It's not at all necessary since we're connecting locally, but
  // some users may want it anyway.
  auto nullTlsStarter = kj::heap<kj::TlsStarterCallback>();
  auto sock = setupSocket(js, kj::mv(conn), kj::none, kj::mv(nullTlsStarter),
                  false, kj::str(this->randomHost), false);
  sock->handleProxyStatus(js, kj::mv(paf.promise));
  return sock;
}

kj::StringPtr Hyperdrive::getDatabase() {
  return this->database;
}

kj::StringPtr Hyperdrive::getUser() {
  return this->user;
}
kj::StringPtr Hyperdrive::getPassword() {
  return this->password;
}

kj::StringPtr Hyperdrive::getScheme() {
  return this->scheme;
}

kj::StringPtr Hyperdrive::getHost() {
  if (!registeredConnectOverride) {
    IoContext::current().getCurrentLock().getWorker().setConnectOverride(
        kj::str(this->randomHost, ":", getPort()), KJ_BIND_METHOD(*this, connect));
    registeredConnectOverride = true;
  }
  return this->randomHost;
}

// Always returns the default postgres port
uint16_t Hyperdrive::getPort() {
  return 5432;
}

kj::String Hyperdrive::getConnectionString() {
  return kj::str(getScheme(), "://", getUser(), ":", getPassword(), "@", getHost(), ":", getPort(),
                 "/", getDatabase(), "?sslmode=disable");
}

kj::Promise<kj::Own<kj::AsyncIoStream>> Hyperdrive::connectToDb() {
  auto& context = IoContext::current();
  auto service = context.getSubrequestChannel(this->clientIndex,
            true, kj::none, "hyperdrive_dev"_kjc);

  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  auto connectReq = kj::newHttpClient(*service)->connect(
    kj::str(getHost(), ":", getPort()), headers, kj::HttpConnectSettings{});

  auto status = co_await connectReq.status;

  if (status.statusCode >= 200 && status.statusCode < 300) {
    co_return kj::mv(connectReq.connection);
  }

  KJ_IF_SOME(e, status.errorBody) {
    try {
      auto errorBody = co_await e->readAllText();
      kj::throwFatalException(KJ_EXCEPTION(
          FAILED, kj::str("unexpected error connecting to database: ", errorBody)));
    } catch (const kj::Exception& e) {
      kj::throwFatalException(
          KJ_EXCEPTION(FAILED, kj::str("unexpected error connecting to database "
                                       "and couldn't read error details: ", e)));
    }
  }
  else {
    kj::throwFatalException(
        KJ_EXCEPTION(FAILED, kj::str("unexpected error connecting to database: ",
                                     status.statusText)));
  }
}
} // namespace workerd::api::public_beta
