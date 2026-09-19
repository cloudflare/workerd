// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// kj-hyper's C++ API: kj's HTTP interfaces implemented over hyper, rustls and tungstenite's frame
// codec.

#include <kj-hyper/ffi.rs.h>
#include <kj-hyper/kj-hyper.h>

#include <kj/compat/http.h>

namespace workerd::rust::kj_hyper {

// A kj::HttpClient over hyper. It honors `settings.idleTimeout` (pooling), `entropySource`,
// `webSocketCompressionMode`, `webSocketErrorHandler` and `errorHandler`'s WebSocket handshake
// errors; `settings` need only outlive this call, the handlers and entropy source the client.
class HyperHttpClient final: public kj::HttpClient {
 public:
  HyperHttpClient(const kj::HttpHeaderTable& table,
      ::rust::Box<HyperClient> impl,
      kj::HttpClientSettings& settings);

  // To the one origin a connector or stream leads to, with `url` sent as given.
  Request request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override;
  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const kj::HttpHeaders& headers) override;
  ConnectRequest connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::HttpConnectSettings settings) override;

  // Over the pool's connections to `authority`, dialed as `scheme` says.
  Request requestTo(kj::StringPtr authority,
      Scheme scheme,
      kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize);
  kj::Promise<WebSocketResponse> openWebSocketTo(
      kj::StringPtr authority, Scheme scheme, kj::StringPtr url, const kj::HttpHeaders& headers);

 private:
  const kj::HttpHeaderTable& table;
  ::rust::Box<HyperClient> impl;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  kj::Maybe<kj::HttpClientErrorHandler&> errorHandler;
  kj::HttpClientErrorHandler defaultErrorHandler;
  kj::Maybe<kj::EntropySource&> entropySource;
  kj::HttpClientSettings::WebSocketCompressionMode compressionMode;
  kj::Promise<void> driveTask;
};

// A pooling client whose connections come from `connector`.
kj::Own<HyperHttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<HttpConnector> connector,
    kj::HttpClientSettings& settings);

// A client over one connection on `stream`, taken natively where possible. Requests wait their
// turn for the connection.
kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    kj::HttpClientSettings& settings);

// The same over a stream the caller keeps: driven directly, never taken apart.
kj::Own<kj::HttpClient> newHttpClient(
    const kj::HttpHeaderTable& table, kj::AsyncIoStream& stream, kj::HttpClientSettings& settings);

// One accepted connection, served until it closes.
class HttpConnection {
 public:
  HttpConnection(kj::Own<HttpDispatcher> dispatcher, ::rust::Box<HyperConnection> impl)
      : dispatcher(kj::mv(dispatcher)),
        impl(kj::mv(impl)) {}
  kj::Promise<void> serve();
  // Graceful shutdown: idle connections close, in-flight requests finish.
  void shutdown() {
    impl->shutdown();
  }

 private:
  kj::Own<HttpDispatcher> dispatcher;
  ::rust::Box<HyperConnection> impl;
};

// Honors `settings.errorHandler` (application failures, missing responses),
// `webSocketErrorHandler`, `webSocketCompressionMode` and `headerTimeout` (on tokio's clock rather
// than kj's timer), as kj's HttpServer does; `settings` outlives the connection. The rest of
// kj::HttpServerSettings has no counterpart and is ignored: hyper answers unparsable requests
// itself (400, not `handleClientProtocolError()`), and has no pipeline timeout or canceled-upload
// grace period. hyper also parses request targets more strictly than kj: a target RFC 3986
// forbids (a `{`, a non-ASCII byte) is refused with a 400 here, and fails a client's request.
// `listenHttpCleanDrain()` never leaves a connection for reuse: a drain closes it once its
// in-flight request is answered.
kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::Own<kj::AsyncIoStream> stream);
kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::AsyncIoStream& stream);

// TLS over a plaintext stream; each completes the handshake before the stream is handed back.
// A peer that closes without close_notify ends reads with DISCONNECTED, as kj's TLS does.
kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsClient(
    kj::Own<kj::AsyncIoStream> stream, const TlsClientConfig& config, kj::StringPtr hostname);
kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsServer(
    kj::Own<kj::AsyncIoStream> stream, const TlsServerConfig& config);

}  // namespace workerd::rust::kj_hyper
