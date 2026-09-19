// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// The build's TLS engine as a kj::SecureNetworkWrapper: rustls (kj-hyper) under
// --//:io_backend=rust, where kj-tls is not linked, or kj::TlsContext (OpenSSL) under
// --//:io_backend=cxx.

#include <workerd/server/workerd.capnp.h>

#include <kj/async-io.h>
#include <kj/function.h>

namespace workerd::server {

// Client-side TLS with the system trust store, as the default "internet" service uses.
kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper();

// The TLS engine configured by `conf`. rustls speaks only TLS 1.2 and 1.3, takes a cipherList only
// as a colon-separated list of TLS 1.2 suite names, and does not accept client certificates from
// the browser CAs (requireClientCerts with trustBrowserCas); it refuses such configurations.
// `reportConfigError` receives non-fatal diagnostics.
kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError);

// The network identity under an accepted TLS connection's peer identity.
kj::PeerIdentity& unwrapTlsPeerIdentity(kj::PeerIdentity& peerIdentity);

}  // namespace workerd::server
