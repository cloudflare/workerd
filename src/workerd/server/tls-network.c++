// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "tls-network.h"

#include <kj/debug.h>

#if WORKERD_RUST_IO_BACKEND_RUST

#include <kj-hyper/hyper-http.h>

namespace workerd::server {
namespace {

// rust I/O backend: raw-socket TLS over any kj::AsyncIoStream, in both directions.
//
// Under the default (OpenSSL) build, kj::TlsContext wraps plaintext kj streams in TLS --
// wrapClient() upgrades an already-connected socket (kj's NetworkHttpClient::connect() uses
// that to populate the socket's kj::TlsStarterCallback), wrapServer()/wrapPort() serve TLS
// listeners. There is no kj::TlsContext under this backend, so we provide a rustls-backed
// kj::SecureNetworkWrapper instead; kj's connect() machinery (the TransitionaryAsyncIoStream /
// tlsStarter wiring) and server.c++'s upstream listener shape are reused verbatim. The rustls
// record processing is a synchronous, pointer-free state machine in
// src/rust/cxx/kj-hyper/client_tls.rs (RustlsConn, client- or server-side); this stream owns
// the plaintext kj::AsyncIoStream and drives that machine over it, so it works over native
// kj-rs-io sockets and foreign (e.g. kj::newPromisedStream, in-memory pipe) plaintext streams
// alike. Deliberately no getFd()/getWin32Handle() passthrough: this stream transforms bytes,
// so exposing the raw transport handle would let consumers (e.g. the hyper server's native
// socket takeover) bypass the TLS layer.
class RustlsStream final: public kj::AsyncIoStream {
 public:
  RustlsStream(kj::Own<kj::AsyncIoStream> inner, ::rust::Box<rust::kj_hyper::RustlsConn> conn)
      : inner(kj::mv(inner)),
        conn(kj::mv(conn)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    co_await ensureHandshake();
    auto* out = reinterpret_cast<uint8_t*>(buffer);
    size_t total = 0;
    while (total < minBytes) {
      int64_t n = conn->read_plaintext(::rust::Slice<uint8_t>(out + total, maxBytes - total));
      if (n > 0) {
        total += static_cast<size_t>(n);
        continue;
      }
      if (n == 0) break;  // clean TLS EOF (peer close_notify / truncation)
      // n < 0: no plaintext buffered; pull more ciphertext off the wire and feed rustls.
      size_t r = co_await inner->tryRead(readBuf.begin(), 1, readBuf.size());
      if (r == 0) break;  // underlying transport EOF
      conn->feed_tls_in(
          ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t*>(readBuf.begin()), r));
      co_await flushOut();  // records rustls produced in response (acks, key updates, alerts)
    }
    co_return total;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    co_await ensureHandshake();
    conn->write_plaintext(::rust::Slice<const uint8_t>(
        reinterpret_cast<const uint8_t*>(buffer.begin()), buffer.size()));
    co_await flushOut();
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    co_await ensureHandshake();
    for (auto& piece: pieces) {
      conn->write_plaintext(::rust::Slice<const uint8_t>(
          reinterpret_cast<const uint8_t*>(piece.begin()), piece.size()));
    }
    co_await flushOut();
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return inner->whenWriteDisconnected();
  }

  void shutdownWrite() override {
    // Best-effort close_notify (kj's TLS shutdownWrite is likewise best-effort), then shut down
    // the underlying write half. Detached and owned so it survives past this void call.
    conn->send_close_notify();
    shutdownTask = flushOut().then([this]() {
      inner->shutdownWrite();
    }).eagerlyEvaluate([](kj::Exception&&) {});
  }

  void abortRead() override {
    inner->abortRead();
  }

  void getsockopt(int level, int option, void* value, uint* length) override {
    inner->getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, uint length) override {
    inner->setsockopt(level, option, value, length);
  }
  void getsockname(struct sockaddr* addr, uint* length) override {
    inner->getsockname(addr, length);
  }
  void getpeername(struct sockaddr* addr, uint* length) override {
    inner->getpeername(addr, length);
  }
  kj::Maybe<int> getFd() const override {
    // Deliberately none; see the class comment.
    return kj::none;
  }

 private:
  // Drives the TLS handshake to completion exactly once; every read/write awaits it first. Lazily
  // started on first I/O: for a client connection the first flushOut sends the ClientHello rustls
  // queued at construction; for a server connection the peer speaks first, so the handshake is
  // driven by the read loop. (Laziness is also what keeps a TLS listener's accept loop live:
  // accepted streams are returned immediately and each handshake runs inside its own
  // connection's task, the same accept-liveness property kj's TlsConnectionReceiver gets from
  // parallelizing handshakes.)
  kj::Promise<void> ensureHandshake() {
    KJ_IF_SOME(p, handshake) {
      return p.addBranch();
    }
    return handshake.emplace(doHandshake().fork()).addBranch();
  }

  kj::Promise<void> doHandshake() {
    co_await flushOut();  // ClientHello for a client conn (+ any subsequent flights); server no-op
    while (conn->is_handshaking()) {
      size_t r = co_await inner->tryRead(readBuf.begin(), 1, readBuf.size());
      if (r == 0) {
        kj::throwFatalException(
            KJ_EXCEPTION(DISCONNECTED, "TLS handshake: peer closed the connection"));
      }
      conn->feed_tls_in(
          ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t*>(readBuf.begin()), r));
      co_await flushOut();
    }
  }

  // Serializes production of outgoing ciphertext with its wire write, so TLS records reach the
  // wire in the sequence-number order rustls assigned them (concurrent read+write would otherwise
  // interleave two inner->write()s -- a kj contract violation -- and reorder records). Held across
  // the awaits; released on completion or on cancellation via the KJ_DEFER, which is armed BEFORE
  // the first suspension so that a flush cancelled while still queued at `co_await prev` cannot
  // strand the lock (destroying the fulfiller unfulfilled would poison `writeLock` -- every later
  // I/O on the stream would fail with "PromiseFulfiller was destroyed"). The defer releases by
  // *chaining* a branch of `prev` rather than fulfilling directly: if we are cancelled while
  // still waiting our turn, our successor keeps waiting for our predecessors to finish instead of
  // being woken early into their in-flight write.
  kj::Promise<void> flushOut() {
    auto release = kj::newPromiseAndFulfiller<kj::Promise<void>>();
    auto prev = kj::mv(writeLock).fork();
    writeLock = kj::mv(release.promise);
    KJ_DEFER(release.fulfiller->fulfill(prev.addBranch()));
    co_await prev.addBranch();
    while (conn->wants_tls_write()) {
      auto bytes = conn->take_tls_out();
      if (bytes.size() == 0) break;
      co_await inner->write(
          kj::arrayPtr(reinterpret_cast<const kj::byte*>(bytes.data()), bytes.size()));
    }
  }

  kj::Own<kj::AsyncIoStream> inner;
  ::rust::Box<rust::kj_hyper::RustlsConn> conn;
  // Max TLS record payload (16 KiB) plus header/MAC slack.
  kj::Array<kj::byte> readBuf = kj::heapArray<kj::byte>(16384 + 2048);
  kj::Maybe<kj::ForkedPromise<void>> handshake;
  kj::Promise<void> writeLock = kj::READY_NOW;
  kj::Promise<void> shutdownTask = kj::READY_NOW;
};

// A kj::NetworkAddress that TLS-wraps every connection via a kj::SecureNetworkWrapper, verifying
// the server against `hostname`. Mirrors kj's own TlsNetworkAddress (kj/compat/tls.c++) so
// TLS-from-the-start connects (JS connect() with secureTransport = 'on') behave as under cxx.
class RustlsTlsNetworkAddress final: public kj::NetworkAddress {
 public:
  RustlsTlsNetworkAddress(
      kj::SecureNetworkWrapper& tls, kj::String hostname, kj::Own<kj::NetworkAddress> inner)
      : tls(tls),
        hostname(kj::mv(hostname)),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
    // Copy hostname: callers commonly drop the NetworkAddress as soon as connect() returns.
    auto& tlsRef = tls;
    auto hostnameCopy = kj::str(hostname);
    return inner->connect().then(
        [&tlsRef, hostname = kj::mv(hostnameCopy)](kj::Own<kj::AsyncIoStream>&& stream) mutable {
      return tlsRef.wrapClient(kj::mv(stream), hostname);
    });
  }

  kj::Own<kj::ConnectionReceiver> listen() override {
    return tls.wrapPort(inner->listen());
  }
  kj::Own<kj::NetworkAddress> clone() override {
    return kj::heap<RustlsTlsNetworkAddress>(tls, kj::str(hostname), inner->clone());
  }
  kj::String toString() override {
    return kj::str("tls:", inner->toString());
  }

 private:
  kj::SecureNetworkWrapper& tls;
  kj::String hostname;
  kj::Own<kj::NetworkAddress> inner;
};

// A kj::Network wrapping every address in TLS via a kj::SecureNetworkWrapper. Mirrors kj's own
// TlsNetwork, including the address-string hostname extraction (kj/compat/tls.c++).
class RustlsTlsNetwork final: public kj::Network {
 public:
  RustlsTlsNetwork(kj::SecureNetworkWrapper& tls, kj::Own<kj::Network> inner)
      : tls(tls),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
      kj::StringPtr addr, uint portHint) override {
    // Extract the hostname/IP to authenticate. This mirrors kj's TlsNetwork::parseAddress: we
    // cannot just split on ':' because the address might be IPv6.
    kj::String hostname;
    if (addr.startsWith("[")) {
      KJ_IF_SOME(pos, addr.findFirst(']')) {
        hostname = kj::str(addr.slice(1, pos));
      } else {
        hostname = kj::heapString(addr);
      }
    } else if (addr.startsWith("unix:") || addr.startsWith("unix-abstract:")) {
      KJ_FAIL_REQUIRE("can't authenticate Unix domain socket with TLS", addr);
    } else {
      uint colons = 0;
      for (auto c: addr) {
        if (c == ':') ++colons;
      }
      if (colons >= 2) {
        // IPv6 without a port (a port would have required brackets); keep the whole thing.
        hostname = kj::heapString(addr);
      } else KJ_IF_SOME(pos, addr.findFirst(':')) {
        hostname = kj::heapString(addr.first(pos));
      } else {
        hostname = kj::heapString(addr);
      }
    }

    auto& tlsRef = tls;
    return inner->parseAddress(addr, portHint)
        .then([&tlsRef, hostname = kj::mv(hostname)](
                  kj::Own<kj::NetworkAddress>&& addr) mutable -> kj::Own<kj::NetworkAddress> {
      return kj::heap<RustlsTlsNetworkAddress>(tlsRef, kj::mv(hostname), kj::mv(addr));
    });
  }

  kj::Own<kj::NetworkAddress> getSockaddr(const void* sockaddr, uint len) override {
    KJ_UNIMPLEMENTED("TLS does not implement getSockaddr() because it needs to know hostnames");
  }
  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override {
    return kj::heap<RustlsTlsNetwork>(tls, inner->restrictPeers(allow, deny));
  }

 private:
  kj::SecureNetworkWrapper& tls;
  kj::Own<kj::Network> inner;
};

// A kj::ConnectionReceiver whose accepted connections are wrapped in server-side TLS -- the
// rust backend's analog of kj's TlsConnectionReceiver (what kj::TlsContext::wrapPort()
// returns). Accepted streams are returned immediately with a *lazy* handshake (driven by the
// connection's first reads inside its own serving task), which preserves kj's accept-loop
// liveness property without the parallel-handshake queue machinery; a failed handshake
// surfaces from that connection's task (logged and dropped by the server's per-connection
// error handling, like kj's "error accepting tls connection" logging), never from accept().
// The peer identity is passed through untouched (the transport identity; see
// unwrapTlsPeerIdentity in tls-network.h).
class RustlsConnectionReceiver final: public kj::ConnectionReceiver {
 public:
  RustlsConnectionReceiver(kj::SecureNetworkWrapper& tls, kj::Own<kj::ConnectionReceiver> inner)
      : tls(tls),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    auto stream = co_await inner->accept();
    co_return co_await tls.wrapServer(kj::mv(stream));
  }

  kj::Promise<kj::AuthenticatedStream> acceptAuthenticated() override {
    auto accepted = co_await inner->acceptAuthenticated();
    accepted.stream = co_await tls.wrapServer(kj::mv(accepted.stream));
    co_return kj::mv(accepted);
  }

  uint getPort() override {
    return inner->getPort();
  }
  void getsockopt(int level, int option, void* value, uint* length) override {
    inner->getsockopt(level, option, value, length);
  }
  void setsockopt(int level, int option, const void* value, uint length) override {
    inner->setsockopt(level, option, value, length);
  }
  void getsockname(struct sockaddr* addr, uint* length) override {
    inner->getsockname(addr, length);
  }

 private:
  kj::SecureNetworkWrapper& tls;
  kj::Own<kj::ConnectionReceiver> inner;
};

// rustls-backed kj::SecureNetworkWrapper: wrapClient() (the socket startTls path, see
// RustlsStream), wrapAddress()/wrapNetwork() (TLS-from-the-start connects), and -- when the
// configuration includes a keypair -- wrapServer()/wrapPort() (TLS listeners). The
// AuthenticatedStream client direction is unused under this backend.
class RustlsSecureNetworkWrapper final: public kj::SecureNetworkWrapper {
 public:
  // Client-only wrapper (e.g. system trust): the server directions fail with `serverState`'s
  // explanation.
  explicit RustlsSecureNetworkWrapper(::rust::Box<rust::kj_hyper::HyperTlsClientConfig> config)
      : clientConfig(kj::mv(config)),
        serverState(KJ_EXCEPTION(FAILED,
            "this TLS context has no keypair configured, so it cannot serve TLS listeners")) {}

  // Full wrapper from config::TlsOptions-derived options: the server config is built lazily on
  // first wrapPort()/wrapServer() (see makeTlsContext in tls-network.h).
  RustlsSecureNetworkWrapper(::rust::Box<rust::kj_hyper::HyperTlsClientConfig> config,
      rust::kj_hyper::TlsServerOptions serverOptions,
      bool hasKeypair)
      : clientConfig(kj::mv(config)),
        serverState(KJ_EXCEPTION(FAILED,
            "an https or TLS socket's TlsOptions must include a keypair (rustls, unlike "
            "OpenSSL, cannot even begin serving TLS without one)")) {
    if (hasKeypair) {
      serverState.init<rust::kj_hyper::TlsServerOptions>(kj::mv(serverOptions));
    }
  }

  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapClient(
      kj::Own<kj::AsyncIoStream> stream, kj::StringPtr expectedServerHostname) override {
    // new_rustls_client_conn throws (rejecting this coroutine) on an invalid server name.
    auto conn = rust::kj_hyper::new_rustls_client_conn(
        *clientConfig, expectedServerHostname.as<kj_rs::RustUncheckedUtf8>());
    co_return kj::heap<RustlsStream>(kj::mv(stream), kj::mv(conn));
  }

  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapServer(kj::Own<kj::AsyncIoStream> stream) override {
    auto conn = rust::kj_hyper::new_rustls_server_conn(getServerConfig());
    co_return kj::heap<RustlsStream>(kj::mv(stream), kj::mv(conn));
  }

  kj::Promise<kj::AuthenticatedStream> wrapServer(kj::AuthenticatedStream stream) override {
    // The transport (network) peer identity is passed through untouched; server.c++ unwraps a
    // kj TlsPeerIdentity to exactly that under the default build (see unwrapTlsPeerIdentity).
    auto conn = rust::kj_hyper::new_rustls_server_conn(getServerConfig());
    stream.stream = kj::heap<RustlsStream>(kj::mv(stream.stream), kj::mv(conn));
    co_return kj::mv(stream);
  }

  kj::Promise<kj::AuthenticatedStream> wrapClient(kj::AuthenticatedStream, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("rust I/O backend: AuthenticatedStream client TLS is not supported");
  }

  kj::Own<kj::ConnectionReceiver> wrapPort(kj::Own<kj::ConnectionReceiver> port) override {
    // Force the server-config build here so an unusable listener configuration (no keypair,
    // options rustls cannot honor) throws at listen setup rather than on the first accepted
    // connection.
    getServerConfig();
    return kj::heap<RustlsConnectionReceiver>(*this, kj::mv(port));
  }

  kj::Own<kj::NetworkAddress> wrapAddress(
      kj::Own<kj::NetworkAddress> address, kj::StringPtr expectedServerHostname) override {
    return kj::heap<RustlsTlsNetworkAddress>(
        *this, kj::str(expectedServerHostname), kj::mv(address));
  }
  kj::Own<kj::Network> wrapNetwork(kj::Network& network) override {
    return kj::heap<RustlsTlsNetwork>(
        *this, kj::Own<kj::Network>(&network, kj::NullDisposer::instance));
  }

 private:
  ::rust::Box<rust::kj_hyper::HyperTlsClientConfig> clientConfig;

  // The server side of the configuration: the not-yet-built options, the built rustls server
  // config, or the exception explaining why no server config can exist (no keypair, or a
  // previous build attempt failed).
  kj::OneOf<kj::Exception,
      rust::kj_hyper::TlsServerOptions,
      ::rust::Box<rust::kj_hyper::HyperTlsServerConfig>>
      serverState;

  // Build (once) and return the rustls server config; throws the recorded exception when the
  // configuration has no usable server side.
  const rust::kj_hyper::HyperTlsServerConfig& getServerConfig() {
    KJ_SWITCH_ONEOF(serverState) {
      KJ_CASE_ONEOF(e, kj::Exception) {
        kj::throwFatalException(e.clone());
      }
      KJ_CASE_ONEOF(options, rust::kj_hyper::TlsServerOptions) {
        auto taken = kj::mv(options);
        // Attempt the build exactly once: on failure, record the exception so later calls
        // rethrow it instead of rebuilding from moved-from options.
        kj::Maybe<::rust::Box<rust::kj_hyper::HyperTlsServerConfig>> built;
        KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
          built = rust::kj_hyper::newHyperTlsServerConfig(kj::mv(taken));
        })) {
          serverState.init<kj::Exception>(exception.clone());
          kj::throwFatalException(kj::mv(exception));
        }
        return *serverState.init<::rust::Box<rust::kj_hyper::HyperTlsServerConfig>>(
            kj::mv(KJ_ASSERT_NONNULL(built)));
      }
      KJ_CASE_ONEOF(config, ::rust::Box<rust::kj_hyper::HyperTlsServerConfig>) {
        return *config;
      }
    }
    KJ_UNREACHABLE;
  }
};

// Extracts config::TlsOptions into the FFI structs rustls consumes; see
// src/rust/cxx/kj-hyper/tls.rs for the full mapping and its documented divergences. Mirrors the
// default build's kj::TlsContext::Options mapping field-for-field. Throws on unknown minVersion
// values; configurations rustls cannot honor (minVersion below TLS 1.2, cipherList with no
// supported cipher, bad PEM) throw from the rust-side config builders with clear messages.
rust::kj_hyper::TlsMinVersion makeHyperTlsMinVersion(config::TlsOptions::Reader conf) {
  switch (conf.getMinVersion()) {
    case config::TlsOptions::Version::GOOD_DEFAULT:
      return rust::kj_hyper::TlsMinVersion::GOOD_DEFAULT;
    case config::TlsOptions::Version::SSL3:
      return rust::kj_hyper::TlsMinVersion::SSL3;
    case config::TlsOptions::Version::TLS1_DOT0:
      return rust::kj_hyper::TlsMinVersion::TLS1_0;
    case config::TlsOptions::Version::TLS1_DOT1:
      return rust::kj_hyper::TlsMinVersion::TLS1_1;
    case config::TlsOptions::Version::TLS1_DOT2:
      return rust::kj_hyper::TlsMinVersion::TLS1_2;
    case config::TlsOptions::Version::TLS1_DOT3:
      return rust::kj_hyper::TlsMinVersion::TLS1_3;
  }
  KJ_FAIL_REQUIRE("Encountered unknown TlsOptions::minVersion setting. Was the config "
                  "compiled with a newer version of the schema?");
}

rust::kj_hyper::TlsClientOptions makeHyperTlsClientOptions(config::TlsOptions::Reader conf) {
  rust::kj_hyper::TlsClientOptions options;
  options.trust_system_roots = conf.getTrustBrowserCas();

  for (auto cert: conf.getTrustedCertificates()) {
    options.trusted_certificates.push_back(::rust::String(cert.begin(), cert.size()));
  }

  if (conf.hasKeypair()) {
    auto pairConf = conf.getKeypair();
    auto chain = pairConf.getCertificateChain();
    auto key = pairConf.getPrivateKey();
    options.certificate_chain = ::rust::String(chain.begin(), chain.size());
    options.private_key = ::rust::String(key.begin(), key.size());
  }

  options.min_version = makeHyperTlsMinVersion(conf);

  if (conf.hasCipherList()) {
    auto cipherList = conf.getCipherList();
    options.cipher_list = ::rust::String(cipherList.begin(), cipherList.size());
  }

  return options;
}

rust::kj_hyper::TlsServerOptions makeHyperTlsServerOptions(config::TlsOptions::Reader conf) {
  rust::kj_hyper::TlsServerOptions options;

  if (conf.hasKeypair()) {
    auto pairConf = conf.getKeypair();
    auto chain = pairConf.getCertificateChain();
    auto key = pairConf.getPrivateKey();
    options.certificate_chain = ::rust::String(chain.begin(), chain.size());
    options.private_key = ::rust::String(key.begin(), key.size());
  }

  options.require_client_certs = conf.getRequireClientCerts();
  options.trust_system_roots = conf.getTrustBrowserCas();

  for (auto cert: conf.getTrustedCertificates()) {
    options.trusted_certificates.push_back(::rust::String(cert.begin(), cert.size()));
  }

  options.min_version = makeHyperTlsMinVersion(conf);

  if (conf.hasCipherList()) {
    auto cipherList = conf.getCipherList();
    options.cipher_list = ::rust::String(cipherList.begin(), cipherList.size());
  }

  return options;
}

}  // namespace

kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
  // The client config is built eagerly, matching kj::TlsContext's throw-at-construction on a
  // bad configuration (rustls additionally rejects minVersion below TLS 1.2 and cipher lists
  // naming no supported suite; see tls.rs). The server config is built lazily on first
  // wrapPort()/wrapServer(), because client-only configurations legitimately have no keypair.
  // `reportConfigError` is unused here: every unusable case throws (kj only soft-reports the
  // unknown-minVersion case, which throws under this backend).
  (void)reportConfigError;
  auto clientConfig = rust::kj_hyper::newHyperTlsClientConfig(makeHyperTlsClientOptions(conf));
  return kj::heap<RustlsSecureNetworkWrapper>(
      kj::mv(clientConfig), makeHyperTlsServerOptions(conf), conf.hasKeypair());
}

kj::PeerIdentity& unwrapTlsPeerIdentity(kj::PeerIdentity& peerIdentity) {
  // The rustls wrapper's accepted streams carry the transport identity directly; there is no
  // TLS identity wrapper to unwrap (and kj::TlsPeerIdentity, a kj-tls type, does not exist
  // under this backend).
  return peerIdentity;
}

kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper() {
  // trust_system_roots with no explicit anchors: rustls delegates verification to the platform
  // verifier (rustls-platform-verifier) -- the same default trust as the "internet" service.
  rust::kj_hyper::TlsClientOptions tlsOptions;
  tlsOptions.trust_system_roots = true;
  tlsOptions.min_version = rust::kj_hyper::TlsMinVersion::GOOD_DEFAULT;
  return kj::heap<RustlsSecureNetworkWrapper>(
      rust::kj_hyper::newHyperTlsClientConfig(kj::mv(tlsOptions)));
}

}  // namespace workerd::server

#else  // WORKERD_RUST_IO_BACKEND_RUST

#include <kj/compat/tls.h>

namespace workerd::server {

kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper() {
  kj::TlsContext::Options options;
  options.useSystemTrustStore = true;
  return kj::heap<kj::TlsContext>(kj::mv(options));
}

kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
  // Upstream Server::makeTlsContext(), verbatim (kj::TlsContext is a kj::SecureNetworkWrapper).
  kj::TlsContext::Options options;

  struct Attachments {
    kj::Maybe<kj::TlsKeypair> keypair;
    kj::Array<kj::TlsCertificate> trustedCerts;
  };
  auto attachments = kj::heap<Attachments>();

  if (conf.hasKeypair()) {
    auto pairConf = conf.getKeypair();
    options.defaultKeypair = attachments->keypair.emplace(
        kj::TlsKeypair{.privateKey = kj::TlsPrivateKey(pairConf.getPrivateKey()),
          .certificate = kj::TlsCertificate(pairConf.getCertificateChain())});
  }

  options.verifyClients = conf.getRequireClientCerts();
  options.useSystemTrustStore = conf.getTrustBrowserCas();

  auto trustList = conf.getTrustedCertificates();
  if (trustList.size() > 0) {
    attachments->trustedCerts = KJ_MAP(cert, trustList) { return kj::TlsCertificate(cert); };
    options.trustedCertificates = attachments->trustedCerts;
  }

  switch (conf.getMinVersion()) {
    case config::TlsOptions::Version::GOOD_DEFAULT:
      // Don't change.
      goto validVersion;
    case config::TlsOptions::Version::SSL3:
      options.minVersion = kj::TlsVersion::SSL_3;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT0:
      options.minVersion = kj::TlsVersion::TLS_1_0;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT1:
      options.minVersion = kj::TlsVersion::TLS_1_1;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT2:
      options.minVersion = kj::TlsVersion::TLS_1_2;
      goto validVersion;
    case config::TlsOptions::Version::TLS1_DOT3:
      options.minVersion = kj::TlsVersion::TLS_1_3;
      goto validVersion;
  }
  reportConfigError(kj::str("Encountered unknown TlsOptions::minVersion setting. Was the "
                            "config compiled with a newer version of the schema?"));

validVersion:
  if (conf.hasCipherList()) {
    options.cipherList = conf.getCipherList();
  }

  return kj::heap<kj::TlsContext>(kj::mv(options)).attach(kj::mv(attachments));
}

kj::PeerIdentity& unwrapTlsPeerIdentity(kj::PeerIdentity& peerIdentity) {
  // Upstream server.c++'s HttpListener::run() identity unwrap, verbatim.
  KJ_IF_SOME(tlsId, kj::tryDowncast<kj::TlsPeerIdentity>(peerIdentity)) {
    // TODO(someday): Add client certificate info to the cf blob? At present, KJ only
    //   supplies the common name, but that doesn't even seem to be one of the fields that
    //   Cloudflare-hosted Workers receive. We should probably try to match those.
    return tlsId.getNetworkIdentity();
  }
  return peerIdentity;
}

}  // namespace workerd::server

#endif  // WORKERD_RUST_IO_BACKEND_RUST
