// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The TLS policy config::TlsOptions describes, checked through makeTlsContext() so that both TLS
// engines (kj::TlsContext, and rustls under --//:io_backend=rust) are held to the same behavior.

#include "tls-network.h"

#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

// Made with openssl, valid for a century: an EC P-256 CA, an unrelated CA, an example.com
// certificate the first CA signed, and a self-signed example.com certificate over the same key as
// `openssl req -x509` makes one (so it is its own CA).
static constexpr char CA_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBmzCCAUGgAwIBAgIUFtVfWCEoNJw9tRYkhBCqWkPYQAkwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx\n"
    "MjYwODI1MTM0NzAxWjAaMRgwFgYDVQQDDA93b3JrZXJkIHRlc3QgQ0EwWTATBgcq\n"
    "hkjOPQIBBggqhkjOPQMBBwNCAAR0c/eq28LGrosC4Jp0m5O6/xS5vvetDh6lDWNG\n"
    "LfwBXbM3O4yoeSz9pUKY4cChCSL4BMldwTbepDKMCBmVMfJDo2MwYTAdBgNVHQ4E\n"
    "FgQU1HLxzohU0eGQqgstH3O0chw0cVIwHwYDVR0jBBgwFoAU1HLxzohU0eGQqgst\n"
    "H3O0chw0cVIwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAgQwCgYIKoZI\n"
    "zj0EAwIDSAAwRQIgYtV4qsw7p1xhZ1OSOWmRmjyc4LzQplblEr4jZmZO6BECIQCx\n"
    "Al0TgxgxuWjJ4FuakSJ5qfCA2BiIliBop+phth7LKw==\n"
    "-----END CERTIFICATE-----\n";

static constexpr char OTHER_CA_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBqDCCAU2gAwIBAgIURKbmFqvXbsPNYNcpsOoC3p9wz24wCgYIKoZIzj0EAwIw\n"
    "IDEeMBwGA1UEAwwVd29ya2VyZCBvdGhlciB0ZXN0IENBMCAXDTI2MDkxODEzNDcw\n"
    "MVoYDzIxMjYwODI1MTM0NzAxWjAgMR4wHAYDVQQDDBV3b3JrZXJkIG90aGVyIHRl\n"
    "c3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATjpjzuMfD87oJWO1+dmaR9\n"
    "WzThSBIax7DRYA1eMPnzYBN4sgjXlYnB3PurAMIRpcAzQwrwe7AqGhQYMoAxF7Lm\n"
    "o2MwYTAdBgNVHQ4EFgQUWIulx+0h/gDS1PpTmqr2q7zaNygwHwYDVR0jBBgwFoAU\n"
    "WIulx+0h/gDS1PpTmqr2q7zaNygwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E\n"
    "BAMCAgQwCgYIKoZIzj0EAwIDSQAwRgIhAP7lmLtP3Ag3cLs3rA8MN0rFpX1HZhzQ\n"
    "Dwto7y1IWJnCAiEArBdbUYBM9JmohOEGr7EwLio9fiYclR2rCs54VwpMyWY=\n"
    "-----END CERTIFICATE-----\n";

static constexpr char HOST_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBwzCCAWmgAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewAwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODEzNDcwMVoYDzIx\n"
    "MjYwODI1MTM0NzAxWjAWMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqGSM49\n"
    "AgEGCCqGSM49AwEHA0IABCc5+7lyl50H3MHWYyEAgNbxnIhMc6TBtR7Wvpp6XOBg\n"
    "7CzaOCZFwix4Mj8KXPoyhi7xgNQVKAgE1maTCPVPlB2jgY4wgYswDAYDVR0TAQH/\n"
    "BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwEwFgYDVR0R\n"
    "BA8wDYILZXhhbXBsZS5jb20wHQYDVR0OBBYEFOFpqeWqxNaPtlBncznOXK34Slsh\n"
    "MB8GA1UdIwQYMBaAFNRy8c6IVNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0gA\n"
    "MEUCIGkJrUe5mthCxcMYy8zUKtbDuURGIS1OqeT9xqypm2nlAiEA5jYVU9d8NUV8\n"
    "WBIiMYUVDYKLwUf1elA4zib/Qnms8DQ=\n"
    "-----END CERTIFICATE-----\n";

static constexpr char SELF_SIGNED_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBnDCCAUGgAwIBAgIUS2xurXFfrYHJlrcRk6lqv44ritUwCgYIKoZIzj0EAwIw\n"
    "FjEUMBIGA1UEAwwLZXhhbXBsZS5jb20wIBcNMjYwOTE4MTM0OTAwWhgPMjEyNjA4\n"
    "MjUxMzQ5MDBaMBYxFDASBgNVBAMMC2V4YW1wbGUuY29tMFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEJzn7uXKXnQfcwdZjIQCA1vGciExzpMG1Hta+mnpc4GDsLNo4\n"
    "JkXCLHgyPwpc+jKGLvGA1BUoCATWZpMI9U+UHaNrMGkwHQYDVR0OBBYEFOFpqeWq\n"
    "xNaPtlBncznOXK34SlshMB8GA1UdIwQYMBaAFOFpqeWqxNaPtlBncznOXK34Slsh\n"
    "MA8GA1UdEwEB/wQFMAMBAf8wFgYDVR0RBA8wDYILZXhhbXBsZS5jb20wCgYIKoZI\n"
    "zj0EAwIDSQAwRgIhAP8xzPMa6tuqL9p3AIKUn1eABTfZv7o/VmiEvtCK3rU2AiEA\n"
    "9S5OQ1DXe5Nt5ZD5h3qOJF1IN9uR5Jb0kf1bl7lKIGI=\n"
    "-----END CERTIFICATE-----\n";

static constexpr char HOST_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgb9K+RSaSGrigmGFU\n"
    "Ucyf1+0zBpj3gEnQ9LCKN9gIFhqhRANCAAQnOfu5cpedB9zB1mMhAIDW8ZyITHOk\n"
    "wbUe1r6aelzgYOws2jgmRcIseDI/Clz6MoYu8YDUFSgIBNZmkwj1T5Qd\n"
    "-----END PRIVATE KEY-----\n";

// Signed by CA_CERT over HOST_KEY: a 127.0.0.1 server certificate, and a client certificate over
// its own key.
static constexpr char IP_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBuzCCAWCgAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewEwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODE4MTcxNVoYDzIx\n"
    "MjYwODI1MTgxNzE1WjAUMRIwEAYDVQQDDAkxMjcuMC4wLjEwWTATBgcqhkjOPQIB\n"
    "BggqhkjOPQMBBwNCAAQnOfu5cpedB9zB1mMhAIDW8ZyITHOkwbUe1r6aelzgYOws\n"
    "2jgmRcIseDI/Clz6MoYu8YDUFSgIBNZmkwj1T5Qdo4GHMIGEMAwGA1UdEwEB/wQC\n"
    "MAAwDgYDVR0PAQH/BAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMA8GA1UdEQQI\n"
    "MAaHBH8AAAEwHQYDVR0OBBYEFOFpqeWqxNaPtlBncznOXK34SlshMB8GA1UdIwQY\n"
    "MBaAFNRy8c6IVNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0kAMEYCIQD2WsDQ\n"
    "/ypIhVIOGVqf7figuf4YxauEMOrrZ3kxn4cMPgIhANTmbGb6b3/sKXkyBTrTieJt\n"
    "aiqt5Ls2/3+/Fzf04N2M\n"
    "-----END CERTIFICATE-----\n";

static constexpr char CLIENT_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBsTCCAVegAwIBAgIUPPhG/ycBgb1qLtsWGGJiMhxtewIwCgYIKoZIzj0EAwIw\n"
    "GjEYMBYGA1UEAwwPd29ya2VyZCB0ZXN0IENBMCAXDTI2MDkxODE4MTcxNVoYDzIx\n"
    "MjYwODI1MTgxNzE1WjAeMRwwGgYDVQQDDBN3b3JrZXJkIHRlc3QgY2xpZW50MFkw\n"
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEf1+6QE6J0A4kaCqmELiGf1WE8ctlQQ3o\n"
    "O9HMPFuWxwmqyHWx4EgW7p/NVc0cLvpDO+mlq5t3ty4RKjGN6ASs8aN1MHMwDAYD\n"
    "VR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCB4AwEwYDVR0lBAwwCgYIKwYBBQUHAwIw\n"
    "HQYDVR0OBBYEFP5LB3a8ceMElm+R3T7kMQwSgqneMB8GA1UdIwQYMBaAFNRy8c6I\n"
    "VNHhkKoLLR9ztHIcNHFSMAoGCCqGSM49BAMCA0gAMEUCICCikKtYhV/a121v/+JC\n"
    "zCTWoIZVpC4+0hwXc9FMKu0lAiEA+Mfwtk957dSWffB+ntSCNsNr9a2UeyAwJY5c\n"
    "ak5n5qY=\n"
    "-----END CERTIFICATE-----\n";

static constexpr char CLIENT_KEY[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjxQourjhr8gzNJrQ\n"
    "GIdkArapVpTBw4yb4ZN0o1DdXBmhRANCAAR/X7pATonQDiRoKqYQuIZ/VYTxy2VB\n"
    "Deg70cw8W5bHCarIdbHgSBbun81VzRwu+kM76aWrm3e3LhEqMY3oBKzx\n"
    "-----END PRIVATE KEY-----\n";

using Configure = kj::Function<void(config::TlsOptions::Builder)>;

kj::Own<kj::SecureNetworkWrapper> makeContext(Configure configure) {
  capnp::MallocMessageBuilder message;
  auto options = message.initRoot<config::TlsOptions>();
  configure(options);
  return makeTlsContext(options, [](kj::String error) { KJ_FAIL_EXPECT(error); });
}

void serveExampleCom(config::TlsOptions::Builder options, kj::StringPtr certificate = HOST_CERT) {
  auto keypair = options.initKeypair();
  keypair.setPrivateKey(HOST_KEY);
  keypair.setCertificateChain(certificate);
}

kj::Promise<void> echo(kj::Promise<kj::Own<kj::AsyncIoStream>> handshake) {
  auto stream = co_await handshake;
  kj::byte buffer[5];
  co_await stream->read(buffer, sizeof(buffer));
  co_await stream->write(kj::arrayPtr(buffer));
}

// The client's failure, if any.
kj::Promise<kj::Maybe<kj::Exception>> ping(kj::Promise<kj::Own<kj::AsyncIoStream>> handshake) {
  try {
    auto stream = co_await handshake;
    co_await stream->write("hello"_kjb);
    kj::byte buffer[5];
    co_await stream->read(buffer, sizeof(buffer));
    KJ_EXPECT(kj::arrayPtr(buffer) == "hello"_kjb);
    co_return kj::none;
  } catch (...) {
    co_return kj::getCaughtExceptionAsKj();
  }
}

// Connects a client to a server over a pipe; returns the client's failure, if any.
kj::Maybe<kj::Exception> connect(
    Configure server, Configure client, kj::StringPtr hostname = "example.com") {
  auto io = kj::setupAsyncIo();
  auto serverContext = makeContext(kj::mv(server));
  auto clientContext = makeContext(kj::mv(client));
  auto pipe = io.provider->newTwoWayPipe();
  auto served = echo(kj::evalNow([&]() {
    return serverContext->wrapServer(kj::mv(pipe.ends[0]));
  })).eagerlyEvaluate([](kj::Exception&&) {});
  return ping(clientContext->wrapClient(kj::mv(pipe.ends[1]), hostname)).wait(io.waitScope);
}

void trust(config::TlsOptions::Builder options, kj::StringPtr certificate) {
  options.initTrustedCertificates(1).set(0, certificate);
}

KJ_TEST("TLS: a certificate a trusted CA signed is accepted") {
  KJ_EXPECT(connect([](auto options) { serveExampleCom(options); },
                [](auto options) { trust(options, CA_CERT); }) == kj::none);
}

KJ_TEST("TLS: with nothing trusted, every certificate is refused") {
  KJ_EXPECT(connect([](auto options) { serveExampleCom(options); }, [](auto) {}) != kj::none);
  KJ_EXPECT(connect([](auto options) { serveExampleCom(options, SELF_SIGNED_CERT); },
                [](auto) {}) != kj::none);
}

KJ_TEST("TLS: a certificate an untrusted CA signed is refused") {
  KJ_EXPECT(connect([](auto options) { serveExampleCom(options); },
                [](auto options) { trust(options, OTHER_CA_CERT); }) != kj::none);
}

KJ_TEST("TLS: the certificate must name the host") {
  KJ_EXPECT(connect([](auto options) { serveExampleCom(options); },
                [](auto options) { trust(options, CA_CERT); }, "wrong.example.com") != kj::none);
}

KJ_TEST("TLS: a trusted self-signed certificate is trusted directly") {
  auto server = [](auto options) { serveExampleCom(options, SELF_SIGNED_CERT); };
  auto client = [](auto options) { trust(options, SELF_SIGNED_CERT); };
  KJ_EXPECT(connect(server, client) == kj::none);
  KJ_EXPECT(connect(server, client, "wrong.example.com") != kj::none);
}

KJ_TEST("TLS: minVersion and cipherList configure both sides") {
  auto policy = [](config::TlsOptions::Builder options) {
    options.setMinVersion(config::TlsOptions::Version::TLS1_DOT3);
    options.setCipherList("ECDHE-ECDSA-AES256-GCM-SHA384");
  };
  KJ_EXPECT(connect([&](auto options) {
    serveExampleCom(options);
    policy(options);
  }, [&](auto options) {
    trust(options, CA_CERT);
    policy(options);
  }) == kj::none);
}

// Recovered from kj-hyper's former TLS suite, rewritten against makeTlsContext().

KJ_TEST("TLS: an IP address is verified against the certificate's IP names") {
  auto server = [](config::TlsOptions::Builder options) { serveExampleCom(options, IP_CERT); };
  auto client = [](config::TlsOptions::Builder options) { trust(options, CA_CERT); };
  KJ_EXPECT(connect(server, client, "127.0.0.1") == kj::none);
  KJ_EXPECT(connect(server, client, "127.0.0.2") != kj::none);
}

KJ_TEST("TLS: requireClientCerts accepts a trusted client certificate and refuses none") {
  auto server = [](config::TlsOptions::Builder options) {
    serveExampleCom(options);
    trust(options, CA_CERT);
    options.setRequireClientCerts(true);
  };
  KJ_EXPECT(connect(server, [](config::TlsOptions::Builder options) {
    trust(options, CA_CERT);
    auto keypair = options.initKeypair();
    keypair.setPrivateKey(CLIENT_KEY);
    keypair.setCertificateChain(CLIENT_CERT);
  }) == kj::none);
  KJ_EXPECT(connect(server, [](config::TlsOptions::Builder options) { trust(options, CA_CERT); }) !=
      kj::none);
}

KJ_TEST("TLS: a server without a keypair cannot complete a handshake") {
  KJ_EXPECT(connect([](config::TlsOptions::Builder options) { trust(options, CA_CERT); },
                [](config::TlsOptions::Builder options) { trust(options, CA_CERT); }) != kj::none);
}

kj::Maybe<kj::Exception> configurationFailure(Configure configure) {
  return kj::runCatchingExceptions([&]() { makeContext(kj::mv(configure)); });
}

KJ_TEST("TLS: an invalid trusted certificate is refused at configuration") {
  KJ_EXPECT(configurationFailure([](config::TlsOptions::Builder options) {
    trust(options, "-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n");
  }) != kj::none);
}

class EchoService final: public kj::HttpService {
 public:
  explicit EchoService(kj::HttpHeaderTable& table): table(table) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    if (headers.isWebSocket()) {
      auto ws = response.acceptWebSocket(kj::HttpHeaders(table));
      auto message = co_await ws->receive();
      co_await ws->send(message.get<kj::String>());
      co_await ws->close(1000, "");
      co_return;
    }
    auto out = response.send(200, "OK", kj::HttpHeaders(table), url.size());
    co_await out->write(url.asBytes());
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    response.accept(200, "OK", kj::HttpHeaders(table));
    kj::byte buffer[4];
    co_await connection.read(buffer, sizeof(buffer));
    co_await connection.write(kj::arrayPtr(buffer));
  }

 private:
  kj::HttpHeaderTable& table;
};

class FixedEntropy final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    buffer.fill(4);
  }
};

KJ_TEST("TLS: HTTP keep-alive, a WebSocket and a CONNECT tunnel ride inside TLS") {
  auto io = kj::setupAsyncIo();
  auto& ws = io.waitScope;
  auto serverContext =
      makeContext([](config::TlsOptions::Builder options) { serveExampleCom(options); });
  auto clientContext =
      makeContext([](config::TlsOptions::Builder options) { trust(options, CA_CERT); });
  kj::HttpHeaderTable table;
  EchoService service(table);
  kj::HttpServer server(io.provider->getTimer(), table, service);
  FixedEntropy entropy;
  kj::HttpHeaders headers(table);
  headers.setPtr(kj::HttpHeaderId::HOST, "example.com");

  auto newTlsClient = [&]() {
    auto pipe = io.provider->newTwoWayPipe();
    auto served = serverContext->wrapServer(kj::mv(pipe.ends[0]))
                      .then([&](kj::Own<kj::AsyncIoStream> stream) {
      return server.listenHttp(kj::mv(stream));
    }).eagerlyEvaluate(nullptr);
    auto stream = clientContext->wrapClient(kj::mv(pipe.ends[1]), "example.com").wait(ws);
    auto client = kj::newHttpClient(table, *stream, {.entropySource = entropy});
    return client.attach(kj::mv(stream), kj::mv(served));
  };

  {
    auto client = newTlsClient();
    for (auto path: {"/one"_kj, "/two"_kj}) {
      auto response =
          client->request(kj::HttpMethod::GET, path, headers, uint64_t(0)).response.wait(ws);
      KJ_EXPECT(response.body->readAllText().wait(ws) == path);
    }
  }
  {
    auto client = newTlsClient();
    auto response = client->openWebSocket("/ws", headers).wait(ws);
    KJ_ASSERT(response.statusCode == 101);
    auto& socket = *response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
    socket.send("over tls"_kj).wait(ws);
    KJ_EXPECT(socket.receive().wait(ws).get<kj::String>() == "over tls");
  }
  {
    auto client = newTlsClient();
    auto request = client->connect("tunnel.example:1", headers, {});
    KJ_EXPECT(request.status.wait(ws).statusCode == 200);
    request.connection->write("ping"_kjb).wait(ws);
    kj::byte buffer[4];
    request.connection->read(buffer, sizeof(buffer)).wait(ws);
    KJ_EXPECT(kj::arrayPtr(buffer) == "ping"_kjb);
  }
}

kj::Promise<kj::Own<kj::AsyncIoStream>> readOneByte(
    kj::Promise<kj::Own<kj::AsyncIoStream>> handshake) {
  auto stream = co_await handshake;
  kj::byte buffer[1];
  co_await stream->read(buffer, 1);
  co_return stream;
}

// A client and server pair over a socket pair, handshaken by exchanging one byte (the server's
// side of the handshake may run on its first I/O).
struct TlsPair {
  kj::AsyncIoContext io = kj::setupAsyncIo();
  kj::Own<kj::SecureNetworkWrapper> serverContext =
      makeContext([](config::TlsOptions::Builder options) { serveExampleCom(options); });
  kj::Own<kj::SecureNetworkWrapper> clientContext =
      makeContext([](config::TlsOptions::Builder options) { trust(options, CA_CERT); });
  kj::TwoWayPipe pipe = io.provider->newTwoWayPipe();
  kj::Own<kj::AsyncIoStream> client;
  kj::Own<kj::AsyncIoStream> server;

  TlsPair() {
    auto serverHandshake = serverContext->wrapServer(kj::mv(pipe.ends[0]));
    auto clientHandshake = clientContext->wrapClient(kj::mv(pipe.ends[1]), "example.com");
    auto served = readOneByte(kj::mv(serverHandshake)).eagerlyEvaluate(nullptr);
    client = clientHandshake.wait(io.waitScope);
    client->write("x"_kjb).wait(io.waitScope);
    server = served.wait(io.waitScope);
  }
};

KJ_TEST("TLS stream: a read and a write in flight at once both complete") {
  TlsPair p;
  kj::byte buffer[5];
  auto read = p.client->read(buffer, sizeof(buffer)).eagerlyEvaluate(nullptr);
  auto write = p.client->write("hello"_kjb).eagerlyEvaluate(nullptr);
  kj::byte serverBuffer[5];
  p.server->read(serverBuffer, sizeof(serverBuffer)).wait(p.io.waitScope);
  p.server->write("world"_kjb).wait(p.io.waitScope);
  write.wait(p.io.waitScope);
  read.wait(p.io.waitScope);
  KJ_EXPECT(kj::arrayPtr(serverBuffer) == "hello"_kjb);
  KJ_EXPECT(kj::arrayPtr(buffer) == "world"_kjb);
}

KJ_TEST("TLS stream: abortRead() ends a parked read") {
  TlsPair p;
  kj::byte buffer[5];
  auto read = p.client->tryRead(buffer, 1, sizeof(buffer))
                  .then([](size_t n) { return n; }, [](kj::Exception&&) {
    return size_t(0);
  }).eagerlyEvaluate(nullptr);
  KJ_EXPECT(!read.poll(p.io.waitScope));
  p.client->abortRead();
  KJ_EXPECT(read.wait(p.io.waitScope) == 0);
}

KJ_TEST("TLS stream: whenWriteDisconnected() observes the peer going away") {
  TlsPair p;
  auto disconnected = p.client->whenWriteDisconnected();
  KJ_EXPECT(!disconnected.poll(p.io.waitScope));
  p.server = nullptr;
  disconnected.wait(p.io.waitScope);
}

}  // namespace
}  // namespace workerd::server
