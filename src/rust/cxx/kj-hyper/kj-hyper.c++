// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hyper-http.h"

#include <kj/debug.h>
#include <kj/encoding.h>

namespace workerd::rust::kj_hyper {
namespace {

// Bridged Rust futures are cold: start them where kj expects the operation to be in flight.
template <typename T>
kj::Promise<T> hot(kj::Promise<T> promise) {
  return promise.eagerlyEvaluate(nullptr);
}

::rust::Slice<const uint8_t> bytes(kj::ArrayPtr<const kj::byte> data) {
  return {data.begin(), data.size()};
}

::rust::Slice<const uint8_t> bytes(kj::StringPtr text) {
  return {text.asBytes().begin(), text.size()};
}

::rust::Str rustStr(kj::StringPtr text) {
  return {text.begin(), text.size()};
}

class RustIoStream final: public kj::AsyncIoStream {
 public:
  explicit RustIoStream(::rust::Box<RustIo> io): io(kj::mv(io)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return hot(rust_io_read(*io, static_cast<uint8_t*>(buffer), maxBytes, minBytes));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return hot(io->write(bytes(buffer)));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) co_await io->write(bytes(piece));
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return hot(io->when_write_disconnected());
  }
  void shutdownWrite() override {
    shutdownTask = hot(io->shutdown_write()).catch_([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
  }
  void abortRead() override {
    io->abort_read();
  }

  bool canRelease() const {
    return io->can_release();
  }
  ::rust::Box<RustIo> release() {
    return kj::mv(io);
  }

 private:
  ::rust::Box<RustIo> io;
  kj::Promise<void> shutdownTask = kj::READY_NOW;
};

class BodyStream final: public kj::AsyncInputStream {
 public:
  explicit BodyStream(::rust::Box<RustBody> body): body(kj::mv(body)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return hot(body_read(*body, static_cast<uint8_t*>(buffer), maxBytes, minBytes));
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return body_length(*body);
  }

 private:
  ::rust::Box<RustBody> body;
};

class BodySinkStream final: public kj::AsyncOutputStream {
 public:
  explicit BodySinkStream(::rust::Box<BodySink> sink): sink(kj::mv(sink)) {}

  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return hot(sink->write(bytes(buffer)));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto piece: pieces) co_await sink->write(bytes(piece));
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return hot(sink->when_write_disconnected());
  }

 private:
  ::rust::Box<BodySink> sink;
};

// Agreed permessage-deflate parameters as Rust takes them.
WsCompression toRust(const kj::Maybe<kj::CompressionParameters>& parameters) {
  WsCompression result{};
  KJ_IF_SOME(p, parameters) {
    result.enabled = true;
    result.outbound_no_context_takeover = p.outboundNoContextTakeover;
    result.inbound_no_context_takeover = p.inboundNoContextTakeover;
    result.outbound_max_window_bits = static_cast<uint8_t>(p.outboundMaxWindowBits.orDefault(0));
    result.inbound_max_window_bits = static_cast<uint8_t>(p.inboundMaxWindowBits.orDefault(0));
  }
  return result;
}

class RustWebSocketImpl final: public kj::WebSocket {
 public:
  enum class Side { SERVER, CLIENT };

  RustWebSocketImpl(::rust::Box<RustWebSocket> ws,
      kj::Maybe<kj::WebSocketErrorHandler&> errorHandler,
      Side side,
      kj::Maybe<kj::CompressionParameters> compression)
      : ws(kj::mv(ws)),
        errorHandler(errorHandler),
        side(side),
        compression(kj::mv(compression)) {}

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> message) override {
    return hot(ws->send(false, bytes(message)));
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return hot(ws->send(true, bytes(message.asBytes())));
  }
  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return hot(ws->close(code, bytes(reason)));
  }
  void disconnect() override {
    disconnectTask = hot(ws->disconnect());
  }
  void abort() override {
    disconnectTask = hot(ws->abort());
  }
  kj::Promise<void> whenAborted() override {
    return hot(ws->when_aborted());
  }
  kj::Promise<Message> receive(size_t maxSize) override {
    auto message = co_await ws->receive(maxSize);
    auto data = kj::arrayPtr(message.data.data(), message.data.size());
    switch (message.kind) {
      case WsMessageKind::TEXT:
        co_return kj::str(data.asChars());
      case WsMessageKind::BINARY:
        co_return kj::heapArray(data);
      case WsMessageKind::CLOSE:
        co_return Close{message.close_code, kj::str(data.asChars())};
      case WsMessageKind::PROTOCOL_ERROR: {
        auto description = kj::str(data.asChars());
        kj::WebSocketErrorHandler defaultHandler;
        kj::throwFatalException(
            errorHandler.orDefault(defaultHandler)
                .handleWebSocketProtocolError({message.close_code, description}));
      }
    }
    KJ_UNREACHABLE;
  }
  uint64_t sentByteCount() override {
    return ws->sent_byte_count();
  }
  uint64_t receivedByteCount() override {
    return ws->received_byte_count();
  }
  // As kj's WebSocketImpl: a proxy passing this WebSocket through should offer (or agree to)
  // what it runs with, so no recompression is needed; an empty string recommends none.
  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    switch (side) {
      case Side::SERVER:
        if (ctx != ExtensionsContext::REQUEST) return kj::none;
        KJ_IF_SOME(c, compression) {
          return kj::_::generateExtensionResponse(c);
        }
        return kj::String(nullptr);
      case Side::CLIENT:
        if (ctx != ExtensionsContext::RESPONSE) return kj::none;
        KJ_IF_SOME(c, compression) {
          kj::CompressionParameters offer[1]{c};
          return kj::_::generateExtensionRequest(offer);
        }
        return kj::String(nullptr);
    }
    KJ_UNREACHABLE;
  }

 private:
  ::rust::Box<RustWebSocket> ws;
  kj::Maybe<kj::WebSocketErrorHandler&> errorHandler;
  Side side;
  kj::Maybe<kj::CompressionParameters> compression;
  kj::Promise<void> disconnectTask = kj::READY_NOW;
};

class ServerResponseImpl final: public kj::HttpService::Response {
 public:
  explicit ServerResponseImpl(::rust::Box<ServerResponse> impl): impl(kj::mv(impl)) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    sent = true;
    return kj::heap<BodySinkStream>(
        impl->send(statusCode, bytes(statusText.asBytes()), headers, kj::mv(expectedBodySize)));
  }
  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    // A handshake the server must refuse is answered with an error response (RFC 6455 4.2.1-4.2.2).
    switch (impl->websocket_rejection()) {
      case WsRejection::NONE:
        break;
      case WsRejection::NOT_UPGRADE:
        KJ_FAIL_REQUIRE(
            "can't call acceptWebSocket() if the request headers didn't have Upgrade: WebSocket");
      case WsRejection::NOT_GET:
        rejectWebSocket(400, "Bad Request", "WebSocket must be initiated with a GET request.");
      case WsRejection::UNSUPPORTED_VERSION:
        rejectWebSocket(
            426, "Upgrade Required", "The requested WebSocket version is not supported.");
      case WsRejection::MISSING_KEY:
        rejectWebSocket(400, "Bad Request", "Missing Sec-WebSocket-Key");
    }
    sent = true;
    // permessage-deflate, negotiated as kj's HttpServer does in each compression mode.
    kj::Maybe<kj::CompressionParameters> agreed;
    auto rawOffers = impl->websocket_extensions();
    if (rawOffers.size() > 0) {
      auto offers = kj::str(kj::arrayPtr(rawOffers.data(), rawOffers.size()).asChars());
      switch (compressionMode) {
        case kj::HttpServerSettings::AUTOMATIC_COMPRESSION:
          agreed = kj::_::tryParseExtensionOffers(offers);
          break;
        case kj::HttpServerSettings::MANUAL_COMPRESSION:
          KJ_IF_SOME(value, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
            KJ_IF_SOME(manualConfig, kj::_::tryParseExtensionOffers(value)) {
              agreed = kj::_::tryParseAllExtensionOffers(offers, manualConfig);
            }
          }
          break;
        case kj::HttpServerSettings::NO_COMPRESSION:
          break;
      }
    }
    kj::String agreement;
    KJ_IF_SOME(p, agreed) {
      agreement = kj::_::generateExtensionResponse(p);
    }
    auto ws = impl->accept_websocket(
        headers, bytes(agreement == nullptr ? ""_kj : kj::StringPtr(agreement)), toRust(agreed));
    return kj::heap<RustWebSocketImpl>(
        kj::mv(ws), webSocketErrors, RustWebSocketImpl::Side::SERVER, kj::mv(agreed));
  }

  bool sent = false;
  // The error response to a refused handshake, still being written.
  kj::Maybe<kj::Promise<void>> rejection;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  kj::HttpServerSettings::WebSocketCompressionMode compressionMode =
      kj::HttpServerSettings::NO_COMPRESSION;
  void closeAfterSend() {
    impl->close_after_send();
  }

 private:
  // As kj's HttpServer: answers, closes the connection, and fails the service's acceptWebSocket().
  [[noreturn]] void rejectWebSocket(uint status, kj::StringPtr statusText, kj::StringPtr message) {
    kj::HttpHeaderTable table;
    kj::HttpHeaders headers(table);
    headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain");
    if (status == 426) {
      headers.setPtr(kj::HttpHeaderId::SEC_WEBSOCKET_VERSION, "13");
    }
    auto body = kj::str("ERROR: ", message);
    closeAfterSend();
    auto stream = send(status, statusText, headers, body.size());
    rejection = stream->write(body.asBytes()).attach(kj::mv(stream), kj::mv(body));
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "received bad WebSocket handshake", message));
  }

  ::rust::Box<ServerResponse> impl;
};

// kj's HttpServer around the application: failures and missing responses go to the error handler,
// which answers through a response that closes the connection after it is sent.
class ErrorHandlingService final: public kj::HttpService {
 public:
  ErrorHandlingService(kj::HttpService& inner, kj::HttpServerSettings& settings)
      : inner(inner),
        handler(settings.errorHandler.orDefault(defaultHandler)),
        webSocketErrors(settings.webSocketErrorHandler),
        compressionMode(settings.webSocketCompressionMode) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    auto& tracked = kj::downcast<ServerResponseImpl>(response);
    tracked.webSocketErrors = webSocketErrors;
    tracked.compressionMode = compressionMode;
    kj::Maybe<kj::Exception> failure;
    try {
      co_await inner.request(method, url, headers, requestBody, response);
    } catch (...) {
      failure = kj::getCaughtExceptionAsKj();
    }
    KJ_IF_SOME(exception, failure) {
      KJ_IF_SOME(rejection, tracked.rejection) {
        // The refusal is the response; its exception is a side effect of it.
        auto writing = kj::mv(rejection);
        tracked.rejection = kj::none;
        co_return co_await writing;
      }
      kj::Maybe<Response&> unsent;
      if (!tracked.sent) {
        tracked.closeAfterSend();
        unsent = response;
      }
      co_await handler.handleApplicationError(kj::mv(exception), unsent);
    } else if (!tracked.sent) {
      tracked.closeAfterSend();
      co_await handler.handleNoResponse(response);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    return inner.connect(host, headers, connection, response, kj::mv(settings));
  }

 private:
  kj::HttpService& inner;
  kj::HttpServerErrorHandler defaultHandler;
  kj::HttpServerErrorHandler& handler;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  kj::HttpServerSettings::WebSocketCompressionMode compressionMode;
};

class ConnectResponseImpl final: public kj::HttpService::ConnectResponse {
 public:
  explicit ConnectResponseImpl(::rust::Box<ConnectResponder> impl): impl(kj::mv(impl)) {}

  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    impl->accept(statusCode, bytes(statusText.asBytes()), headers);
  }
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    return kj::heap<BodySinkStream>(
        impl->reject(statusCode, bytes(statusText.asBytes()), headers, kj::mv(expectedBodySize)));
  }

 private:
  ::rust::Box<ConnectResponder> impl;
};

class RustHttpClient final: public kj::HttpClient {
 public:
  RustHttpClient(::rust::Box<HyperClient> implParam,
      kj::HttpClientSettings& settings,
      kj::Own<void> keepAlive = {})
      : impl(kj::mv(implParam)),
        webSocketErrors(settings.webSocketErrorHandler),
        errorHandler(settings.errorHandler),
        entropySource(settings.entropySource),
        compressionMode(settings.webSocketCompressionMode),
        keepAlive(kj::mv(keepAlive)),
        driveTask(hot(impl->drive())) {}

  Request request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    auto methodName = kj::str(method);
    auto request =
        impl->request(rustStr(methodName), rustStr(url), headers, kj::mv(expectedBodySize));
    auto body = kj::heap<BodySinkStream>(request->take_body_sink());
    return {kj::mv(body), responseOf(kj::mv(request))};
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const kj::HttpHeaders& headers) override {
    // permessage-deflate, offered and agreed as kj's HttpClient does in each compression mode.
    kj::Vector<kj::CompressionParameters> extensions;
    if (compressionMode == kj::HttpClientSettings::MANUAL_COMPRESSION) {
      KJ_IF_SOME(value, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
        extensions = kj::_::findValidExtensionOffers(value);
      }
    } else if (compressionMode == kj::HttpClientSettings::AUTOMATIC_COMPRESSION) {
      extensions.add(kj::CompressionParameters());
    }
    kj::Maybe<kj::CompressionParameters> clientOffer;
    kj::String offer;
    if (extensions.size() > 0) {
      clientOffer = extensions.front();
      offer = kj::_::generateExtensionRequest(extensions.asPtr());
    }

    kj::byte keyBytes[16]{};
    KJ_ASSERT_NONNULL(entropySource,
        "can't use openWebSocket() because no EntropySource was provided when creating the "
        "HttpClient")
        .generate(keyBytes);
    auto key = kj::encodeBase64(keyBytes);
    auto request = impl->open_websocket(rustStr(url), headers, rustStr(key),
        bytes(offer == nullptr ? ""_kj : kj::StringPtr(offer)));
    auto response = co_await request->response();
    auto code = response->status_code();
    auto& responseHeaders = response->headers();
    auto rawText = response->status_text();
    auto text = kj::str(kj::arrayPtr(rawText.data(), rawText.size()).asChars());
    if (code == 101) {
      auto handshakeError = response->websocket_handshake_error();
      if (handshakeError.size() > 0) {
        auto message = kj::str(kj::arrayPtr(handshakeError.data(), handshakeError.size()));
        co_return errorHandler.orDefault(defaultErrorHandler)
            .handleWebSocketProtocolError({502, "Bad Gateway", message, nullptr});
      }
      kj::Maybe<kj::CompressionParameters> agreed;
      if (compressionMode != kj::HttpClientSettings::NO_COMPRESSION) {
        KJ_IF_SOME(agreement, responseHeaders.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
          KJ_SWITCH_ONEOF(kj::_::tryParseExtensionAgreement(clientOffer, agreement)) {
            KJ_CASE_ONEOF(exception, kj::Exception) {
              co_return errorHandler.orDefault(defaultErrorHandler)
                  .handleWebSocketProtocolError(
                      {502, "Bad Gateway", exception.getDescription(), nullptr});
            }
            KJ_CASE_ONEOF(parameters, kj::CompressionParameters) {
              agreed = kj::mv(parameters);
            }
          }
        }
      }
      auto rust = response->take_websocket(toRust(agreed));
      kj::Own<kj::WebSocket> ws = kj::heap<RustWebSocketImpl>(
          kj::mv(rust), webSocketErrors, RustWebSocketImpl::Side::CLIENT, kj::mv(agreed))
                                      .attach(kj::mv(response));
      kj::StringPtr textPtr = text;
      co_return WebSocketResponse(code, textPtr, &responseHeaders, ws.attach(kj::mv(text)));
    }
    kj::StringPtr textPtr = text;
    kj::Own<kj::AsyncInputStream> body =
        kj::heap<BodyStream>(response->take_body()).attach(kj::mv(response), kj::mv(text));
    co_return WebSocketResponse(code, textPtr, &responseHeaders, kj::mv(body));
  }

  ConnectRequest connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::HttpConnectSettings settings) override {
    auto status = kj::newPromiseAndFulfiller<ConnectRequest::Status>();
    auto tunnel = tunnelOf(impl->connect(rustStr(host), headers), kj::mv(status.fulfiller));
    return {kj::mv(status.promise), kj::newPromisedStream(kj::mv(tunnel))};
  }

 private:
  // The tunnel drives the CONNECT (a promised stream runs eagerly); the status is reported on the
  // way, so callers may use either one alone.
  static kj::Promise<kj::Own<kj::AsyncIoStream>> tunnelOf(::rust::Box<ClientRequest> request,
      kj::Own<kj::PromiseFulfiller<ConnectRequest::Status>> status) {
    kj::Maybe<::rust::Box<ClientResponse>> received;
    try {
      received = co_await request->response();
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      status->reject(kj::Exception(exception.getType(), exception.getFile(), exception.getLine(),
          kj::str(exception.getDescription())));
      kj::throwFatalException(kj::mv(exception));
    }
    auto response = KJ_ASSERT_NONNULL(kj::mv(received));
    auto code = response->status_code();
    auto rawText = response->status_text();
    auto text = kj::str(kj::arrayPtr(rawText.data(), rawText.size()).asChars());
    auto headers = kj::heap<kj::HttpHeaders>(response->headers().clone());
    if (code >= 200 && code < 300) {
      auto io = newRustIoStream(response->take_tunnel());
      status->fulfill(ConnectRequest::Status(code, kj::mv(text), kj::mv(headers)));
      co_return io;
    }
    kj::Own<kj::AsyncInputStream> errorBody =
        kj::heap<BodyStream>(response->take_body()).attach(kj::mv(response));
    status->fulfill(ConnectRequest::Status(code, kj::mv(text), kj::mv(headers), kj::mv(errorBody)));
    kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "CONNECT rejected", code));
  }

  static kj::Promise<Response> responseOf(::rust::Box<ClientRequest> request) {
    auto response = co_await request->response();
    auto code = response->status_code();
    auto& headers = response->headers();
    auto rawText = response->status_text();
    auto text = kj::str(kj::arrayPtr(rawText.data(), rawText.size()).asChars());
    kj::StringPtr textPtr = text;
    auto body = kj::heap<BodyStream>(response->take_body()).attach(kj::mv(response), kj::mv(text));
    co_return Response(code, textPtr, &headers, kj::mv(body));
  }

  ::rust::Box<HyperClient> impl;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  kj::Maybe<kj::HttpClientErrorHandler&> errorHandler;
  kj::HttpClientErrorHandler defaultErrorHandler;
  kj::Maybe<kj::EntropySource&> entropySource;
  kj::HttpClientSettings::WebSocketCompressionMode compressionMode;
  kj::Own<void> keepAlive;
  kj::Promise<void> driveTask;
};

}  // namespace

bool isReleasableRustIo(const kj::AsyncIoStream& stream) {
  KJ_IF_SOME(io, kj::dynamicDowncastIfAvailable<const RustIoStream>(stream)) {
    return io.canRelease();
  }
  return false;
}

::rust::Box<RustIo> releaseRustIo(kj::Own<kj::AsyncIoStream> stream) {
  return KJ_ASSERT_NONNULL(kj::dynamicDowncastIfAvailable<RustIoStream>(*stream)).release();
}

kj::Own<kj::AsyncIoStream> newRustIoStream(::rust::Box<RustIo> io) {
  return kj::heap<RustIoStream>(kj::mv(io));
}

kj::Own<kj::AsyncInputStream> newBodyStream(::rust::Box<RustBody> body) {
  return kj::heap<BodyStream>(kj::mv(body));
}

kj::Own<kj::HttpService::Response> newServerResponse(::rust::Box<ServerResponse> response) {
  return kj::heap<ServerResponseImpl>(kj::mv(response));
}

kj::Own<kj::HttpService::ConnectResponse> newConnectResponse(
    ::rust::Box<ConnectResponder> response) {
  return kj::heap<ConnectResponseImpl>(kj::mv(response));
}

kj::Promise<void> serviceRequest(const kj::HttpService& service,
    ::rust::Slice<const uint8_t> method,
    ::rust::Slice<const uint8_t> url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& body,
    kj::HttpService::Response& response) {
  auto methodName = kj::str(kj::arrayPtr(method.data(), method.size()).asChars());
  auto parsed =
      KJ_REQUIRE_NONNULL(kj::tryParseHttpMethod(methodName), "unsupported HTTP method", methodName);
  auto urlText = kj::str(kj::arrayPtr(url.data(), url.size()).asChars());
  co_await const_cast<kj::HttpService&>(service).request(parsed, urlText, headers, body, response);
}

kj::Promise<void> serviceConnect(const kj::HttpService& service,
    ::rust::Slice<const uint8_t> host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& response) {
  auto hostText = kj::str(kj::arrayPtr(host.data(), host.size()).asChars());
  co_await const_cast<kj::HttpService&>(service).connect(
      hostText, headers, connection, response, {});
}

kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<HttpConnector> connector,
    kj::HttpClientSettings& settings,
    bool proxy) {
  auto impl = new_pooled_client(table, *connector, settings.idleTimeout / kj::MILLISECONDS, proxy);
  return kj::heap<RustHttpClient>(kj::mv(impl), settings, kj::mv(connector));
}

kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    kj::HttpClientSettings& settings) {
  return kj::heap<RustHttpClient>(new_single_client(table, kj::mv(stream), false), settings);
}

kj::Own<kj::HttpClient> newHttpClient(
    const kj::HttpHeaderTable& table, kj::AsyncIoStream& stream, kj::HttpClientSettings& settings) {
  return kj::heap<RustHttpClient>(
      new_single_client(
          table, kj::Own<kj::AsyncIoStream>(&stream, kj::NullDisposer::instance), true),
      settings);
}

kj::Promise<void> HttpConnection::serve() {
  return hot(impl->serve());
}

kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::Own<kj::AsyncIoStream> stream,
    bool lent) {
  auto wrapped = kj::heap<ErrorHandlingService>(service, settings);
  auto impl = new_connection(
      table, *wrapped, kj::mv(stream), lent, settings.headerTimeout / kj::MILLISECONDS);
  return kj::heap<HttpConnection>(kj::mv(impl)).attach(kj::mv(wrapped));
}

kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::Own<kj::AsyncIoStream> stream) {
  return newHttpConnection(table, service, settings, kj::mv(stream), false);
}

kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::AsyncIoStream& stream) {
  return newHttpConnection(table, service, settings,
      kj::Own<kj::AsyncIoStream>(&stream, kj::NullDisposer::instance), true);
}

kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsClient(
    kj::Own<kj::AsyncIoStream> stream, const TlsClientConfig& config, kj::StringPtr hostname) {
  auto name = kj::str(hostname);
  co_return newRustIoStream(co_await wrap_tls_client(kj::mv(stream), config, rustStr(name)));
}

kj::Own<kj::AsyncIoStream> wrapTlsServer(
    kj::Own<kj::AsyncIoStream> stream, const TlsServerConfig& config) {
  return newRustIoStream(wrap_tls_server(kj::mv(stream), config));
}

}  // namespace workerd::rust::kj_hyper
