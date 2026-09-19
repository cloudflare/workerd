// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "tls-network.h"

#include <kj/debug.h>

#if WORKERD_RUST_IO_BACKEND_RUST

#include <kj-hyper/hyper-http.h>

namespace workerd::server {
namespace {

// kj's TlsNetwork, TlsNetworkAddress and TlsConnectionReceiver live in kj-tls (OpenSSL); these are
// their counterparts over any kj::SecureNetworkWrapper.

class RustlsTlsNetworkAddress final: public kj::NetworkAddress {
 public:
  RustlsTlsNetworkAddress(
      kj::SecureNetworkWrapper& tls, kj::String hostname, kj::Own<kj::NetworkAddress> inner)
      : tls(tls),
        hostname(kj::mv(hostname)),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
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

class RustlsTlsNetwork final: public kj::Network {
 public:
  RustlsTlsNetwork(kj::SecureNetworkWrapper& tls, kj::Own<kj::Network> inner)
      : tls(tls),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::NetworkAddress>> parseAddress(
      kj::StringPtr addr, uint portHint) override {
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

// rustls behind kj::SecureNetworkWrapper (kj-hyper tls.rs).
class RustlsSecureNetworkWrapper final: public kj::SecureNetworkWrapper {
 public:
  RustlsSecureNetworkWrapper(::rust::Box<rust::kj_hyper::TlsClientConfig> client,
      kj::OneOf<kj::Exception, ::rust::Box<rust::kj_hyper::TlsServerConfig>> server)
      : client(kj::mv(client)),
        server(kj::mv(server)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapClient(
      kj::Own<kj::AsyncIoStream> stream, kj::StringPtr expectedServerHostname) override {
    return rust::kj_hyper::wrapTlsClient(kj::mv(stream), *client, expectedServerHostname);
  }
  kj::Promise<kj::Own<kj::AsyncIoStream>> wrapServer(kj::Own<kj::AsyncIoStream> stream) override {
    return rust::kj_hyper::wrapTlsServer(kj::mv(stream), serverConfig());
  }
  kj::Promise<kj::AuthenticatedStream> wrapServer(kj::AuthenticatedStream stream) override {
    stream.stream = rust::kj_hyper::wrapTlsServer(kj::mv(stream.stream), serverConfig());
    return kj::mv(stream);
  }
  kj::Promise<kj::AuthenticatedStream> wrapClient(kj::AuthenticatedStream, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("AuthenticatedStream client TLS");
  }
  kj::Own<kj::ConnectionReceiver> wrapPort(kj::Own<kj::ConnectionReceiver> port) override {
    serverConfig();
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
  const rust::kj_hyper::TlsServerConfig& serverConfig() {
    KJ_SWITCH_ONEOF(server) {
      KJ_CASE_ONEOF(e, kj::Exception) {
        kj::throwFatalException(e.clone());
      }
      KJ_CASE_ONEOF(config, ::rust::Box<rust::kj_hyper::TlsServerConfig>) {
        return *config;
      }
    }
    KJ_UNREACHABLE;
  }

  ::rust::Box<rust::kj_hyper::TlsClientConfig> client;
  kj::OneOf<kj::Exception, ::rust::Box<rust::kj_hyper::TlsServerConfig>> server;
};

rust::kj_hyper::TlsOptions tlsOptions(config::TlsOptions::Reader conf) {
  rust::kj_hyper::TlsOptions options;
  options.trust_system_roots = conf.getTrustBrowserCas();
  for (auto cert: conf.getTrustedCertificates()) {
    options.trusted_certificates.push_back(::rust::String(cert.begin(), cert.size()));
  }
  if (conf.hasKeypair()) {
    auto keypair = conf.getKeypair();
    options.certificate_chain = ::rust::String(keypair.getCertificateChain().cStr());
    options.private_key = ::rust::String(keypair.getPrivateKey().cStr());
  }
  options.require_client_certs = conf.getRequireClientCerts();
  options.min_tls13 = conf.getMinVersion() == config::TlsOptions::Version::TLS1_DOT3;
  if (conf.hasCipherList()) {
    options.cipher_list = ::rust::String(conf.getCipherList().cStr());
  }
  return options;
}

}  // namespace

kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
  auto options = tlsOptions(conf);
  kj::OneOf<kj::Exception, ::rust::Box<rust::kj_hyper::TlsServerConfig>> server =
      KJ_EXCEPTION(FAILED, "this TLS configuration has no keypair, so it cannot serve TLS");
  if (conf.hasKeypair()) {
    server = rust::kj_hyper::new_tls_server_config(options);
  }
  return kj::heap<RustlsSecureNetworkWrapper>(
      rust::kj_hyper::new_tls_client_config(options), kj::mv(server));
}

kj::PeerIdentity& unwrapTlsPeerIdentity(kj::PeerIdentity& peerIdentity) {
  return peerIdentity;
}

kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper() {
  rust::kj_hyper::TlsOptions options;
  options.trust_system_roots = true;
  return kj::heap<RustlsSecureNetworkWrapper>(rust::kj_hyper::new_tls_client_config(options),
      KJ_EXCEPTION(FAILED, "a system-trust TLS context cannot serve TLS"));
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
