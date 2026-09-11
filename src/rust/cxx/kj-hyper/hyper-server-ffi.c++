// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hyper-server-ffi.h"

#include <kj-hyper/ffi.rs.h>
#include <kj-hyper/hyper-http.h>
#include <kj-rs-http/ffi.h>
#include <kj-rs/convert.h>

#include <kj/debug.h>

namespace workerd::rust::kj_hyper {
namespace {

// The request body handed to the C++ kj::HttpService, backed by hyper's Incoming body (which
// stays connected to the tokio-side connection; polling it is what makes hyper read request
// bytes off the socket, giving end-to-end upload backpressure). Also used, in the client
// direction, for non-101 WebSocket-attempt responses and CONNECT rejection bodies.
class HyperRequestBodyStream final: public kj::AsyncInputStream {
 public:
  explicit HyperRequestBodyStream(::rust::Box<HyperRequestBody> body): body(kj::mv(body)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return body->read(::rust::Slice<uint8_t>(static_cast<uint8_t*>(buffer), maxBytes), minBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return body->try_get_length();
  }

 private:
  ::rust::Box<HyperRequestBody> body;
};

// The hyper serve path's inline (same-task, pump-free) request body — see ServeRequestBody
// (translate.rs, Stage 4). Same kj::AsyncInputStream contract as HyperRequestBodyStream; polling
// read() is what makes hyper read request bytes off the socket (end-to-end upload backpressure),
// but the frames are pulled directly within serve()'s poll tree rather than a spawned pump.
class ServeRequestBodyStream final: public kj::AsyncInputStream {
 public:
  explicit ServeRequestBodyStream(::rust::Box<ServeRequestBody> body): body(kj::mv(body)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return body->read(::rust::Slice<uint8_t>(static_cast<uint8_t*>(buffer), maxBytes), minBytes);
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return body->try_get_length();
  }

 private:
  ::rust::Box<ServeRequestBody> body;
};

// The response body stream returned from HyperResponseImpl::send() (and from CONNECT reject()),
// feeding the service's writes into the hyper connection through a bounded channel (write
// backpressure).
class HyperResponseBodyStream final: public kj::AsyncOutputStream {
 public:
  explicit HyperResponseBodyStream(::rust::Box<HyperResponseBodySink> sink): sink(kj::mv(sink)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return sink->write(buffer.as<kj_rs::Rust>());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      co_await sink->write(piece.as<kj_rs::Rust>());
    }
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return sink->when_write_disconnected();
  }

 private:
  ::rust::Box<HyperResponseBodySink> sink;
};

// kj::WebSocket over a Rust WsSession (hyper-upgraded connection + kj-parity framing; see
// ws.rs). The default pumpTo()/tryPumpFrom() implementations are inherited: kj's message-level
// pump is exactly what we want, and the optimized byte pump requires two kj-internal
// WebSocketImpls anyway.
class RustWebSocket final: public kj::WebSocket {
 public:
  explicit RustWebSocket(::rust::Box<WsSession> session): session(kj::mv(session)) {}

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> message) override {
    return session->send(false, message.as<kj_rs::Rust>());
  }

  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return session->send(true, message.asBytes().as<kj_rs::Rust>());
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return session->close(code, reason.asBytes().as<kj_rs::Rust>());
  }

  void disconnect() override {
    session->disconnect();
  }

  void abort() override {
    session->abort();
  }

  kj::Promise<void> whenAborted() override {
    return session->when_aborted();
  }

  kj::Promise<Message> receive(size_t maxSize) override {
    auto message = co_await session->receive(maxSize);
    auto bytes = kj::arrayPtr(message.data.data(), message.data.size());
    switch (message.kind) {
      case WsMessageKind::TEXT:
        // kj does not validate UTF-8 in text messages; neither do we.
        co_return Message(kj::str(bytes.asChars()));
      case WsMessageKind::BINARY:
        co_return Message(kj::heapArray<kj::byte>(bytes));
      case WsMessageKind::CLOSE:
        co_return Message(Close{message.close_code, kj::str(bytes.asChars())});
    }
    KJ_UNREACHABLE;
  }

  uint64_t sentByteCount() override {
    return session->sent_byte_count();
  }

  uint64_t receivedByteCount() override {
    return session->received_byte_count();
  }

  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    auto result = session->get_preferred_extensions(ctx == ExtensionsContext::REQUEST);
    if (!result.is_some) {
      return kj::none;
    }
    return kj::heapString(result.value.data(), result.value.size());
  }

 private:
  ::rust::Box<WsSession> session;
};

// kj::AsyncIoStream over a Rust CONNECT tunnel (see upgraded_io.rs). Server-side tunnels become
// live once the service accepts (reads/writes before that simply wait); client-side tunnels are
// live from the start.
class RustTunnelStream final: public kj::AsyncIoStream {
 public:
  explicit RustTunnelStream(::rust::Box<HyperTunnel> tunnel): tunnel(kj::mv(tunnel)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return tunnel->read(::rust::Slice<uint8_t>(static_cast<uint8_t*>(buffer), maxBytes), minBytes);
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return tunnel->write(buffer.as<kj_rs::Rust>());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      co_await tunnel->write(piece.as<kj_rs::Rust>());
    }
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return tunnel->when_write_disconnected();
  }

  void shutdownWrite() override {
    tunnel->shutdown_write();
  }

  void abortRead() override {
    tunnel->abort_read();
  }

 private:
  ::rust::Box<HyperTunnel> tunnel;
};

// The kj::HttpService::ConnectResponse handed to the C++ service for inbound CONNECT requests.
class HyperConnectResponseImpl final: public kj::HttpService::ConnectResponse {
 public:
  explicit HyperConnectResponseImpl(::rust::Box<HyperConnectResponder> responder)
      : responder(kj::mv(responder)) {}

  void accept(
      kj::uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    responder->accept(statusCode, statusText.asBytes().as<kj_rs::Rust>(), headers);
  }

  kj::Own<kj::AsyncOutputStream> reject(kj::uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    auto sink = responder->reject(
        statusCode, statusText.asBytes().as<kj_rs::Rust>(), headers, expectedBodySize);
    return kj::heap<HyperResponseBodyStream>(kj::mv(sink));
  }

 private:
  ::rust::Box<HyperConnectResponder> responder;
};

// The kj::HttpService::Response handed to the C++ service. send() crosses into Rust, which
// applies kj::HttpServer's framing rules and forwards the head to the hyper connection;
// acceptWebSocket() validates and performs the upgrade (see server.rs).
class HyperResponseImpl final: public kj::HttpService::Response {
 public:
  explicit HyperResponseImpl(::rust::Box<HyperResponseSender> sender): sender(kj::mv(sender)) {}

  kj::Own<kj::AsyncOutputStream> send(kj::uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    auto sink =
        sender->send(statusCode, statusText.asBytes().as<kj_rs::Rust>(), headers, expectedBodySize);
    return kj::heap<HyperResponseBodyStream>(kj::mv(sink));
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return kj::heap<RustWebSocket>(sender->accept_websocket(headers));
  }

 private:
  ::rust::Box<HyperResponseSender> sender;
};

}  // namespace

kj::Own<kj::AsyncInputStream> new_hyper_request_body_stream(::rust::Box<HyperRequestBody> body) {
  // normalizeForRust: see kj::rust::normalizeForRust — required for any Own crossing into KjOwn.
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::AsyncInputStream>(kj::heap<HyperRequestBodyStream>(kj::mv(body))));
}

kj::Own<kj::AsyncInputStream> new_serve_request_body_stream(::rust::Box<ServeRequestBody> body) {
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::AsyncInputStream>(kj::heap<ServeRequestBodyStream>(kj::mv(body))));
}

kj::Own<kj::HttpService::Response> new_hyper_response(::rust::Box<HyperResponseSender> sender) {
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::HttpService::Response>(kj::heap<HyperResponseImpl>(kj::mv(sender))));
}

kj::Own<kj::AsyncOutputStream> new_hyper_response_body_stream(
    ::rust::Box<HyperResponseBodySink> sink) {
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::AsyncOutputStream>(kj::heap<HyperResponseBodyStream>(kj::mv(sink))));
}

// One end of the Rust in-memory WebSocket pipe (ws_pipe.rs), as a kj::WebSocket --
// kj::newWebSocketPipe()'s ends under the rust I/O backend (built by newRustWebSocketPipe(),
// called from //src/workerd/util:kj-http's shim). Identical forwarding to RustWebSocket above,
// plus the pipe-specific pump adoption and preferred-extensions forwarding.
class RustWebSocketPipeEnd final: public kj::WebSocket {
 public:
  explicit RustWebSocketPipeEnd(::rust::Box<WsPipeEnd> end): end(kj::mv(end)) {}

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> message) override {
    return end->send_binary(message.as<kj_rs::Rust>());
  }

  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return end->send_text(message.asBytes().as<kj_rs::Rust>());
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return end->close(code, reason.asBytes().as<kj_rs::Rust>());
  }

  void disconnect() override {
    end->disconnect();
  }

  void abort() override {
    end->abort();
  }

  kj::Promise<void> whenAborted() override {
    return end->when_aborted();
  }

  kj::Promise<Message> receive(size_t maxSize) override {
    auto message = co_await end->receive(maxSize);
    auto bytes = kj::arrayPtr(message.data.data(), message.data.size());
    switch (message.kind) {
      case WsMessageKind::TEXT:
        co_return Message(kj::str(bytes.asChars()));
      case WsMessageKind::BINARY:
        co_return Message(kj::heapArray<kj::byte>(bytes));
      case WsMessageKind::CLOSE:
        co_return Message(Close{message.close_code, kj::str(bytes.asChars())});
    }
    KJ_UNREACHABLE;
  }

  kj::Promise<void> pumpTo(kj::WebSocket& other) override {
    return ws_pipe_pump_to(*end, other);
  }

  kj::Maybe<kj::Promise<void>> tryPumpFrom(kj::WebSocket& other) override {
    return ws_pipe_pump_from(*end, other);
  }

  uint64_t sentByteCount() override {
    return end->sent_byte_count();
  }

  uint64_t receivedByteCount() override {
    return end->received_byte_count();
  }

  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    ::rust::String out;
    if (end->peer_preferred_extensions(ctx == ExtensionsContext::REQUEST, out)) {
      return kj::heapString(out.data(), out.size());
    }
    return kj::none;
  }

 private:
  ::rust::Box<WsPipeEnd> end;
};

kj::WebSocketPipe newRustWebSocketPipe() {
  auto pair = new_websocket_pipe();
  return {{kj::heap<RustWebSocketPipeEnd>(kj::mv(pair.end1)),
    kj::heap<RustWebSocketPipeEnd>(kj::mv(pair.end2))}};
}

kj::Own<kj::WebSocket> new_rust_websocket(::rust::Box<WsSession> session) {
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::WebSocket>(kj::heap<RustWebSocket>(kj::mv(session))));
}

kj::Own<kj::AsyncIoStream> new_tunnel_stream(::rust::Box<HyperTunnel> tunnel) {
  return ::kj::rust::normalizeForRust(
      kj::Own<kj::AsyncIoStream>(kj::heap<RustTunnelStream>(kj::mv(tunnel))));
}

kj::Own<kj::HttpService::ConnectResponse> new_hyper_connect_response(
    ::rust::Box<HyperConnectResponder> responder) {
  return ::kj::rust::normalizeForRust(kj::Own<kj::HttpService::ConnectResponse>(
      kj::heap<HyperConnectResponseImpl>(kj::mv(responder))));
}

// Shared-receiver kj::HttpService::request()/connect() shims; see the declarations in
// hyper-server-ffi.h. `const_cast` recovers the non-const reference: kj services are
// shared-reentrant and the Rust side (server.rs) models the service as a shared `&` (see
// `ServicePtr`), so the const only reflects that shared access — the pointee is a live mutable
// service.
kj::Promise<void> hyper_service_request(const kj::HttpService& service,
    kj::HttpMethod method,
    ::rust::Slice<const kj::byte> url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response) {
  auto strUrl = kj::str(kj::from<kj_rs::Rust>(url).asChars());
  co_await const_cast<kj::HttpService&>(service).request(
      method, strUrl, headers, requestBody, response);
}

kj::Promise<void> hyper_service_connect(const kj::HttpService& service,
    ::rust::Slice<const kj::byte> host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& response) {
  auto strHost = kj::str(kj::from<kj_rs::Rust>(host).asChars());
  // kj::HttpServer dispatches CONNECT with default settings ({}): no TLS, no starter.
  co_await const_cast<kj::HttpService&>(service).connect(strHost, headers, connection, response,
      {
        .useTls = false,
        .tlsStarter = kj::none,
      });
}

// =======================================================================================
// HyperHttpClient (the native kj::HttpClient shape; declared in hyper-http.h)

namespace {

// The kj::AsyncOutputStream returned as HttpClient::Request::body: writes feed the Rust
// request-body channel that hyper's connection task drains (end-to-end upload backpressure).
// Destruction ends the body — kj's drop-to-finish contract (the chunked terminator, or hyper's
// short-body failure for an unfinished Content-Length body, follow from the channel closing).
class RequestBodySinkStream final: public kj::AsyncOutputStream {
 public:
  explicit RequestBodySinkStream(::rust::Box<RequestBodySink> sink): sink(kj::mv(sink)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return sink->write(buffer.as<kj_rs::Rust>());
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) {
      co_await sink->write(piece.as<kj_rs::Rust>());
    }
  }

  kj::Promise<void> whenWriteDisconnected() override {
    return sink->when_write_disconnected();
  }

 private:
  ::rust::Box<RequestBodySink> sink;
};

// Await the response head and translate it into kj::HttpClient::Response. The outcome (which
// owns the response headers) and the statusText copy are attached to the returned body, giving
// exactly http.h's documented lifetime ("statusText and headers remain valid until body is
// dropped").
kj::Promise<kj::HttpClient::Response> awaitResponse(::rust::Box<PendingHttpRequest> pending) {
  auto outcome = co_await pending->response();
  auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
  kj::StringPtr statusTextPtr = statusText;
  const kj::HttpHeaders* headersPtr = &outcome->response_headers();
  auto body = new_hyper_request_body_stream(outcome->take_body());
  co_return kj::HttpClient::Response(outcome->status_code(), statusTextPtr, headersPtr,
      body.attach(kj::mv(statusText), kj::mv(outcome)));
}

// The CONNECT flow behind HyperHttpClient::connect(), producing both ConnectRequest halves
// (split() by the caller): the Status, and the tunnel stream — or, on rejection, a stream that
// fails with kj's adapter-parity DISCONNECTED text while the Status carries the error body.
kj::Promise<kj::Tuple<kj::HttpClient::ConnectRequest::Status, kj::Own<kj::AsyncIoStream>>>
connectTunnel(HyperClient& client, kj::String host, kj::HttpHeaders headers) {
  auto outcome = co_await client.connect_tunnel(host.asBytes().as<kj_rs::Rust>(), headers);
  auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
  // ConnectRequest::Status owns its headers; deep-copy out of the outcome.
  auto headersCopy = kj::heap(outcome->response_headers().clone());
  if (outcome->is_accepted()) {
    co_return kj::tuple(kj::HttpClient::ConnectRequest::Status(
                            outcome->status_code(), kj::mv(statusText), kj::mv(headersCopy)),
        new_tunnel_stream(outcome->take_tunnel()));
  }
  auto errorBody = new_hyper_request_body_stream(outcome->take_body());
  co_return kj::tuple(kj::HttpClient::ConnectRequest::Status(outcome->status_code(),
                          kj::mv(statusText), kj::mv(headersCopy), kj::mv(errorBody)),
      kj::newPromisedStream(kj::Promise<kj::Own<kj::AsyncIoStream>>(
          KJ_EXCEPTION(DISCONNECTED, "the connect request was rejected"))));
}

}  // namespace

kj::HttpClient::Request HyperHttpClient::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  // start_request translates everything into Rust-owned state synchronously (kj::HttpClient
  // callers may destroy url/headers as soon as this returns), throwing for an invalid
  // URL/header like kj's serializeRequest.
  auto pending =
      start_request(*impl, method, url.asBytes().as<kj_rs::Rust>(), headers, expectedBodySize);
  auto body = kj::heap<RequestBodySinkStream>(pending->take_body_sink());
  return Request{kj::mv(body), awaitResponse(kj::mv(pending))};
}

kj::Promise<kj::HttpClient::WebSocketResponse> HyperHttpClient::openWebSocket(
    kj::StringPtr url, const kj::HttpHeaders& headers) {
  // kj::HttpClient callers may destroy url/headers as soon as this returns; copy before the
  // first co_await (the service shape borrows instead — its contract spans the whole call).
  auto urlCopy = kj::str(url);
  auto headersCopy = headers.clone();
  auto outcome = co_await impl->open_websocket(urlCopy.asBytes().as<kj_rs::Rust>(), headersCopy);
  auto statusText = kj::str(kj::from<kj_rs::Rust>(outcome->status_text()).asChars());
  kj::StringPtr statusTextPtr = statusText;
  const kj::HttpHeaders* headersPtr = &outcome->response_headers();
  if (outcome->is_websocket()) {
    auto ws = new_rust_websocket(outcome->take_websocket());
    co_return WebSocketResponse(outcome->status_code(), statusTextPtr, headersPtr,
        kj::OneOf<kj::Own<kj::AsyncInputStream>, kj::Own<kj::WebSocket>>(
            ws.attach(kj::mv(statusText), kj::mv(outcome))));
  }
  auto body = new_hyper_request_body_stream(outcome->take_body());
  co_return WebSocketResponse(outcome->status_code(), statusTextPtr, headersPtr,
      kj::OneOf<kj::Own<kj::AsyncInputStream>, kj::Own<kj::WebSocket>>(
          body.attach(kj::mv(statusText), kj::mv(outcome))));
}

kj::HttpClient::ConnectRequest HyperHttpClient::connect(
    kj::StringPtr host, const kj::HttpHeaders& headers, kj::HttpConnectSettings settings) {
  if (settings.useTls) {
    // Matches kj::HttpClientImpl::connect() (and HyperHttpService::connect()).
    auto exception = KJ_EXCEPTION(UNIMPLEMENTED, "This HttpClient does not support TLS.");
    auto connection =
        kj::newPromisedStream(kj::Promise<kj::Own<kj::AsyncIoStream>>(exception.clone()));
    return ConnectRequest{
      kj::Promise<ConnectRequest::Status>(kj::mv(exception)),
      kj::mv(connection),
    };
  }
  // settings.tlsStarter, if provided, is deliberately left unset (kj's plain-TCP client does
  // not support startTls either). The connection half is a promised stream: writes issued
  // before the 2xx arrives wait for the tunnel (see the class divergence note).
  auto split = connectTunnel(*impl, kj::str(host), headers.clone()).split();
  return ConnectRequest{
    kj::mv(kj::get<0>(split)),
    kj::newPromisedStream(kj::mv(kj::get<1>(split))),
  };
}

}  // namespace workerd::rust::kj_hyper
