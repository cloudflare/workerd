// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tls-network.h over rustls (kj-hyper tls.rs), for --//:io_backend=rust.

#include "tls-network.h"

#include <kj-hyper/hyper-http.h>

#include <kj/async-queue.h>
#include <kj/debug.h>

namespace workerd::server {
namespace {

// kj's TlsNetwork, TlsNetworkAddress and TlsConnectionReceiver live in kj-tls (OpenSSL); these are
// their counterparts over any kj::SecureNetworkWrapper.

class TlsNetworkAddress final: public kj::NetworkAddress {
 public:
  TlsNetworkAddress(
      kj::SecureNetworkWrapper& tls, kj::String hostname, kj::Own<kj::NetworkAddress> inner)
      : tls(tls),
        hostname(kj::mv(hostname)),
        inner(kj::mv(inner)) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> connect() override {
    // As kj: callers may drop the address once connect() returns, so the promise copies.
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
    return kj::heap<TlsNetworkAddress>(tls, kj::str(hostname), inner->clone());
  }
  kj::String toString() override {
    return kj::str("tls:", inner->toString());
  }

 private:
  kj::SecureNetworkWrapper& tls;
  kj::String hostname;
  kj::Own<kj::NetworkAddress> inner;
};

class TlsNetwork final: public kj::Network {
 public:
  TlsNetwork(kj::SecureNetworkWrapper& tls, kj::Own<kj::Network> inner)
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
      return kj::heap<TlsNetworkAddress>(tlsRef, kj::mv(hostname), kj::mv(addr));
    });
  }

  kj::Own<kj::NetworkAddress> getSockaddr(const void* sockaddr, uint len) override {
    KJ_UNIMPLEMENTED("TLS does not implement getSockaddr() because it needs to know hostnames");
  }
  kj::Own<kj::Network> restrictPeers(
      kj::ArrayPtr<const kj::StringPtr> allow, kj::ArrayPtr<const kj::StringPtr> deny) override {
    return kj::heap<TlsNetwork>(tls, inner->restrictPeers(allow, deny));
  }

 private:
  kj::SecureNetworkWrapper& tls;
  kj::Own<kj::Network> inner;
};

// As kj's: accepts continuously, and each handshake (client authentication included) runs
// concurrently, so a slow or silent peer holds up no one; connections are handed out once they
// complete.
class TlsConnectionReceiver final: public kj::ConnectionReceiver,
                                   private kj::TaskSet::ErrorHandler {
 public:
  TlsConnectionReceiver(kj::SecureNetworkWrapper& tls, kj::Own<kj::ConnectionReceiver> inner)
      : tls(tls),
        inner(kj::mv(inner)),
        acceptLoopTask(acceptLoop().eagerlyEvaluate(
            [this](kj::Exception&& e) { onAcceptFailure(kj::mv(e)); })) {}

  kj::Promise<kj::Own<kj::AsyncIoStream>> accept() override {
    auto stream = co_await acceptAuthenticated();
    co_return kj::mv(stream.stream);
  }

  kj::Promise<kj::AuthenticatedStream> acceptAuthenticated() override {
    KJ_IF_SOME(e, innerException) {
      return e.clone();
    }
    return queue.pop();
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
  void taskFailed(kj::Exception&& e) override {
    if (e.getType() != kj::Exception::Type::DISCONNECTED) {
      KJ_LOG(ERROR, "error accepting tls connection", e);
    }
  }

  kj::Promise<void> acceptLoop() {
    for (;;) {
      auto accepted = co_await inner->acceptAuthenticated();
      tasks.add(handshake(kj::mv(accepted)));
    }
  }

  kj::Promise<void> handshake(kj::AuthenticatedStream accepted) {
    queue.push(co_await tls.wrapServer(kj::mv(accepted)));
  }

  void onAcceptFailure(kj::Exception&& e) {
    innerException = kj::mv(e);
    queue.rejectAll(KJ_ASSERT_NONNULL(innerException).clone());
  }

  kj::SecureNetworkWrapper& tls;
  kj::Own<kj::ConnectionReceiver> inner;
  kj::ProducerConsumerQueue<kj::AuthenticatedStream> queue;
  kj::Maybe<kj::Exception> innerException;
  kj::TaskSet tasks{*this};
  kj::Promise<void> acceptLoopTask;
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
    auto peerIdentity = kj::mv(stream.peerIdentity);
    auto wrapped = co_await rust::kj_hyper::wrapTlsServer(kj::mv(stream.stream), serverConfig());
    co_return kj::AuthenticatedStream{kj::mv(wrapped), kj::mv(peerIdentity)};
  }
  kj::Promise<kj::AuthenticatedStream> wrapClient(kj::AuthenticatedStream, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("AuthenticatedStream client TLS");
  }
  kj::Own<kj::ConnectionReceiver> wrapPort(kj::Own<kj::ConnectionReceiver> port) override {
    serverConfig();
    return kj::heap<TlsConnectionReceiver>(*this, kj::mv(port));
  }
  kj::Own<kj::NetworkAddress> wrapAddress(
      kj::Own<kj::NetworkAddress> address, kj::StringPtr expectedServerHostname) override {
    return kj::heap<TlsNetworkAddress>(*this, kj::str(expectedServerHostname), kj::mv(address));
  }
  kj::Own<kj::Network> wrapNetwork(kj::Network& network) override {
    return kj::heap<TlsNetwork>(*this, kj::Own<kj::Network>(&network, kj::NullDisposer::instance));
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

rust::kj_hyper::TlsOptions tlsOptions(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
  rust::kj_hyper::TlsOptions options;
  options.trust_system_roots = conf.getTrustBrowserCas();
  for (auto cert: conf.getTrustedCertificates()) {
    options.trusted_certificates.push_back(::rust::String::lossy(cert.begin(), cert.size()));
  }
  if (conf.hasKeypair()) {
    auto keypair = conf.getKeypair();
    // Non-UTF-8 bytes can only make the PEM invalid, which configuration then reports.
    auto chain = keypair.getCertificateChain();
    auto key = keypair.getPrivateKey();
    options.certificate_chain = ::rust::String::lossy(chain.begin(), chain.size());
    options.private_key = ::rust::String::lossy(key.begin(), key.size());
  }
  options.require_client_certs = conf.getRequireClientCerts();
  switch (conf.getMinVersion()) {
    // rustls speaks TLS 1.2 and 1.3 only, so every lower floor allows both.
    case config::TlsOptions::Version::GOOD_DEFAULT:
    case config::TlsOptions::Version::SSL3:
    case config::TlsOptions::Version::TLS1_DOT0:
    case config::TlsOptions::Version::TLS1_DOT1:
    case config::TlsOptions::Version::TLS1_DOT2:
      break;
    case config::TlsOptions::Version::TLS1_DOT3:
      options.min_tls13 = true;
      break;
    default:
      reportConfigError(kj::str("Encountered unknown TlsOptions::minVersion setting. Was the "
                                "config compiled with a newer version of the schema?"));
  }
  if (conf.hasCipherList()) {
    auto cipherList = conf.getCipherList();
    options.cipher_list = ::rust::String::lossy(cipherList.begin(), cipherList.size());
  }
  return options;
}

}  // namespace

kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
  auto options = tlsOptions(conf, reportConfigError);
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
