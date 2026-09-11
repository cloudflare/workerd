// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// One TLS-engine surface for both build configs. Callers get a kj::SecureNetworkWrapper
// (wrapNetwork()/wrapAddress()/wrapClient() for the client directions, wrapPort()/wrapServer()
// for TLS listeners) backed by the build's TLS engine -- kj::TlsContext (OpenSSL) in the
// default build, rustls (the src/rust/cxx/kj-hyper record state machine) under
// --//:io_backend=rust, where kj-tls must not be linked -- so call sites (server.c++'s TLS
// paths, pyodide.c++'s bundle download) need no #if per config.

#include <workerd/server/workerd.capnp.h>

#include <kj/async-io.h>
#include <kj/function.h>

namespace workerd::server {

// A kj::SecureNetworkWrapper with default (system) trust: kj::TlsContext with
// useSystemTrustStore in the default build; under the rust I/O backend, rustls delegating
// verification to the platform verifier (rustls-platform-verifier) -- the same trust the two
// engines use for the default "internet" service. Client-side directions only under the rust
// backend (nothing listens with system trust).
kj::Own<kj::SecureNetworkWrapper> newSystemTrustTlsNetworkWrapper();

// The configured TLS engine, built from config::TlsOptions: server.c++'s replacement for
// constructing a kj::TlsContext directly (kj::TlsContext IS a kj::SecureNetworkWrapper, so the
// default build's object is exactly upstream's). Throws on configurations the engine cannot
// honor at all (kj: bad PEM; rustls additionally: minVersion below TLS 1.2, cipherList naming
// no supported cipher). `reportConfigError` is only invoked synchronously, for non-fatal
// diagnostics (kj's unknown-minVersion case).
//
// Under the rust backend the server side (wrapPort()/wrapServer()) additionally requires a
// keypair; that requirement is checked when a server direction is first used (client-only
// configurations -- external https services, network TLS -- legitimately have no keypair), so
// an unusable listener configuration throws from wrapPort(), i.e. at listen setup.
kj::Own<kj::SecureNetworkWrapper> makeTlsContext(
    config::TlsOptions::Reader conf, kj::FunctionParam<void(kj::String)> reportConfigError);

// The peer identity of a possibly-TLS-wrapped accepted connection, unwrapped to its transport
// (network) identity: under the default build a TLS listener's accepted streams carry a
// kj::TlsPeerIdentity (a kj-tls type unavailable under the rust backend), which this unwraps
// exactly as upstream server.c++ did; the rustls wrapper's accepted streams carry the network
// identity directly, so under the rust backend this is the identity function.
kj::PeerIdentity& unwrapTlsPeerIdentity(kj::PeerIdentity& peerIdentity);

}  // namespace workerd::server
