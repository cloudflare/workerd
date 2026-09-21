// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// tls-network.h over kj::TlsContext (OpenSSL), for --//:io_backend=cxx.

#include "tls-network.h"

#include <kj/compat/tls.h>

namespace workerd::server {

kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper() {
  kj::TlsContext::Options options;
  options.useSystemTrustStore = true;
  return kj::heap<kj::TlsContext>(kj::mv(options));
}

kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError) {
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
  KJ_IF_SOME(tlsId, kj::tryDowncast<kj::TlsPeerIdentity>(peerIdentity)) {
    // TODO(someday): Add client certificate info to the cf blob? At present, KJ only
    //   supplies the common name, but that doesn't even seem to be one of the fields that
    //   Cloudflare-hosted Workers receive. We should probably try to match those.
    return tlsId.getNetworkIdentity();
  }
  return peerIdentity;
}

}  // namespace workerd::server
