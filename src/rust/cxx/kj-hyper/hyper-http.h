// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// kj-hyper's C++ API: kj's HTTP interfaces implemented over hyper, rustls and tungstenite's frame
// codec.

#include <kj-hyper/ffi.rs.h>
#include <kj-hyper/kj-hyper.h>

#include <kj/compat/http.h>
#include <kj/time.h>

namespace workerd::rust::kj_hyper {

// Clients honor `settings.idleTimeout` (pooling) and `settings.webSocketErrorHandler`.

// A pooling client whose connections come from `connector`. With `proxy`, request URLs go out in
// absolute form, as to an HTTP proxy; otherwise in origin form.
kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<HttpConnector> connector,
    kj::HttpClientSettings& settings,
    bool proxy = false);

// A single-connection client over `stream`, taken natively where possible.
kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    kj::HttpClientSettings& settings);

// The same over a stream the caller keeps: driven directly, never taken apart.
kj::Own<kj::HttpClient> newHttpClient(
    const kj::HttpHeaderTable& table, kj::AsyncIoStream& stream, kj::HttpClientSettings& settings);

// One accepted connection, served until it closes.
class HttpConnection {
 public:
  explicit HttpConnection(::rust::Box<HyperConnection> impl): impl(kj::mv(impl)) {}
  kj::Promise<void> serve();
  // Graceful shutdown: idle connections close, in-flight requests finish.
  void shutdown() {
    impl->shutdown();
  }

 private:
  ::rust::Box<HyperConnection> impl;
};

// Honors `settings.errorHandler` (application failures, missing responses) and
// `settings.webSocketErrorHandler`, as kj's HttpServer does. `settings` outlives the connection.
kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::Own<kj::AsyncIoStream> stream);
kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::AsyncIoStream& stream);

// TLS over a plaintext stream.
kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsClient(
    kj::Own<kj::AsyncIoStream> stream, const TlsClientConfig& config, kj::StringPtr hostname);
kj::Own<kj::AsyncIoStream> wrapTlsServer(
    kj::Own<kj::AsyncIoStream> stream, const TlsServerConfig& config);

}  // namespace workerd::rust::kj_hyper
