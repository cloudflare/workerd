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

kj::String str(::rust::Slice<const uint8_t> data) {
  return kj::str(kj::arrayPtr(data.data(), data.size()).asChars());
}

kj::Array<kj::byte> join(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  size_t size = 0;
  for (auto piece: pieces) size += piece.size();
  auto joined = kj::heapArray<kj::byte>(size);
  auto pos = joined.begin();
  for (auto piece: pieces) {
    if (piece.size() > 0) memcpy(pos, piece.begin(), piece.size());
    pos += piece.size();
  }
  return joined;
}

// Headers from a packed block (body.rs's HeaderBlock), checked against the block's bounds.
kj::HttpHeaders headersFrom(const kj::HttpHeaderTable& table,
    ::rust::Slice<const uint8_t> arena,
    ::rust::Slice<const uint32_t> lens) {
  KJ_REQUIRE(lens.size() % 2 == 0, "malformed header block");
  auto owned = kj::heapArray<char>(reinterpret_cast<const char*>(arena.data()), arena.size());
  kj::ArrayPtr<const char> rest = owned;
  auto next = [&](uint32_t len) {
    KJ_REQUIRE(len < rest.size() && rest[len] == '\0', "malformed header block");
    kj::StringPtr text(rest.begin(), len);
    rest = rest.slice(len + 1);
    return text;
  };
  kj::HttpHeaders headers(table);
  for (size_t i = 0; i < lens.size(); i += 2) {
    auto name = next(lens[i]);
    auto value = next(lens[i + 1]);
    headers.addPtrPtr(name, value);
  }
  headers.takeOwnership(kj::mv(owned));
  return headers;
}

class RustIoStream final: public kj::AsyncIoStream {
 public:
  explicit RustIoStream(::rust::Box<RustIo> io): inner(kj::mv(io)) {}

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    return hot(rust_io_read(io(), static_cast<uint8_t*>(buffer), maxBytes, minBytes));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::byte> buffer) override {
    return hot(io().write(bytes(buffer)));
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    auto joined = join(pieces);
    auto promise = io().write(bytes(joined));
    return hot(kj::mv(promise)).attach(kj::mv(joined));
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return hot(rust_io_when_write_disconnected(io()));
  }
  void shutdownWrite() override {
    shutdownTask = hot(io().shutdown_write()).catch_([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
  }
  void abortRead() override {
    io().abort_read();
  }

  bool canRelease() const {
    KJ_IF_SOME(i, inner) {
      return i->can_release();
    }
    return false;
  }
  ::rust::Box<RustIo> release() {
    auto released = KJ_REQUIRE_NONNULL(kj::mv(inner), "stream already released");
    inner = kj::none;
    return released;
  }

 private:
  RustIo& io() {
    return *KJ_REQUIRE_NONNULL(inner, "stream released to kj-hyper");
  }

  kj::Maybe<::rust::Box<RustIo>> inner;
  kj::Promise<void> shutdownTask = kj::READY_NOW;
};

kj::Own<kj::AsyncIoStream> newRustIoStream(::rust::Box<RustIo> io) {
  return kj::heap<RustIoStream>(kj::mv(io));
}

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
    auto joined = join(pieces);
    auto promise = sink->write(bytes(joined));
    return hot(kj::mv(promise)).attach(kj::mv(joined));
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
    ws->abort();
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

// Why a WebSocket handshake must be refused (RFC 6455 4.2.1-4.2.2), with kj's messages.
struct Refusal {
  uint status;
  kj::StringPtr statusText;
  kj::StringPtr message;
};

kj::Maybe<Refusal> websocketRefusal(kj::HttpMethod method, const kj::HttpHeaders& headers) {
  if (method != kj::HttpMethod::GET) {
    return Refusal{400, "Bad Request", "WebSocket must be initiated with a GET request."};
  }
  if (headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_VERSION).orDefault(nullptr) != "13") {
    return Refusal{426, "Upgrade Required", "The requested WebSocket version is not supported."};
  }
  if (headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_KEY) == kj::none) {
    return Refusal{400, "Bad Request", "Missing Sec-WebSocket-Key"};
  }
  return kj::none;
}

// A request's WebSocket handshake, as the dispatcher saw it.
struct WebSocketRequest {
  kj::Maybe<Refusal> refusal;
  // The client's Sec-WebSocket-Extensions offer.
  kj::Maybe<kj::String> offers;
};

class ServerResponseImpl final: public kj::HttpService::Response {
 public:
  ServerResponseImpl(::rust::Box<ServerResponse> impl,
      kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors,
      kj::HttpServerSettings::WebSocketCompressionMode compressionMode,
      kj::Maybe<WebSocketRequest> webSocketRequest)
      : impl(kj::mv(impl)),
        webSocketErrors(webSocketErrors),
        compressionMode(compressionMode),
        webSocketRequest(kj::mv(webSocketRequest)) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    auto sink =
        impl->send(statusCode, bytes(statusText.asBytes()), headers, kj::mv(expectedBodySize));
    sent = true;
    return kj::heap<BodySinkStream>(kj::mv(sink));
  }
  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    auto& request = KJ_REQUIRE_NONNULL(webSocketRequest,
        "can't call acceptWebSocket() if the request headers didn't have Upgrade: WebSocket");
    KJ_IF_SOME(refusal, request.refusal) {
      refuseWebSocket(refusal);
    }
    // permessage-deflate, negotiated as kj's HttpServer does in each compression mode.
    kj::Maybe<kj::CompressionParameters> agreed;
    KJ_IF_SOME(offers, request.offers) {
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
    sent = true;
    return kj::heap<RustWebSocketImpl>(
        kj::mv(ws), webSocketErrors, RustWebSocketImpl::Side::SERVER, kj::mv(agreed));
  }

  void closeAfterSend() {
    impl->close_after_send();
  }
  bool isSent() const {
    return sent;
  }
  // The error response to a refused handshake, still being written.
  kj::Maybe<kj::Promise<void>> takeRefusal() {
    auto refusal = kj::mv(refusalWrite);
    refusalWrite = kj::none;
    return refusal;
  }

 private:
  // As kj's HttpServer: answers, closes the connection, and fails the service's acceptWebSocket().
  [[noreturn]] void refuseWebSocket(const Refusal& refused) {
    kj::HttpHeaderTable table;
    kj::HttpHeaders headers(table);
    headers.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain");
    if (refused.status == 426) {
      headers.setPtr(kj::HttpHeaderId::SEC_WEBSOCKET_VERSION, "13");
    }
    auto body = kj::str("ERROR: ", refused.message);
    closeAfterSend();
    auto stream = send(refused.status, refused.statusText, headers, body.size());
    refusalWrite = stream->write(body.asBytes()).attach(kj::mv(stream), kj::mv(body));
    kj::throwFatalException(
        KJ_EXCEPTION(FAILED, "received bad WebSocket handshake", refused.message));
  }

  ::rust::Box<ServerResponse> impl;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  kj::HttpServerSettings::WebSocketCompressionMode compressionMode;
  kj::Maybe<WebSocketRequest> webSocketRequest;
  bool sent = false;
  kj::Maybe<kj::Promise<void>> refusalWrite;
};

class ConnectResponseImpl final: public kj::HttpService::ConnectResponse {
 public:
  explicit ConnectResponseImpl(::rust::Box<ConnectResponder> impl): impl(kj::mv(impl)) {}

  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_REQUIRE(statusCode >= 200 && statusCode < 300, "the statusCode must be 2xx for accept");
    impl->accept(statusCode, bytes(statusText.asBytes()), headers);
    answered = true;
  }
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_REQUIRE(statusCode < 200 || statusCode >= 300, "the statusCode must not be 2xx for reject.");
    auto sink =
        impl->reject(statusCode, bytes(statusText.asBytes()), headers, kj::mv(expectedBodySize));
    answered = true;
    rejected = true;
    return kj::heap<BodySinkStream>(kj::mv(sink));
  }

  // A plain response to the CONNECT request, which closes the connection once sent.
  kj::Own<ServerResponseImpl> errorResponse(
      kj::HttpServerSettings::WebSocketCompressionMode compressionMode) {
    auto response =
        kj::heap<ServerResponseImpl>(impl->error_response(), kj::none, compressionMode, kj::none);
    response->closeAfterSend();
    return response;
  }
  bool isAnswered() const {
    return answered;
  }
  bool isRejected() const {
    return rejected;
  }

 private:
  ::rust::Box<ConnectResponder> impl;
  bool answered = false;
  bool rejected = false;
};

// The tunnel drives the CONNECT (a promised stream runs eagerly); the status is reported on the
// way, so callers may use either one alone.
kj::Promise<kj::Own<kj::AsyncIoStream>> tunnelOf(const kj::HttpHeaderTable& table,
    ::rust::Box<ClientRequest> request,
    kj::Own<kj::PromiseFulfiller<kj::HttpClient::ConnectRequest::Status>> status) {
  using Status = kj::HttpClient::ConnectRequest::Status;
  kj::Maybe<::rust::Box<ClientResponse>> received;
  try {
    received = co_await request->response();
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    status->reject(exception.clone());
    kj::throwFatalException(kj::mv(exception));
  }
  auto response = KJ_ASSERT_NONNULL(kj::mv(received));
  auto code = response->status_code();
  auto text = str(response->status_text());
  auto headers = kj::heap<kj::HttpHeaders>(
      headersFrom(table, response->header_arena(), response->header_lens()));
  if (code >= 200 && code < 300) {
    auto io = newRustIoStream(response->take_tunnel());
    status->fulfill(Status(code, kj::mv(text), kj::mv(headers)));
    co_return io;
  }
  kj::Own<kj::AsyncInputStream> errorBody =
      kj::heap<BodyStream>(response->take_body()).attach(kj::mv(response));
  status->fulfill(Status(code, kj::mv(text), kj::mv(headers), kj::mv(errorBody)));
  kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "CONNECT rejected", code));
}

kj::Promise<kj::HttpClient::Response> responseOf(
    const kj::HttpHeaderTable& table, ::rust::Box<ClientRequest> request) {
  auto response = co_await request->response();
  auto code = response->status_code();
  auto text = str(response->status_text());
  auto headers = kj::heap<kj::HttpHeaders>(
      headersFrom(table, response->header_arena(), response->header_lens()));
  kj::StringPtr textPtr = text;
  const kj::HttpHeaders* headersPtr = headers.get();
  auto body = kj::heap<BodyStream>(response->take_body())
                  .attach(kj::mv(response), kj::mv(text), kj::mv(headers));
  co_return kj::HttpClient::Response(code, textPtr, headersPtr, kj::mv(body));
}

}  // namespace

HyperHttpClient::HyperHttpClient(const kj::HttpHeaderTable& table,
    ::rust::Box<HyperClient> implParam,
    kj::HttpClientSettings& settings)
    : table(table),
      impl(kj::mv(implParam)),
      webSocketErrors(settings.webSocketErrorHandler),
      errorHandler(settings.errorHandler),
      entropySource(settings.entropySource),
      compressionMode(settings.webSocketCompressionMode),
      driveTask(hot(impl->drive())) {}

kj::HttpClient::Request HyperHttpClient::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  return requestTo(""_kj, Scheme::HTTP, method, url, headers, kj::mv(expectedBodySize));
}

kj::Promise<kj::HttpClient::WebSocketResponse> HyperHttpClient::openWebSocket(
    kj::StringPtr url, const kj::HttpHeaders& headers) {
  return openWebSocketTo(""_kj, Scheme::HTTP, url, headers);
}

kj::HttpClient::Request HyperHttpClient::requestTo(kj::StringPtr authority,
    Scheme scheme,
    kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  auto methodName = kj::str(method);
  auto request = impl->request(rustStr(authority), scheme, rustStr(methodName), rustStr(url),
      headers, kj::mv(expectedBodySize));
  auto body = kj::heap<BodySinkStream>(request->take_body_sink());
  return {kj::mv(body), responseOf(table, kj::mv(request))};
}

kj::Promise<kj::HttpClient::WebSocketResponse> HyperHttpClient::openWebSocketTo(
    kj::StringPtr authority, Scheme scheme, kj::StringPtr url, const kj::HttpHeaders& headers) {
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
  auto request = impl->open_websocket(rustStr(authority), scheme, rustStr(url), headers,
      rustStr(key), bytes(offer == nullptr ? ""_kj : kj::StringPtr(offer)));
  auto response = co_await request->response();
  auto code = response->status_code();
  auto responseHeaders = kj::heap<kj::HttpHeaders>(
      headersFrom(table, response->header_arena(), response->header_lens()));
  const kj::HttpHeaders* headersPtr = responseHeaders.get();
  auto text = str(response->status_text());
  kj::StringPtr textPtr = text;
  if (code == 101) {
    auto handshakeError = response->websocket_handshake_error();
    if (handshakeError.size() > 0) {
      auto message = kj::str(kj::arrayPtr(handshakeError.data(), handshakeError.size()));
      co_return errorHandler.orDefault(defaultErrorHandler)
          .handleWebSocketProtocolError({502, "Bad Gateway", message, nullptr});
    }
    kj::Maybe<kj::CompressionParameters> agreed;
    if (compressionMode != kj::HttpClientSettings::NO_COMPRESSION) {
      KJ_IF_SOME(agreement, responseHeaders->get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
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
    kj::Own<kj::WebSocket> ws =
        kj::heap<RustWebSocketImpl>(
            kj::mv(rust), webSocketErrors, RustWebSocketImpl::Side::CLIENT, kj::mv(agreed))
            .attach(kj::mv(response), kj::mv(text), kj::mv(responseHeaders));
    co_return WebSocketResponse(code, textPtr, headersPtr, kj::mv(ws));
  }
  kj::Own<kj::AsyncInputStream> body =
      kj::heap<BodyStream>(response->take_body())
          .attach(kj::mv(response), kj::mv(text), kj::mv(responseHeaders));
  co_return WebSocketResponse(code, textPtr, headersPtr, kj::mv(body));
}

kj::HttpClient::ConnectRequest HyperHttpClient::connect(
    kj::StringPtr host, const kj::HttpHeaders& headers, kj::HttpConnectSettings settings) {
  auto status = kj::newPromiseAndFulfiller<ConnectRequest::Status>();
  auto tunnel = tunnelOf(table, impl->connect(rustStr(host), headers), kj::mv(status.fulfiller));
  return {kj::mv(status.promise), kj::newPromisedStream(kj::mv(tunnel))};
}

bool isReleasableRustIo(const kj::AsyncIoStream& stream) {
  KJ_IF_SOME(io, kj::dynamicDowncastIfAvailable<const RustIoStream>(stream)) {
    return io.canRelease();
  }
  return false;
}

::rust::Box<RustIo> releaseRustIo(kj::Own<kj::AsyncIoStream> stream) {
  return KJ_ASSERT_NONNULL(kj::dynamicDowncastIfAvailable<RustIoStream>(*stream)).release();
}

void forEachHeader(const kj::HttpHeaders& headers, Head& head) {
  headers.forEach(
      [&](kj::StringPtr name, kj::StringPtr value) { head.append(bytes(name), bytes(value)); });
}

HttpDispatcher::HttpDispatcher(
    const kj::HttpHeaderTable& table, kj::HttpService& service, kj::HttpServerSettings& settings)
    : table(table),
      service(service),
      settings(settings),
      handler(settings.errorHandler.orDefault(defaultHandler)) {}

kj::Promise<void> HttpDispatcher::request(::rust::Slice<const uint8_t> method,
    ::rust::Slice<const uint8_t> url,
    ::rust::Slice<const uint8_t> headerArena,
    ::rust::Slice<const uint32_t> headerLens,
    ::rust::Box<RustBody> body,
    ::rust::Box<ServerResponse> impl) const {
  // Copied before the first suspension: the slices are only borrowed for the call.
  auto methodName = str(method);
  auto parsed =
      KJ_REQUIRE_NONNULL(kj::tryParseHttpMethod(methodName), "unsupported HTTP method", methodName);
  auto urlText = str(url);
  auto headers = headersFrom(table, headerArena, headerLens);
  kj::Maybe<WebSocketRequest> webSocketRequest;
  if (headers.isWebSocket()) {
    webSocketRequest = WebSocketRequest{
      .refusal = websocketRefusal(parsed, headers),
      .offers =
          headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS).map([](kj::StringPtr offers) {
      return kj::str(offers);
    }),
    };
  }
  BodyStream requestBody(kj::mv(body));
  ServerResponseImpl response(kj::mv(impl), settings.webSocketErrorHandler,
      settings.webSocketCompressionMode, kj::mv(webSocketRequest));
  kj::Maybe<kj::Exception> failure;
  try {
    co_await service.request(parsed, urlText, headers, requestBody, response);
  } catch (...) {
    failure = kj::getCaughtExceptionAsKj();
  }
  KJ_IF_SOME(exception, failure) {
    KJ_IF_SOME(refusal, response.takeRefusal()) {
      // The refusal is the response; its exception is a side effect of it.
      co_return co_await refusal;
    }
    kj::Maybe<kj::HttpService::Response&> unsent;
    if (!response.isSent()) {
      response.closeAfterSend();
      unsent = response;
    }
    co_await handler.handleApplicationError(kj::mv(exception), unsent);
  } else if (!response.isSent()) {
    response.closeAfterSend();
    co_await handler.handleNoResponse(response);
  }
}

kj::Promise<void> HttpDispatcher::connect(::rust::Slice<const uint8_t> host,
    ::rust::Slice<const uint8_t> headerArena,
    ::rust::Slice<const uint32_t> headerLens,
    ::rust::Box<RustIo> tunnel,
    ::rust::Box<ConnectResponder> impl) const {
  auto hostText = str(host);
  auto headers = headersFrom(table, headerArena, headerLens);
  RustIoStream connection(kj::mv(tunnel));
  ConnectResponseImpl response(kj::mv(impl));
  kj::Maybe<kj::Exception> failure;
  try {
    co_await service.connect(hostText, headers, connection, response, {});
  } catch (...) {
    failure = kj::getCaughtExceptionAsKj();
  }
  if (response.isRejected()) {
    // As for a refused WebSocket handshake.
    co_return;
  }
  KJ_IF_SOME(exception, failure) {
    kj::Maybe<kj::Own<ServerResponseImpl>> errorResponse;
    kj::Maybe<kj::HttpService::Response&> unsent;
    if (!response.isAnswered()) {
      unsent = *errorResponse.emplace(response.errorResponse(settings.webSocketCompressionMode));
    }
    co_await handler.handleApplicationError(kj::mv(exception), unsent);
  } else if (!response.isAnswered()) {
    auto errorResponse = response.errorResponse(settings.webSocketCompressionMode);
    co_await handler.handleNoResponse(*errorResponse);
  }
}

kj::Own<HyperHttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<HttpConnector> connector,
    kj::HttpClientSettings& settings) {
  auto impl = new_pooled_client(kj::mv(connector), settings.idleTimeout / kj::MILLISECONDS);
  return kj::heap<HyperHttpClient>(table, kj::mv(impl), settings);
}

kj::Own<kj::HttpClient> newHttpClient(const kj::HttpHeaderTable& table,
    kj::Own<kj::AsyncIoStream> stream,
    kj::HttpClientSettings& settings) {
  return kj::heap<HyperHttpClient>(table, new_single_client(kj::mv(stream)), settings);
}

kj::Own<kj::HttpClient> newHttpClient(
    const kj::HttpHeaderTable& table, kj::AsyncIoStream& stream, kj::HttpClientSettings& settings) {
  return kj::heap<HyperHttpClient>(table,
      new_single_client_over_lent_stream(
          kj::Own<kj::AsyncIoStream>(&stream, kj::NullDisposer::instance)),
      settings);
}

kj::Promise<void> HttpConnection::serve() {
  return hot(impl->serve(*dispatcher));
}

kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::Own<kj::AsyncIoStream> stream) {
  return kj::heap<HttpConnection>(kj::heap<HttpDispatcher>(table, service, settings),
      new_connection(kj::mv(stream), settings.headerTimeout / kj::MILLISECONDS));
}

kj::Own<HttpConnection> newHttpConnection(const kj::HttpHeaderTable& table,
    kj::HttpService& service,
    kj::HttpServerSettings& settings,
    kj::AsyncIoStream& stream) {
  return kj::heap<HttpConnection>(kj::heap<HttpDispatcher>(table, service, settings),
      new_connection_over_lent_stream(
          kj::Own<kj::AsyncIoStream>(&stream, kj::NullDisposer::instance),
          settings.headerTimeout / kj::MILLISECONDS));
}

kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsClient(
    kj::Own<kj::AsyncIoStream> stream, const TlsClientConfig& config, kj::StringPtr hostname) {
  auto name = kj::str(hostname);
  co_return newRustIoStream(co_await wrap_tls_client(kj::mv(stream), config, rustStr(name)));
}

kj::Promise<kj::Own<kj::AsyncIoStream>> wrapTlsServer(
    kj::Own<kj::AsyncIoStream> stream, const TlsServerConfig& config) {
  co_return newRustIoStream(co_await wrap_tls_server(kj::mv(stream), config));
}

}  // namespace workerd::rust::kj_hyper
