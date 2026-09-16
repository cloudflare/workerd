// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// End-to-end tests for the hyper-backed outbound HTTPS client: a real kj::HttpServer behind
// kj::TlsContext (OpenSSL) serves on a loopback socket on the test's KJ event loop, while the
// hyper client (rustls, its tasks on the same thread's kj-rs-tokio loop runtime) connects to it
// over TLS through
// the kj::HttpService / kj::HttpClient interfaces. This cross-implementation setup (rustls
// client <-> OpenSSL server) validates interoperability as well as the TlsOptions mapping
// policy documented in kj-hyper/tls.rs.

#include "kj-hyper/tests/test-harness.h"

#include <kj-hyper/hyper-http.h>
#include <kj-rs-io/async-io.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/test.h>

#if !_WIN32
#include <sys/socket.h>
#endif

namespace {

using namespace kj_hyper_test;

// =======================================================================================
// Test certificates (embedded fixtures, generated with `openssl` -- EC P-256, valid to 2125;
// kj's own tls-test.c++ certificates cannot be reused here because they predate the
// subjectAltName requirement: rustls/webpki does not accept CN-only certificates).
//
//   test CA          (CA:TRUE)                          -> CA_CERT
//   server cert      (SAN: example.com, localhost, 127.0.0.1; signed by test CA)
//   RSA server cert  (same SANs; RSA-2048; for the ECDHE-RSA cipherList test)
//   self-signed cert (same SANs; CA:FALSE, its own key) -> directly-trusted-cert test
//   client cert      (clientAuth; signed by test CA)    -> client-certificate test

static constexpr kj::StringPtr CA_CERT = R"(-----BEGIN CERTIFICATE-----
MIIB0DCCAXWgAwIBAgIUbju3Cq3JY5H4Ls1CvCzG6GRZu5wwCgYIKoZIzj0EAwIw
NDEXMBUGA1UECgwOa2otaHlwZXIgdGVzdHMxGTAXBgNVBAMMEGtqLWh5cGVyIHRl
c3QgQ0EwIBcNMjYwODAzMTcwNDE3WhgPMjEyNTAyMjUxNzA0MTdaMDQxFzAVBgNV
BAoMDmtqLWh5cGVyIHRlc3RzMRkwFwYDVQQDDBBrai1oeXBlciB0ZXN0IENBMFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEnwj1GLB+jA3Dz5ZX2v5glwljMVKSZzFJ
UAX6FFK2uTO1ff3Aq1RuHVNInI2CkFqRoC0VfCCXqlMdOWr9dUnYV6NjMGEwHQYD
VR0OBBYEFNY6wC9f3dwzcMfcnSiO2ukm25t5MB8GA1UdIwQYMBaAFNY6wC9f3dwz
cMfcnSiO2ukm25t5MA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoG
CCqGSM49BAMCA0kAMEYCIQDJBOf77rK1/ObLtMsLcFClIDxd1vNM1aUsmi18oDyZ
GgIhAOi/HCH82dZIVx4eom70lx4kb059gBDTY3WAUjM2BQv6
-----END CERTIFICATE-----
)"_kj;

static constexpr kj::StringPtr SERVER_CERT = R"(-----BEGIN CERTIFICATE-----
MIICADCCAaegAwIBAgIUQjjehxDcIIKU4Vag+VJZA7pc2lYwCgYIKoZIzj0EAwIw
NDEXMBUGA1UECgwOa2otaHlwZXIgdGVzdHMxGTAXBgNVBAMMEGtqLWh5cGVyIHRl
c3QgQ0EwIBcNMjYwODAzMTcwNDE3WhgPMjEyNTAyMjUxNzA0MTdaMC8xFzAVBgNV
BAoMDmtqLWh5cGVyIHRlc3RzMRQwEgYDVQQDDAtleGFtcGxlLmNvbTBZMBMGByqG
SM49AgEGCCqGSM49AwEHA0IABNs6peyuiy/gzGITcdj6o8n1qbc9xbic6/e5TAGW
YHReItb6Tf5UefDOdfdjoAXCRmf34ePPk9KxuhX9cEcqswCjgZkwgZYwCQYDVR0T
BAIwADALBgNVHQ8EBAMCBaAwEwYDVR0lBAwwCgYIKwYBBQUHAwEwJwYDVR0RBCAw
HoILZXhhbXBsZS5jb22CCWxvY2FsaG9zdIcEfwAAATAdBgNVHQ4EFgQUxL89fgT9
JDGuSYYww4KEKQmhelIwHwYDVR0jBBgwFoAU1jrAL1/d3DNwx9ydKI7a6Sbbm3kw
CgYIKoZIzj0EAwIDRwAwRAIgUG+Kw3Uz0V3uFjOJcy+Gqli7hGHfp26VnndieGqv
uaMCICtLJ93SczjNS+BgY5kja8/DSgJT4Fpi6UCdEWJUlfAu
-----END CERTIFICATE-----
)"_kj;

static constexpr kj::StringPtr SERVER_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgzVojtw+gi05TFkcK
V7uygifeJi2n5mPdLfsj1GOUBZuhRANCAATbOqXsrosv4MxiE3HY+qPJ9am3PcW4
nOv3uUwBlmB0XiLW+k3+VHnwznX3Y6AFwkZn9+Hjz5PSsboV/XBHKrMA
-----END PRIVATE KEY-----
)"_kj;

static constexpr kj::StringPtr SELF_SIGNED_CERT = R"(-----BEGIN CERTIFICATE-----
MIICFDCCAbqgAwIBAgIUGMzmYt4s/qVWEnfv+f7bx5wzSBYwCgYIKoZIzj0EAwIw
OzEXMBUGA1UECgwOa2otaHlwZXIgdGVzdHMxIDAeBgNVBAMMF3NlbGYtc2lnbmVk
LmV4YW1wbGUuY29tMCAXDTI2MDgwMzE3MDQxN1oYDzIxMjUwMjI1MTcwNDE3WjA7
MRcwFQYDVQQKDA5rai1oeXBlciB0ZXN0czEgMB4GA1UEAwwXc2VsZi1zaWduZWQu
ZXhhbXBsZS5jb20wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATv0ptNqpk6xDIS
dDF/r5Oq5lhnLiF4PMSJD5bI64B5ByZhE3R9RzbLHoG5AAmio5CrTTymW12TOu2J
mdCO3MUmo4GZMIGWMB0GA1UdDgQWBBRLbo0UtGoSMeMAG/1diJIC187UvTAfBgNV
HSMEGDAWgBRLbo0UtGoSMeMAG/1diJIC187UvTAJBgNVHRMEAjAAMAsGA1UdDwQE
AwIFoDATBgNVHSUEDDAKBggrBgEFBQcDATAnBgNVHREEIDAeggtleGFtcGxlLmNv
bYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49BAMCA0gAMEUCIGj2zHcLoEerLJeT
kyoIPluABfjQlcUDugH5pN0jw5nOAiEAyWtHR0Lwp59BR054o9+YiH4/eZp8xsuA
qOVyvLBrSSo=
-----END CERTIFICATE-----
)"_kj;

static constexpr kj::StringPtr SELF_SIGNED_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgKMgi/kV7M75jZM1x
2Yh0y7onfvCDTWUali78hQR2lDuhRANCAATv0ptNqpk6xDISdDF/r5Oq5lhnLiF4
PMSJD5bI64B5ByZhE3R9RzbLHoG5AAmio5CrTTymW12TOu2JmdCO3MUm
-----END PRIVATE KEY-----
)"_kj;

static constexpr kj::StringPtr CLIENT_CERT = R"(-----BEGIN CERTIFICATE-----
MIIB3zCCAYWgAwIBAgIUQjjehxDcIIKU4Vag+VJZA7pc2lcwCgYIKoZIzj0EAwIw
NDEXMBUGA1UECgwOa2otaHlwZXIgdGVzdHMxGTAXBgNVBAMMEGtqLWh5cGVyIHRl
c3QgQ0EwIBcNMjYwODAzMTcwNDE3WhgPMjEyNTAyMjUxNzA0MTdaMDgxFzAVBgNV
BAoMDmtqLWh5cGVyIHRlc3RzMR0wGwYDVQQDDBRrai1oeXBlciB0ZXN0IGNsaWVu
dDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABAjo3ayPHaCLj6FECWp+FGVYTa86
Tsjab/18ncAwAn5bK46dXjXq7L6yk80qt0bwnAeRjBTJI4Sx7LjMEO4VPGijbzBt
MAkGA1UdEwQCMAAwCwYDVR0PBAQDAgeAMBMGA1UdJQQMMAoGCCsGAQUFBwMCMB0G
A1UdDgQWBBQ4tbaGL7645svlhVJuRDm2Xs6pHDAfBgNVHSMEGDAWgBTWOsAvX93c
M3DH3J0ojtrpJtubeTAKBggqhkjOPQQDAgNIADBFAiEAgNn/HkiwG5thB/MuANqp
W2ABWd0ZxYY2AHYDvZtsjakCICnPN9uKCs9Hj6NSDkplLmaEL3bImsseyZ0f8qQ1
3Kah
-----END CERTIFICATE-----
)"_kj;

static constexpr kj::StringPtr CLIENT_KEY = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgvMA1FywMiFlGhdkm
mpIfk5DVC2r1YWiS3jlgXu9dAeKhRANCAAQI6N2sjx2gi4+hRAlqfhRlWE2vOk7I
2m/9fJ3AMAJ+WyuOnV416uy+spPNKrdG8JwHkYwUySOEsey4zBDuFTxo
-----END PRIVATE KEY-----
)"_kj;

static constexpr kj::StringPtr RSA_SERVER_CERT = R"(-----BEGIN CERTIFICATE-----
MIICzTCCAnKgAwIBAgIUQjjehxDcIIKU4Vag+VJZA7pc2lgwCgYIKoZIzj0EAwIw
NDEXMBUGA1UECgwOa2otaHlwZXIgdGVzdHMxGTAXBgNVBAMMEGtqLWh5cGVyIHRl
c3QgQ0EwIBcNMjYwODAzMTcwNDE3WhgPMjEyNTAyMjUxNzA0MTdaMC8xFzAVBgNV
BAoMDmtqLWh5cGVyIHRlc3RzMRQwEgYDVQQDDAtleGFtcGxlLmNvbTCCASIwDQYJ
KoZIhvcNAQEBBQADggEPADCCAQoCggEBALNbkAxoGHWjaVGQQG2qLQiYiApvyGGL
e7RldOix5xS5ekw5EmG+T8xjR4qyMTZHebEs25aN6f/iAE0Y6IboNwij3OCTpuFX
gSYS+dJMqO/s8jwL8HlcBbO3JpCEB7n6m+18jH/VUygk3w501z+Epvtl33o2dTtc
Wae00omATk3TmLSbj2lfxlYCcgQls/d6QNH4+n5w0F0rWN+3hV3Rm6CFBDNdYS7/
mMgPzYA5yaD2BllsttUeIEkv6C6sZtqZ/9erTJjsR1jrhAonmkP4M0iXIIxMk5fp
hFxfJGX7qInmIVYIJiE4wRmYBFX+TOMRfKUP6+eWV640gtmOsnXMLNMCAwEAAaOB
mTCBljAJBgNVHRMEAjAAMAsGA1UdDwQEAwIFoDATBgNVHSUEDDAKBggrBgEFBQcD
ATAnBgNVHREEIDAeggtleGFtcGxlLmNvbYIJbG9jYWxob3N0hwR/AAABMB0GA1Ud
DgQWBBRR2Wog7b3wp3WvZiXSLTdC+axY8TAfBgNVHSMEGDAWgBTWOsAvX93cM3DH
3J0ojtrpJtubeTAKBggqhkjOPQQDAgNJADBGAiEA0iQ2eFuFuJAqM+ROxOJuTPCO
8peBVr0CWqELVD7UJBgCIQDZr57Hy8qAf6VEIS+Us7sjOo67XCHpGnKcEtufK7Rh
Hg==
-----END CERTIFICATE-----
)"_kj;

static constexpr kj::StringPtr RSA_SERVER_KEY = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCzW5AMaBh1o2lR
kEBtqi0ImIgKb8hhi3u0ZXTosecUuXpMORJhvk/MY0eKsjE2R3mxLNuWjen/4gBN
GOiG6DcIo9zgk6bhV4EmEvnSTKjv7PI8C/B5XAWztyaQhAe5+pvtfIx/1VMoJN8O
dNc/hKb7Zd96NnU7XFmntNKJgE5N05i0m49pX8ZWAnIEJbP3ekDR+Pp+cNBdK1jf
t4Vd0ZughQQzXWEu/5jID82AOcmg9gZZbLbVHiBJL+gurGbamf/Xq0yY7EdY64QK
J5pD+DNIlyCMTJOX6YRcXyRl+6iJ5iFWCCYhOMEZmARV/kzjEXylD+vnlleuNILZ
jrJ1zCzTAgMBAAECggEAQlPeFXh5cyeXTx51dRi1xrQBuzpTdB9NBA1xCoyf1D4z
R/TjI1pp0D/2uQfkwxnzmt3n5ulcW9CMI/bOJvXBRGS0zihMUpE1pYrm8CqBXxjV
C/euWnL5eNiGXnM1GEXhXBXvL2cIdZQNGR+Eamz4A4hM+Lki6Wzhiu+H3Gmii4zR
ckUrQySibJeQessbcSpQBGZopA9cVn4ioBY2+Mw7AoN9lu4UbzBSHHgsMxVwAu0Z
zSUeUGn0XwVqkDftr4nVRYh6MOROQEXp3hUTh5iwClP2C+T89C882VPtyWjcCH+g
vl7PttmT8Rq/EtovL7BYoOSRORaTVLf5OIWpXWEsYQKBgQDsP4JfDB4eYEJw0ije
I4pdFX9YK9JsJsaNsdciPONvia3wlvwekD3t/idHW3gwvmjA6IL+BVScaiz+alwK
6KWD1WU7dlhb6Cw0bVTNgTut9Aai6M4K5f0tcOHoGyoCSHp+FKHUngOnH13bj9jM
0tmmRqygi3ucoJWkqXcuaqOlvQKBgQDCWmkHChCgznpHZFAsWv7gU9XmUjI5ZHay
RRBEjJhHDwodbgiqi3CSyBwQOnTAeHKEwHu/bc3GMflOLkDq2CUFwSVo4FgEPFql
Ye+5klByM3CArqimQiqw/lM0di7XIDbv9miT9MPWujsAnlUjWC1FbxtRd/4Jpdvm
gZBgZMndzwKBgFIFJp0bFuXS455JyHh+/E2e5gxVgoOb3rlY8ejoHMofka0Crltq
FqYm47opbov1v2aiEsBgV2TKk/5sLLJ43OXtU20WhEy8SYSWJKgIYxDctOUgUFCG
UqTRGQ09SRNr2GMnhJvNlFUUVcChq1JJLdlFli9S3cKeGfYGA+OwkEo9AoGBALi0
y9QQ0LTDBpsKztcYs9nmUwbNX0YEIHpUAJ3lMGN3+5j0le/fDWGyJFmX9JVm4w8f
D3xikomlvNV8R93pBWZupWsCqyN7aHp8lSO71hByqpzhYQ6BG6mSSuk02FdDGnL7
r2/N0sEjxCdWUnuAtaWjbkcCzr/EjTSINfomT2MpAoGBANjTBEZLo4e7h5obbSai
z3s3dRsy6mb3xx2nhKZ9852fgyXVVDTh0/75HdStE2lMgCrVeUD34ocUidxrrO7n
+LO5yY/Jx7eWPnTXn/wXfa/bZCsgOr916Ra5BQLEuM/c2FtN1zkMdZ0UnRzN88az
X1aXOyZUFxuvwUNZyxalHtwq
-----END PRIVATE KEY-----
)"_kj;

// =======================================================================================

void ensureTokioInitialized() {
  static bool initialized = []() { return true; }();
  (void)initialized;
}

// The test upstream: /hello, WebSocket echo on /ws-echo, CONNECT echo for "echo-host:1234".
class TestHttpService final: public kj::HttpService {
 public:
  explicit TestHttpService(kj::HttpHeaderTable& table): table(table) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    if (headers.isWebSocket()) {
      auto ws = response.acceptWebSocket(kj::HttpHeaders(table));
      for (;;) {
        auto message = co_await ws->receive();
        KJ_SWITCH_ONEOF(message) {
          KJ_CASE_ONEOF(text, kj::String) {
            co_await ws->send(text.asArray());
          }
          KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
            co_await ws->send(data.asPtr());
          }
          KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
            co_await ws->close(close.code, close.reason);
            co_return;
          }
        }
      }
    }
    kj::HttpHeaders respHeaders(table);
    auto body = "Hello TLS!"_kj;
    auto out = response.send(200, "OK", respHeaders, uint64_t(body.size()));
    co_await out->write(body.asBytes());
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_ASSERT(host == "echo-host:1234", host);
    response.accept(200, "OK", kj::HttpHeaders(table));
    auto buf = kj::heapArray<kj::byte>(4096);
    for (;;) {
      auto n = co_await connection.tryRead(buf.begin(), 1, buf.size());
      if (n == 0) break;
      co_await connection.write(buf.first(n));
    }
    connection.shutdownWrite();
  }

 private:
  kj::HttpHeaderTable& table;
};

using workerd::rust::kj_hyper::TlsClientOptions;
using workerd::rust::kj_hyper::TlsMinVersion;

// The client-side TLS parameters for a test.
struct ClientTls {
  bool trustCa = true;                    // put CA_CERT in trusted_certificates
  kj::Maybe<kj::StringPtr> extraTrusted;  // additional trusted PEM (e.g. SELF_SIGNED_CERT)
  bool presentClientCert = false;         // present CLIENT_CERT/CLIENT_KEY
  TlsMinVersion minVersion = TlsMinVersion::GOOD_DEFAULT;
  kj::StringPtr cipherList = nullptr;
  kj::StringPtr expectedHostname = "example.com";
};

TlsClientOptions makeTlsClientOptions(const ClientTls& tls) {
  TlsClientOptions options;
  options.trust_system_roots = false;
  if (tls.trustCa) {
    options.trusted_certificates.push_back(::rust::String(CA_CERT.begin(), CA_CERT.size()));
  }
  KJ_IF_SOME(extra, tls.extraTrusted) {
    options.trusted_certificates.push_back(::rust::String(extra.begin(), extra.size()));
  }
  if (tls.presentClientCert) {
    options.certificate_chain = ::rust::String(CLIENT_CERT.begin(), CLIENT_CERT.size());
    options.private_key = ::rust::String(CLIENT_KEY.begin(), CLIENT_KEY.size());
  }
  options.min_version = tls.minVersion;
  if (tls.cipherList != nullptr) {
    options.cipher_list = ::rust::String(tls.cipherList.begin(), tls.cipherList.size());
  }
  return options;
}

// The server-side (kj/OpenSSL) parameters for a test.
struct ServerTls {
  kj::StringPtr cert = SERVER_CERT;
  kj::StringPtr key = SERVER_KEY;
  bool verifyClients = false;
};

struct TlsFixture {
  explicit TlsFixture(ServerTls serverTls = {}, ClientTls clientTls = {}) {
    ensureTokioInitialized();

    table = kj::HttpHeaderTable::Builder().build();
    service = kj::heap<TestHttpService>(*table);

    // --- The kj (OpenSSL) TLS server.
    kj::TlsContext::Options options;
    options.useSystemTrustStore = false;
    kj::TlsKeypair keypair{.privateKey = kj::TlsPrivateKey(serverTls.key),
      .certificate = kj::TlsCertificate(serverTls.cert)};
    options.defaultKeypair = keypair;
    kj::TlsCertificate caCert(CA_CERT);
    if (serverTls.verifyClients) {
      options.verifyClients = true;
      options.trustedCertificates = kj::arrayPtr(&caCert, 1);
    }
    tls = kj::heap<kj::TlsContext>(kj::mv(options));

    auto addr = io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(io.waitScope);
    auto tcp = kj::heap<CountingReceiver>(addr->listen());
    tcpListener = tcp;
    port = tcp->getPort();
    listener = tls->wrapPort(kj::mv(tcp));

    server = kj::heap<kj::HttpServer>(io.provider->getTimer(), *table, *service,
        kj::HttpServerSettings{
          .webSocketCompressionMode = kj::HttpServerSettings::MANUAL_COMPRESSION});
    listenTask =
        server->listenHttp(*listener).eagerlyEvaluate([](kj::Exception&& e) { KJ_LOG(ERROR, e); });

    // --- The hyper (rustls) HTTPS client.
    auto config = workerd::rust::kj_hyper::newHyperTlsClientConfig(makeTlsClientOptions(clientTls));
    clientService = workerd::rust::kj_hyper::newHyperHttpsService(*table, "127.0.0.1", port,
        *config, clientTls.expectedHostname, workerd::rust::kj_hyper::newAllowAllHyperPeerFilter());
    client = kj::newHttpClient(*clientService);
  }

  kj::HttpHeaders makeHeaders() {
    kj::HttpHeaders headers(*table);
    headers.set(kj::HttpHeaderId::HOST, kj::str("example.com"));
    return headers;
  }

  kj::String get(kj::StringPtr url) {
    auto req = client->request(kj::HttpMethod::GET, url, makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(io.waitScope);
    KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
    return resp.body->readAllText().wait(io.waitScope);
  }

  TokioTestIo io;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<TestHttpService> service;
  kj::Own<kj::TlsContext> tls;
  kj::Maybe<CountingReceiver&> tcpListener;
  uint port;
  kj::Own<kj::ConnectionReceiver> listener;
  kj::Own<kj::HttpServer> server;
  kj::Promise<void> listenTask = nullptr;
  kj::Own<kj::HttpService> clientService;
  kj::Own<kj::HttpClient> client;
};

// Runs a GET and returns the exception it failed with; asserts it failed.
kj::Exception expectGetFails(TlsFixture& f) {
  auto result = kj::runCatchingExceptions([&]() {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    resp.body->readAllText().wait(f.io.waitScope);
  });
  return KJ_ASSERT_NONNULL(kj::mv(result), "expected the HTTPS request to fail");
}

// =======================================================================================

KJ_TEST("hyper https client: round trip via a trusted private CA, with keep-alive reuse") {
  TlsFixture f;

  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");

  // Both requests rode one TCP+TLS connection (keep-alive reuse through the TLS layer).
  KJ_EXPECT(KJ_ASSERT_NONNULL(f.tcpListener).count == 1);
}

KJ_TEST("hyper https client: IP-address server name verification") {
  // The server certificate's SAN includes IP:127.0.0.1; expectedServerHostname may be an IP
  // (kj handles this with X509_VERIFY_PARAM_set1_ip_asc).
  TlsFixture f({}, {.expectedHostname = "127.0.0.1"});
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
}

KJ_TEST("hyper https client: wss WebSocket echo over TLS") {
  TlsFixture f;

  auto ws = f.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(ws.statusCode == 101);
  auto& socket = *KJ_ASSERT_NONNULL(ws.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>());

  socket.send("tls echo"_kj).wait(f.io.waitScope);
  auto message = socket.receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "tls echo");

  socket.close(1000, "bye").wait(f.io.waitScope);
  auto close = socket.receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(close.tryGet<kj::WebSocket::Close>()).code == 1000);
}

KJ_TEST("hyper https client: CONNECT tunnel rides inside TLS") {
  TlsFixture f;

  auto request = f.client->connect("echo-host:1234", f.makeHeaders(), {});
  auto status = request.status.wait(f.io.waitScope);
  KJ_EXPECT(status.statusCode == 200, status.statusCode);

  request.connection->write("tunnel through tls"_kjb).wait(f.io.waitScope);
  kj::byte buf[64];
  auto n = request.connection->tryRead(buf, 18, sizeof(buf)).wait(f.io.waitScope);
  KJ_EXPECT(kj::str(kj::arrayPtr(buf, n).asChars()) == "tunnel through tls");
}

// The kj (OpenSSL) server may observe the client's certificate-rejection alert before the
// socket closes; its TLS receiver then logs "error accepting tls connection". Whether the log
// happens is timing-dependent, so it is tolerated (rather than KJ_EXPECT_LOG-required) in the
// rejection tests.
class TolerateTlsAcceptErrors: public kj::ExceptionCallback {
 public:
  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    if (severity == kj::LogSeverity::ERROR &&
        kj::StringPtr(text).contains("error accepting tls connection")) {
      return;
    }
    kj::ExceptionCallback::logMessage(severity, file, line, contextDepth, kj::mv(text));
  }
};

KJ_TEST("hyper https client: untrusted server certificate is rejected") {
  // No trust anchors at all: verification must fail with kj's error surface.
  TlsFixture f({}, {.trustCa = false});

  TolerateTlsAcceptErrors tolerateLog;
  auto e = expectGetFails(f);
  KJ_EXPECT(
      e.getDescription().contains("TLS peer's certificate is not trusted"), e.getDescription());
  KJ_EXPECT(
      e.getDescription().contains("unable to get local issuer certificate"), e.getDescription());
}

KJ_TEST("hyper https client: hostname mismatch is rejected") {
  // CA is trusted but the certificate (example.com/localhost/127.0.0.1) is not valid for the
  // expected hostname.
  TlsFixture f({}, {.expectedHostname = "other.example.com"});

  TolerateTlsAcceptErrors tolerateLog;
  auto e = expectGetFails(f);
  KJ_EXPECT(
      e.getDescription().contains("TLS peer's certificate is not trusted"), e.getDescription());
  KJ_EXPECT(e.getDescription().contains("hostname mismatch"), e.getDescription());
}

KJ_TEST("hyper https client: self-signed certificate in trustedCertificates is directly trusted") {
  // The OpenSSL trust-store behavior workerd dev configs rely on: a self-signed end-entity
  // certificate listed in trustedCertificates is accepted for its names.
  TlsFixture f({.cert = SELF_SIGNED_CERT, .key = SELF_SIGNED_KEY},
      {.trustCa = false, .extraTrusted = SELF_SIGNED_CERT});
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
}

KJ_TEST("hyper https client: client certificate presented when the server requires one") {
  TlsFixture f({.verifyClients = true}, {.presentClientCert = true});
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
}

KJ_TEST("hyper https client: server requiring a client certificate rejects a client without one") {
  TlsFixture f({.verifyClients = true}, {});
  // The kj server logs the OpenSSL-side rejection of the certificate-less handshake
  // (timing-dependent, hence tolerated rather than required).
  TolerateTlsAcceptErrors tolerateLog;
  // With TLS 1.3 the server's rejection can arrive during the handshake or on the first
  // request/response I/O, so only the failure itself is asserted.
  expectGetFails(f);
}

KJ_TEST("hyper https client: minVersion below TLS 1.2 is rejected with a clear config error") {
  ensureTokioInitialized();
  for (auto version: {TlsMinVersion::SSL3, TlsMinVersion::TLS1_0, TlsMinVersion::TLS1_1}) {
    auto options = makeTlsClientOptions({.minVersion = version});
    auto result = kj::runCatchingExceptions(
        [&]() { workerd::rust::kj_hyper::newHyperTlsClientConfig(kj::mv(options)); });
    auto& e = KJ_ASSERT_NONNULL(result, "expected config construction to fail", (uint)version);
    KJ_EXPECT(e.getDescription().contains("not supported by the hyper (rustls) HTTP client"),
        e.getDescription());
    KJ_EXPECT(e.getDescription().contains("only TLS 1.2 and TLS 1.3"), e.getDescription());
  }
}

KJ_TEST("hyper https client: minVersion TLS 1.3 round trip") {
  TlsFixture f({}, {.minVersion = TlsMinVersion::TLS1_3});
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
}

KJ_TEST("hyper https client: cipherList with a supported OpenSSL cipher name works") {
  // ECDHE-RSA suites require an RSA server certificate. (TLS 1.3, when negotiated, is not
  // governed by the cipher list -- same as OpenSSL.)
  TlsFixture f({.cert = RSA_SERVER_CERT, .key = RSA_SERVER_KEY},
      {.cipherList = "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384"});
  KJ_EXPECT(f.get("/hello") == "Hello TLS!");
}

KJ_TEST("hyper https client: cipherList with no supported cipher is rejected at config time") {
  ensureTokioInitialized();
  // A classic OpenSSL keyword list: meaningful to OpenSSL, but naming no concrete suite rustls
  // supports; the documented policy is reject-with-clear-error rather than silently ignoring.
  auto options = makeTlsClientOptions({.cipherList = "HIGH:!aNULL:!MD5"});
  auto result = kj::runCatchingExceptions(
      [&]() { workerd::rust::kj_hyper::newHyperTlsClientConfig(kj::mv(options)); });
  auto& e = KJ_ASSERT_NONNULL(result, "expected config construction to fail");
  KJ_EXPECT(e.getDescription().contains("cipherList"), e.getDescription());
  KJ_EXPECT(e.getDescription().contains("Supported ciphers:"), e.getDescription());
}

KJ_TEST("hyper https client: invalid trusted certificate PEM is rejected at config time") {
  ensureTokioInitialized();
  TlsClientOptions options;
  options.trust_system_roots = false;
  options.trusted_certificates.push_back(::rust::String("not a pem"));
  options.min_version = TlsMinVersion::GOOD_DEFAULT;
  auto result = kj::runCatchingExceptions(
      [&]() { workerd::rust::kj_hyper::newHyperTlsClientConfig(kj::mv(options)); });
  auto& e = KJ_ASSERT_NONNULL(result, "expected config construction to fail");
  KJ_EXPECT(e.getDescription().contains("trustedCertificates"), e.getDescription());
}

// =======================================================================================
// Inbound TLS: the hyper (rustls) server on an externally-accepted connection, exercised by a
// kj (OpenSSL) TLS client — the shape of workerd's https sockets under the rust I/O backend.

#if !_WIN32

using workerd::rust::kj_hyper::TlsServerOptions;

TlsServerOptions makeTlsServerOptions(bool requireClientCerts) {
  TlsServerOptions options;
  options.certificate_chain = ::rust::String(SERVER_CERT.begin(), SERVER_CERT.size());
  options.private_key = ::rust::String(SERVER_KEY.begin(), SERVER_KEY.size());
  options.require_client_certs = requireClientCerts;
  options.trust_system_roots = false;
  if (requireClientCerts) {
    options.trusted_certificates.push_back(::rust::String(CA_CERT.begin(), CA_CERT.size()));
  }
  options.min_version = TlsMinVersion::GOOD_DEFAULT;
  return options;
}

class NullEntropy: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    for (auto& b: buffer) b = 4;  // chosen by fair dice roll
  }
};

// How the accepted connection reaches hyper.
enum class InboundPath {
  // newHyperHttpConnection(stream, tlsConfig): hyper's own TLS accept over the taken socket.
  HyperTls,
  // The SecureNetworkWrapper shape (workerd tls-network.c++): wrap_tls_server() first, then
  // newHyperHttpConnection(stream) takes the Rust TLS stream back out of its kj wrapper.
  WrappedTcp,
  // The same over an in-memory pipe: the plaintext stream is foreign, so the Rust TLS stream
  // drives that stream directly.
  WrappedPipe,
};

struct InboundOptions {
  bool requireClientCerts = false;
  bool presentClientCert = false;
  InboundPath path = InboundPath::HyperTls;
  // Client side: kj's OpenSSL TlsContext (default), or wrap_tls_client() -- the Rust TLS
  // stream driven by a kj HttpClient through the RustAsyncIoStream bridge, the shape of the
  // sockets API's startTls() under the rust backend.
  bool rustClient = false;
  bool clientTrustsCa = true;
};

struct InboundTlsFixture {
  explicit InboundTlsFixture(bool requireClientCerts = false, bool presentClientCert = false)
      : InboundTlsFixture(InboundOptions{
          .requireClientCerts = requireClientCerts, .presentClientCert = presentClientCert}) {}

  explicit InboundTlsFixture(InboundOptions opts) {
    ensureTokioInitialized();

    table = kj::HttpHeaderTable::Builder().build();
    service = kj::heap<TestHttpService>(*table);

    auto tlsConfig = workerd::rust::kj_hyper::newHyperTlsServerConfig(
        makeTlsServerOptions(opts.requireClientCerts));

    kj::Own<kj::AsyncIoStream> serverStream;
    kj::Own<kj::AsyncIoStream> clientStream;
    if (opts.path == InboundPath::WrappedPipe) {
      auto pipe = io.provider->newTwoWayPipe();
      serverStream = kj::mv(pipe.ends[0]);
      clientStream = kj::mv(pipe.ends[1]);
    } else {
      // Loopback pair, mirroring workerd's HttpListener: the accept happens in "C++", the
      // accepted stream's socket is taken natively by hyper together with the rustls config.
      auto listener =
          io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(io.waitScope)->listen();
      auto connectPromise = io.provider->getNetwork()
                                .parseAddress("127.0.0.1", listener->getPort())
                                .wait(io.waitScope)
                                ->connect();
      serverStream = listener->accept().wait(io.waitScope);
      clientStream = connectPromise.wait(io.waitScope);
    }

    if (opts.path == InboundPath::HyperTls) {
      connection = workerd::rust::kj_hyper::newHyperHttpConnection(
          *table, *service, kj::mv(serverStream), *tlsConfig);
    } else {
      connection = workerd::rust::kj_hyper::newHyperHttpConnection(*table, *service,
          workerd::rust::kj_hyper::wrap_tls_server(kj::mv(serverStream), *tlsConfig));
    }

    // serve() legitimately rejects in the failed-handshake tests (the client side observes
    // and asserts the failure); swallow it so no stray ERROR reaches the test log.
    serveTask =
        connection->serve().catch_([](kj::Exception&& e) {}).eagerlyEvaluate(nullptr).fork();

    if (opts.rustClient) {
      // The Rust TLS stream, handed to kj: its handshake runs on the first request.
      auto clientConfig = workerd::rust::kj_hyper::newHyperTlsClientConfig(makeTlsClientOptions(
          {.trustCa = opts.clientTrustsCa, .presentClientCert = opts.presentClientCert}));
      tlsStream = workerd::rust::kj_hyper::wrap_tls_client(
          kj::mv(clientStream), *clientConfig, "example.com");
    } else {
      // The kj (OpenSSL) client side of the TLS session.
      kj::TlsContext::Options clientOptions;
      clientOptions.useSystemTrustStore = false;
      kj::TlsCertificate caCert(CA_CERT);
      clientOptions.trustedCertificates = kj::arrayPtr(&caCert, 1);
      kj::TlsKeypair clientKeypair{.privateKey = kj::TlsPrivateKey(CLIENT_KEY),
        .certificate = kj::TlsCertificate(CLIENT_CERT)};
      if (opts.presentClientCert) {
        clientOptions.defaultKeypair = clientKeypair;
      }
      clientTls = kj::heap<kj::TlsContext>(kj::mv(clientOptions));

      // Throws if the handshake fails (e.g. the server requires a certificate we didn't send);
      // callers expecting failure catch it.
      tlsStream = clientTls->wrapClient(kj::mv(clientStream), "example.com").wait(io.waitScope);
    }
    client = kj::newHttpClient(*table, *tlsStream,
        {.entropySource = entropy,
          .webSocketCompressionMode = kj::HttpClientSettings::MANUAL_COMPRESSION});
  }

  ~InboundTlsFixture() noexcept(false) {
    // Quiesce before tearing down the KJ event loop: closing the client end makes hyper's
    // connection task exit, resolving serve(). serve() legitimately rejects in the
    // failed-handshake tests, so its outcome is swallowed here (asserted by the tests
    // themselves where relevant).
    client = nullptr;
    tlsStream = nullptr;
    auto ignored = kj::runCatchingExceptions([&]() {
      auto task = serveTask.addBranch();
      if (!task.poll(io.waitScope)) {
        connection->shutdown();
        task.wait(io.waitScope);
      }
    });
  }

  kj::HttpHeaders makeHeaders() {
    kj::HttpHeaders headers(*table);
    headers.set(kj::HttpHeaderId::HOST, kj::str("example.com"));
    return headers;
  }

  TokioTestIo io;
  NullEntropy entropy;
  kj::Own<kj::HttpHeaderTable> table;
  kj::Own<TestHttpService> service;
  kj::Own<workerd::rust::kj_hyper::HyperHttpConnection> connection;
  kj::ForkedPromise<void> serveTask = nullptr;
  kj::Own<kj::TlsContext> clientTls;
  kj::Own<kj::AsyncIoStream> tlsStream;
  kj::Own<kj::HttpClient> client;
};

KJ_TEST("hyper https server: kj TLS client round trip with keep-alive") {
  InboundTlsFixture f;

  for (int i = 0; i < 2; i++) {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello TLS!", body);
  }
}

// The rust backend's SecureNetworkWrapper (workerd tls-network.c++) shapes: wrap_tls_server()
// / wrap_tls_client() hand kj a Rust TLS stream; hyper takes it back out natively.

KJ_TEST("wrap_tls_server: the wrapper's TLS stream over a TCP socket is served natively by hyper") {
  InboundTlsFixture f({.path = InboundPath::WrappedTcp});

  for (int i = 0; i < 2; i++) {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello TLS!", body);
  }
}

KJ_TEST("wrap_tls_server over an in-memory pipe: the Rust TLS stream drives the kj stream") {
  InboundTlsFixture f({.path = InboundPath::WrappedPipe});

  for (int i = 0; i < 2; i++) {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello TLS!", body);
  }
}

KJ_TEST("wrap_tls_client: a kj HttpClient drives the Rust TLS stream through the bridge") {
  InboundTlsFixture f({.path = InboundPath::WrappedTcp, .rustClient = true});

  for (int i = 0; i < 2; i++) {
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    auto resp = req.response.wait(f.io.waitScope);
    KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
    auto body = resp.body->readAllText().wait(f.io.waitScope);
    KJ_EXPECT(body == "Hello TLS!", body);
  }
}

KJ_TEST("wrap_tls_client: an untrusted server certificate fails with kj's wording") {
  InboundTlsFixture f(
      {.path = InboundPath::WrappedTcp, .rustClient = true, .clientTrustsCa = false});

  auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
  KJ_EXPECT_THROW_MESSAGE(
      "TLS peer's certificate is not trusted", req.response.wait(f.io.waitScope));
}

// Rust TLS streams handed to kj consumers (RustAsyncIoStream): kj's stream contract beyond
// plain reads and writes.

// Two Rust TLS streams talking over an in-memory pipe: both sides drive foreign kj streams.
struct RustTlsPipe {
  RustTlsPipe()
      : serverConfig((ensureTokioInitialized(),
            workerd::rust::kj_hyper::newHyperTlsServerConfig(makeTlsServerOptions(false)))),
        clientConfig(workerd::rust::kj_hyper::newHyperTlsClientConfig(makeTlsClientOptions({}))) {
    auto pipe = io.provider->newTwoWayPipe();
    rawPeer = kj::mv(pipe.ends[1]);
    server = workerd::rust::kj_hyper::wrap_tls_server(kj::mv(pipe.ends[0]), *serverConfig);
  }

  // Wraps the pipe's other end in client-side TLS (tests of the server's transport alone keep
  // the raw end instead).
  void connectClient() {
    client =
        workerd::rust::kj_hyper::wrap_tls_client(kj::mv(rawPeer), *clientConfig, "example.com");
  }

  TokioTestIo io;
  ::rust::Box<workerd::rust::kj_hyper::HyperTlsServerConfig> serverConfig;
  ::rust::Box<workerd::rust::kj_hyper::HyperTlsClientConfig> clientConfig;
  kj::Own<kj::AsyncIoStream> rawPeer;
  kj::Own<kj::AsyncIoStream> server;
  kj::Own<kj::AsyncIoStream> client;
};

KJ_TEST("Rust TLS stream: a read and a write in flight at once both complete") {
  RustTlsPipe f;
  f.connectClient();
  auto& ws = f.io.waitScope;

  // The client parks a read, then writes while it is parked; the server answers. Both client
  // operations progress on the one kj transport. (The server's read is started up front:
  // the handshake needs both ends in motion, as with any TLS stream.)
  kj::byte serverBuffer[16];
  auto serverRead =
      f.server->tryRead(serverBuffer, 4, sizeof(serverBuffer)).eagerlyEvaluate(nullptr);
  kj::byte clientBuffer[16];  // deliberately uninitialized, as kj permits
  auto clientRead =
      f.client->tryRead(clientBuffer, 4, sizeof(clientBuffer)).eagerlyEvaluate(nullptr);
  KJ_EXPECT(!clientRead.poll(ws));
  f.client->write("ping"_kjb).wait(ws);

  KJ_EXPECT(serverRead.wait(ws) == 4);
  KJ_EXPECT(kj::arrayPtr(serverBuffer, 4) == "ping"_kjb);
  f.server->write("pong"_kjb).wait(ws);

  KJ_EXPECT(clientRead.wait(ws) == 4);
  KJ_EXPECT(kj::arrayPtr(clientBuffer, 4) == "pong"_kjb);
}

KJ_TEST("Rust TLS stream: abortRead() fails a parked read") {
  RustTlsPipe f;
  f.connectClient();
  auto& ws = f.io.waitScope;

  kj::byte buffer[16];
  auto read = f.client->tryRead(buffer, 1, sizeof(buffer));
  KJ_EXPECT(!read.poll(ws));
  f.client->abortRead();
  KJ_EXPECT_THROW_MESSAGE("abortRead() has been called", read.wait(ws));
  KJ_EXPECT_THROW_MESSAGE("abortRead() has been called", f.client->tryRead(buffer, 1, 16).wait(ws));
}

KJ_TEST("Rust TLS stream: whenWriteDisconnected() forwards to a kj transport") {
  RustTlsPipe f;
  auto& ws = f.io.waitScope;

  // The server's transport is a pipe end; destroying the other end disconnects it.
  auto disconnected = f.server->whenWriteDisconnected();
  KJ_EXPECT(!disconnected.poll(ws));
  f.rawPeer = nullptr;
  disconnected.wait(ws);
}

KJ_TEST(
    "Rust TLS stream: whenWriteDisconnected() forwards to a socket, and blocks the native take") {
  ensureTokioInitialized();
  TokioTestIo io;
  auto& ws = io.waitScope;
  auto serverConfig = workerd::rust::kj_hyper::newHyperTlsServerConfig(makeTlsServerOptions(false));

  auto listener = io.provider->getNetwork().parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto connecting =
      io.provider->getNetwork().parseAddress("127.0.0.1", listener->getPort()).wait(ws)->connect();
  auto accepted = listener->accept().wait(ws);
  auto peer = connecting.wait(ws);

  auto tls = workerd::rust::kj_hyper::wrap_tls_server(kj::mv(accepted), *serverConfig);
  KJ_EXPECT(workerd::rust::kj_hyper::isReleasableRustStream(*tls));

  auto disconnected = tls->whenWriteDisconnected();
  KJ_EXPECT(!disconnected.poll(ws));
  // An operation holds the stream: hyper must drive it through kj rather than take it apart.
  KJ_EXPECT(!workerd::rust::kj_hyper::isReleasableRustStream(*tls));

  // The peer resets the connection (SO_LINGER=0): new writes are doomed.
  struct linger lin;
  lin.l_onoff = 1;
  lin.l_linger = 0;
  KJ_ASSERT(
      setsockopt(KJ_ASSERT_NONNULL(peer->getFd()), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin)) == 0);
  peer = nullptr;
  disconnected.wait(ws);
  KJ_EXPECT(workerd::rust::kj_hyper::isReleasableRustStream(*tls));
}

KJ_TEST("hyper https server: WebSocket echo over TLS") {
  InboundTlsFixture f;

  auto ws = f.client->openWebSocket("/ws-echo", f.makeHeaders()).wait(f.io.waitScope);
  KJ_EXPECT(ws.statusCode == 101, ws.statusCode);
  auto& socket = *KJ_ASSERT_NONNULL(ws.webSocketOrBody.tryGet<kj::Own<kj::WebSocket>>());

  socket.send("inbound tls echo"_kj).wait(f.io.waitScope);
  auto message = socket.receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(message.tryGet<kj::String>()) == "inbound tls echo");

  socket.close(1000, "bye").wait(f.io.waitScope);
  auto close = socket.receive().wait(f.io.waitScope);
  KJ_EXPECT(KJ_ASSERT_NONNULL(close.tryGet<kj::WebSocket::Close>()).code == 1000);
}

KJ_TEST("hyper https server: requireClientCerts accepts a certificate from the trusted CA") {
  InboundTlsFixture f(/*requireClientCerts=*/true, /*presentClientCert=*/true);

  auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
  auto resp = req.response.wait(f.io.waitScope);
  KJ_EXPECT(resp.statusCode == 200, resp.statusCode);
  KJ_EXPECT(resp.body->readAllText().wait(f.io.waitScope) == "Hello TLS!");
}

KJ_TEST("hyper https server: requireClientCerts rejects a client without a certificate") {
  auto result = kj::runCatchingExceptions([&]() {
    InboundTlsFixture f(/*requireClientCerts=*/true, /*presentClientCert=*/false);
    // If the client-side handshake somehow "succeeded", the first request must still fail.
    auto req = f.client->request(kj::HttpMethod::GET, "/hello", f.makeHeaders(), uint64_t(0));
    req.response.wait(f.io.waitScope);
  });
  KJ_ASSERT_NONNULL(result, "expected the handshake (or first request) to fail");
}

KJ_TEST("hyper https server: missing keypair is rejected at config time") {
  ensureTokioInitialized();
  TlsServerOptions options;
  options.require_client_certs = false;
  options.trust_system_roots = false;
  options.min_version = TlsMinVersion::GOOD_DEFAULT;
  auto result = kj::runCatchingExceptions(
      [&]() { workerd::rust::kj_hyper::newHyperTlsServerConfig(kj::mv(options)); });
  auto& e = KJ_ASSERT_NONNULL(result, "expected config construction to fail");
  KJ_EXPECT(e.getDescription().contains("certificateChain"), e.getDescription());
}

#endif  // !_WIN32

}  // namespace
