// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj-hyper/ffi.rs.h>
#include <kj-hyper/peer-filter.h>
#include <kj-rs-http/ffi.h>
#include <kj-rs/kj-rs.h>

#include <kj/compat/http.h>

#include <memory>

namespace workerd::rust::kj_hyper {

// The outcome of HyperHttpService::upgradeRequest(): exactly one of `connection` (the raw
// upgraded byte stream, iff the server answered 101) or `body` (the regular response body,
// typically error details) is present.
struct HyperUpgradeResponse {
  kj::uint statusCode;
  kj::String statusText;
  kj::Maybe<kj::Own<kj::AsyncIoStream>> connection;
  kj::Maybe<kj::Own<kj::AsyncInputStream>> body;
};

// kj::HttpService backed by the Rust hyper HTTP/1.1 client. All requests go to a single
// upstream host:port — plain TCP, or rustls TLS via newHyperHttpsService() — with keep-alive
// reuse; socket I/O runs as tokio tasks on the KJ thread's loop runtime while bodies stream
// (backpressured) through the KJ event loop. WebSocket upgrades follow
// kj::HttpClient::openWebSocket() semantics with workerd's MANUAL_COMPRESSION model; connect()
// issues a real CONNECT; settings.useTls is unsupported, matching kj's HttpClientImpl. See
// client.rs for the implementation.
//
// Known behavioral divergences from kj's client:
// - A WebSocket upgrade attempt yielding a non-101 response closes the connection instead of
//   returning it to the keep-alive pool.
// - CONNECT tunnel bytes written before the 2xx arrives are held until the tunnel is
//   established (kj forwards them optimistically).
// - An I/O-stall watchdog aborts any non-upgraded connection whose outstanding socket I/O
//   makes zero progress for 60 s, and retires idle pooled connections after 60 s (kj's default
//   idleTimeout is 5 s); kj has no watchdog and can wedge on silently dead sockets. See
//   stall.rs.
class HyperHttpService final: public kj::HttpService {
 public:
  explicit HyperHttpService(::rust::Box<HyperClient> impl): impl(kj::mv(impl)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    if (headers.isWebSocket()) {
      return openWebSocket(url, headers, response);
    }
    return impl->request(method, url.asBytes().as<kj_rs::Rust>(), headers, requestBody, response);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    if (settings.useTls) {
      // Matches kj::HttpClientImpl::connect().
      return KJ_EXCEPTION(UNIMPLEMENTED, "This HttpClient does not support TLS.");
    }
    // settings.tlsStarter, if provided, is deliberately left unset (kj's plain-TCP client does
    // not support startTls either).
    return connectImpl(host, headers, connection, response);
  }

  // Sends `method url` asking for an HTTP/1.1 protocol upgrade (Connection: Upgrade plus the
  // caller's Upgrade: header, e.g. Docker's connection-hijacking "Upgrade: tcp"): 101 yields
  // the raw upgraded byte stream, anything else the regular response with its body. `body` is
  // sent complete with an exact Content-Length. This is the client shape kj::HttpClient cannot
  // express (a non-WebSocket 101 taking over the connection); Docker's exec-attach hijack
  // needs it, which is why container-client previously hand-serialized the request.
  kj::Promise<HyperUpgradeResponse> upgradeRequest(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::ArrayPtr<const kj::byte> body) {
    auto outcome = co_await impl->upgrade_request(
        method, url.asBytes().as<kj_rs::Rust>(), headers, body.as<kj_rs::Rust>());
    auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
    if (outcome->is_accepted()) {
      co_return HyperUpgradeResponse{
        .statusCode = outcome->status_code(),
        .statusText = kj::mv(statusText),
        .connection = new_tunnel_stream(outcome->take_tunnel()),
      };
    }
    co_return HyperUpgradeResponse{
      .statusCode = outcome->status_code(),
      .statusText = kj::mv(statusText),
      .body = new_hyper_request_body_stream(outcome->take_body()),
    };
  }

  // A new handle to the same Rust client (shared inner: upstream, keep-alive pool, header
  // table). The composition seam for the kj::newHttpClient(HttpService&) /
  // kj::newHttpService(HttpClient&) fast paths (workerd's rust-backend kj-http shim): the
  // client and service shapes become two views of one client, with no C++ round-trips.
  ::rust::Box<HyperClient> cloneClient() {
    return clone_client(*impl);
  }

 private:
  ::rust::Box<HyperClient> impl;

  // Mirrors kj's HttpServiceAdapter::request() WebSocket branch: on 101 the upgraded WebSocket
  // is paired with response.acceptWebSocket() and the two are pumped both ways; on any other
  // status the regular response is forwarded.
  kj::Promise<void> openWebSocket(
      kj::StringPtr url, const kj::HttpHeaders& headers, Response& response) {
    auto outcome = co_await impl->open_websocket(url.asBytes().as<kj_rs::Rust>(), headers);
    if (outcome->is_websocket()) {
      auto ws = new_rust_websocket(outcome->take_websocket());
      auto ws2 = response.acceptWebSocket(outcome->response_headers());
      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
      promises.add(ws->pumpTo(*ws2));
      promises.add(ws2->pumpTo(*ws));
      co_await kj::joinPromisesFailFast(promises.finish());
    } else {
      auto body = new_hyper_request_body_stream(outcome->take_body());
      auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
      auto out = response.send(
          outcome->status_code(), statusText, outcome->response_headers(), body->tryGetLength());
      co_await body->pumpTo(*out);
    }
  }

  kj::Promise<void> connectImpl(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response) {
    auto outcome = co_await impl->connect_tunnel(host.asBytes().as<kj_rs::Rust>(), headers);
    auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
    if (outcome->is_accepted()) {
      response.accept(outcome->status_code(), statusText, outcome->response_headers());
      auto io = new_tunnel_stream(outcome->take_tunnel());
      // Pump both directions, propagating clean shutdown each way (mirrors kj's
      // HttpServiceAdapter::connect()).
      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
      promises.add(connection.pumpTo(*io).then([&io = *io](uint64_t) { io.shutdownWrite(); }));
      promises.add(
          io->pumpTo(connection).then([&connection](uint64_t) { connection.shutdownWrite(); }));
      co_await kj::joinPromisesFailFast(promises.finish());
    } else {
      auto body = new_hyper_request_body_stream(outcome->take_body());
      auto out = response.reject(
          outcome->status_code(), statusText, outcome->response_headers(), body->tryGetLength());
      co_await body->pumpTo(*out);
    }
  }
};

// Creates a kj::HttpService sending outbound HTTP/1.1 requests to host:port using hyper. The
// calling thread must own a kj_rs_tokio::TokioEventPort by the time of the first request.
// `table` must outlive the returned service; response headers are allocated against it. Wrap
// with kj::newHttpClient(HttpService&) to get the kj::HttpClient shape.
// `jsgifyWebSocketErrors` renders WebSocket protocol errors the way workerd's
// JsgifyWebSocketErrors handler does (mirrors kj's webSocketErrorHandler setting).
// `filter` is the restrictPeers ACL applied at the resolved IP of every dial — the same check
// kj applies inside connect(). Pass newAllowAllHyperPeerFilter() for an unrestricted client.
inline kj::Own<kj::HttpService> newHyperHttpService(const kj::HttpHeaderTable& table,
    kj::StringPtr host,
    uint16_t port,
    std::unique_ptr<HyperPeerFilter> filter,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpService>(new_hyper_http_client(
      table, host.as<kj_rs::RustUncheckedUtf8>(), port, jsgifyWebSocketErrors, kj::mv(filter)));
}

// Builds the shared rustls client configuration for hyper HTTPS services from the outbound
// subset of workerd's config::TlsOptions. One config can back many per-host services (each
// copies a reference, so the Box need not outlive them). Throws for configurations rustls
// cannot honor (bad PEM, minVersion below TLS 1.2, cipherList naming no supported cipher).
// See tls.rs for the full mapping and its divergences.
inline ::rust::Box<HyperTlsClientConfig> newHyperTlsClientConfig(TlsClientOptions options) {
  return new_hyper_tls_client_config(options);
}

// Like newHyperHttpService(), but connections are wrapped in TLS (HTTPS upstream).
// `expectedServerHostname` is the name the server certificate is verified against (also sent
// as SNI) — config's certificateHost, or `host` itself, mirroring kj's makeTlsNetworkAddress().
// Throws if it is not a valid DNS name or IP address. `filter` is applied to the resolved IP
// before the TCP dial, as kj's TLS-wrapped network filters at its inner connect.
inline kj::Own<kj::HttpService> newHyperHttpsService(const kj::HttpHeaderTable& table,
    kj::StringPtr host,
    uint16_t port,
    const HyperTlsClientConfig& tlsConfig,
    kj::StringPtr expectedServerHostname,
    std::unique_ptr<HyperPeerFilter> filter,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpService>(new_hyper_https_client(table,
      host.as<kj_rs::RustUncheckedUtf8>(), port, jsgifyWebSocketErrors, tlsConfig,
      expectedServerHostname.as<kj_rs::RustUncheckedUtf8>(), kj::mv(filter)));
}

// Like newHyperHttpService(), but over one pre-connected stream the caller hands over — the
// kj::newHttpClient(table, stream) single-connection client shape (no dialing, no TLS, no peer
// filter; the stream carries whatever the caller established, e.g. a connected Docker unix
// socket). Once that connection closes, further requests fail DISCONNECTED, like kj's client
// whose stream hit EOF. Returned as the concrete type so callers can also use
// upgradeRequest(); it is a kj::HttpService, and kj::newHttpClient(HttpService&) gives the
// kj::HttpClient shape.
inline kj::Own<HyperHttpService> newHyperStreamHttpService(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpService>(
      new_hyper_stream_http_client(table, kj::mv(stream), jsgifyWebSocketErrors));
}

// kj::HttpClient over the same Rust hyper client HyperHttpService wraps — the native client
// shape, so the kj::HttpClient entry points (workerd's rust-backend kj-http shim) need no
// service->client adapter. request()/openWebSocket()/connect() consume the client-shape
// bridge flows (PendingHttpRequest / WsUpgradeOutcome / TunnelOutcome); see
// hyper-server-ffi.c++ for the implementations.
//
// Lifetimes follow kj::HttpClient's documented contracts: url/headers need only stay valid
// until each call returns (they are translated into Rust-owned state synchronously, or copied
// before the first co_await); each Response/WebSocketResponse's statusText/headers borrow the
// Rust outcome object, which is attached to the returned body/WebSocket — "valid until `body`
// is dropped", exactly as http.h documents (and safe under this client's concurrent
// keep-alive-pooled requests, unlike a per-client last-response slot).
//
// Known behavioral divergences (beyond HyperHttpService's, which all apply):
// - connect(): tunnel bytes written before the 2xx arrives are held until the tunnel is
//   established (kj::HttpClientImpl forwards them optimistically).
class HyperHttpClient final: public kj::HttpClient {
 public:
  explicit HyperHttpClient(::rust::Box<HyperClient> impl)
      : impl(kj::mv(impl)),
        pumpTask(this->impl->drive_stream_pump()) {}

  Request request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override;

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const kj::HttpHeaders& headers) override;

  ConnectRequest connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::HttpConnectSettings settings) override;

  // See HyperHttpService::cloneClient().
  ::rust::Box<HyperClient> cloneClient() {
    return clone_client(*impl);
  }

  // Whether this client can accept another request without failing DISCONNECTED — kj
  // HttpClientImpl::canReuse()'s analog, for pooling wrappers holding single-connection stream
  // clients (workerd's rust-backend kj-http shim). See can_reuse() in ffi.rs.
  bool canReuse() {
    return can_reuse(*impl);
  }

 private:
  ::rust::Box<HyperClient> impl;
  // The stream-tier byte pump for stream clients without a native socket (immediately-resolved
  // otherwise). Owned here — and declared after `impl` — so destroying the client cancels the
  // pump synchronously, before the borrowed underlying stream can die and while the KJ event
  // loop still exists (a detached runtime task would be reaped only at port teardown, after
  // both). See drive_stream_pump() in ffi.rs.
  kj::Promise<void> pumpTask;
};

// The client-shape twins of newHyperHttpService()/newHyperHttpsService()/
// newHyperStreamHttpService() (same argument contracts): a kj::HttpClient over a fresh Rust
// hyper client.
inline kj::Own<HyperHttpClient> newHyperHttpClient(const kj::HttpHeaderTable& table,
    kj::StringPtr host,
    uint16_t port,
    std::unique_ptr<HyperPeerFilter> filter,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpClient>(new_hyper_http_client(
      table, host.as<kj_rs::RustUncheckedUtf8>(), port, jsgifyWebSocketErrors, kj::mv(filter)));
}

inline kj::Own<HyperHttpClient> newHyperHttpsClient(const kj::HttpHeaderTable& table,
    kj::StringPtr host,
    uint16_t port,
    const HyperTlsClientConfig& tlsConfig,
    kj::StringPtr expectedServerHostname,
    std::unique_ptr<HyperPeerFilter> filter,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpClient>(new_hyper_https_client(table,
      host.as<kj_rs::RustUncheckedUtf8>(), port, jsgifyWebSocketErrors, tlsConfig,
      expectedServerHostname.as<kj_rs::RustUncheckedUtf8>(), kj::mv(filter)));
}

inline kj::Own<HyperHttpClient> newHyperStreamHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpClient>(
      new_hyper_stream_http_client(table, kj::mv(stream), jsgifyWebSocketErrors));
}

// kj::newWebSocketPipe()'s replacement under the rust I/O backend: an in-memory
// kj::WebSocket pair backed by the Rust pipe state machine (ws_pipe.rs) -- rendezvous message
// handoff, kj's exact close/disconnect/abort semantics, pump adoption of real sockets, and
// preferred-extensions forwarding through active pumps. Called by //src/workerd/util:kj-http's
// shim; behavior-parity with kj's WebSocketPipeImpl (workerd's tests assert its exact
// exception texts).
kj::WebSocketPipe newRustWebSocketPipe();

// Inbound HTTP/1.1 serving backed by the Rust hyper server, one externally-accepted connection
// at a time: the caller owns the accept loop (workerd's Server::HttpListener under the rust I/O
// backend). Every request is dispatched to the C++ kj::HttpService on the KJ event loop of the
// thread awaiting serve(), with bodies streaming (backpressured) end to end; a client
// disconnect cancels the in-flight service call's promise. WebSocket upgrades follow
// kj::HttpServer semantics; CONNECT dispatches to the service's connect(). No h2. See
// server.rs.
//
// Known behavioral divergences from kj::HttpServer (beyond scope exclusions):
// - Error-response details render only the exception description, not kj's full
//   "file:line: type: description" form; protocol-level 400s come from hyper with hyper's text.
// - whenAborted()/whenWriteDisconnected() fire when a read or write *observes* the peer reset;
//   kj can detect a reset proactively via epoll/kqueue error events with no I/O outstanding.
// - A HEAD response with unknown body size sends no framing header (kj advertises
//   "Transfer-Encoding: chunked").
// - hyper enforces a header read timeout of 30 seconds (kj's default headerTimeout is 15s).
// - After a response completes without the request body having been fully read, hyper may close
//   the connection where kj would have drained up to 64KiB to preserve keep-alive.
// - CONNECT tunnel bytes the client sends before the service accepts are buffered by hyper and
//   surface after acceptance (kj lets the service read them immediately).
// - An I/O-stall watchdog aborts any non-upgraded connection whose peer accepts zero bytes for
//   60 s while a socket write is outstanding (10 s once draining); kj has no watchdog and can
//   wedge indefinitely, including in drain(). See "I/O-stall watchdog" in server.rs.
class HyperHttpConnection final {
 public:
  explicit HyperHttpConnection(::rust::Box<HyperConnection> impl): impl(kj::mv(impl)) {}

  // Serve the connection until it closes (client disconnect, shutdown(), or a completed upgrade
  // handoff); analogous to kj::HttpServer::listenHttp(kj::Own<kj::AsyncIoStream>). May only be
  // called once. Dropping the returned promise aborts the connection and cancels the in-flight
  // service call.
  kj::Promise<void> serve() {
    return impl->serve();
  }

  // Begin a graceful shutdown, the per-connection analog of kj::HttpServer::drain(): an idle
  // connection closes immediately, an in-flight request finishes with "Connection: close", then
  // serve() resolves.
  void shutdown() {
    impl->shutdown();
  }

 private:
  ::rust::Box<HyperConnection> impl;
};

// Creates a HyperHttpConnection serving `service` on the already-connected stream `stream`,
// taking ownership of it. Where possible the socket is taken natively (kj-rs-io's
// take_kj_socket): a kj-rs-io stream gives up its tokio socket outright, any other fd-backed
// stream has its fd duplicated — either way the consumed kj stream is destroyed before this
// returns. Streams with no usable socket (in-memory transports, tunnel streams) are bridged
// through kj-rs-io's duplex pump tier instead, driven by serve(); the stream then lives until
// the pump settles. The caller must pass a *plain* stream — not a TLS or otherwise
// byte-transforming wrapper, whose getFd() would expose the raw transport — with no I/O in
// flight. The calling thread must own a kj_rs_tokio::TokioEventPort, `table` and `service`
// must outlive the returned object, and it must be driven from the thread owning the KJ event
// loop.
// `jsgifyWebSocketErrors`: see newHyperHttpService().
inline kj::Own<HyperHttpConnection> newHyperHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::Own<kj::AsyncIoStream> stream,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpConnection>(
      new_hyper_http_connection(table, service, kj::mv(stream), jsgifyWebSocketErrors));
}

// Builds the shared rustls *server* configuration for hyper-served https sockets from the
// inbound subset of workerd's config::TlsOptions. One config backs every connection of a socket
// (each copies a reference, so the Box need not outlive them). Throws for configurations rustls
// cannot honor (no/bad keypair PEM, minVersion below TLS 1.2, cipherList naming no supported
// cipher, requireClientCerts with no usable trusted certificate). See tls.rs.
inline ::rust::Box<HyperTlsServerConfig> newHyperTlsServerConfig(TlsServerOptions options) {
  return new_hyper_tls_server_config(options);
}

// Like newHyperHttpConnection(), but `stream` is a freshly-accepted https connection (still
// carrying raw, not-yet-decrypted bytes): the server-side TLS handshake (rustls, per
// `tlsConfig`) runs first and hyper serves HTTP/1.1 over the TLS stream. A handshake failure
// surfaces from serve() (mirroring kj's "error accepting tls connection" logging), except peer
// disconnects, which resolve serve() cleanly (kj's TLS receiver silently drops those too).
inline kj::Own<HyperHttpConnection> newHyperHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::Own<kj::AsyncIoStream> stream,
    const HyperTlsServerConfig& tlsConfig,
    bool jsgifyWebSocketErrors = false) {
  return kj::heap<HyperHttpConnection>(new_hyper_http_connection_tls(
      table, service, kj::mv(stream), jsgifyWebSocketErrors, tlsConfig));
}

}  // namespace workerd::rust::kj_hyper
