// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "kj-hyper.h"

#include <kj-hyper/ffi.rs.h>

#include <kj/debug.h>

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

kj::Array<kj::byte> join(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  size_t size = 0;
  for (auto& piece: pieces) size += piece.size();
  auto joined = kj::heapArray<kj::byte>(size);
  auto pos = joined.begin();
  for (auto& piece: pieces) {
    if (piece.size() > 0) memcpy(pos, piece.begin(), piece.size());
    pos += piece.size();
  }
  return joined;
}

// A Rust transport (an upgraded connection) as a kj stream. `whenWriteDisconnected()` never
// resolves (kj's own choice for streams without a hangup signal): the transport has no way to
// observe its peer short of reading it.
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
    auto joined = join(pieces);
    auto promise = io->write(bytes(joined));
    return hot(kj::mv(promise)).attach(kj::mv(joined));
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return kj::NEVER_DONE;
  }
  void shutdownWrite() override {
    shutdownTask = hot(io->shutdown_write()).catch_([](kj::Exception&& e) { KJ_LOG(ERROR, e); });
  }
  void abortRead() override {
    io->abort_read();
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

kj::Maybe<kj::CompressionParameters> fromRust(const WsCompression& compression) {
  if (!compression.enabled) return kj::none;
  kj::CompressionParameters p;
  p.outboundNoContextTakeover = compression.outbound_no_context_takeover;
  p.inboundNoContextTakeover = compression.inbound_no_context_takeover;
  if (compression.outbound_max_window_bits != 0) {
    p.outboundMaxWindowBits = compression.outbound_max_window_bits;
  }
  if (compression.inbound_max_window_bits != 0) {
    p.inboundMaxWindowBits = compression.inbound_max_window_bits;
  }
  return p;
}

// kj's handlers are stateless policy objects; the interface lacks a const qualifier, which is
// all the const_cast removes. The Rust settings holding the handler are shared, hence const.
kj::Maybe<kj::WebSocketErrorHandler&> mutableHandler(
    kj::Maybe<const kj::WebSocketErrorHandler&> errors) {
  return errors.map([](const kj::WebSocketErrorHandler& handler) -> kj::WebSocketErrorHandler& {
    return const_cast<kj::WebSocketErrorHandler&>(handler);
  });
}

// kj masks a client's frames with this; the bytes come from Rust's CSPRNG.
class RustEntropySource final: public kj::EntropySource {
 public:
  void generate(kj::ArrayPtr<kj::byte> buffer) override {
    fill_random(::rust::Slice<uint8_t>(buffer.begin(), buffer.size()));
  }
};

// Stateless, so one instance serves every WebSocket.
kj::EntropySource& entropySource() {
  static RustEntropySource source;
  return source;
}

// Why a WebSocket handshake must be refused (RFC 6455 4.2.1-4.2.2), with kj's messages.
struct Refusal {
  kj::uint status;
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

// A request's WebSocket handshake, as the request's headers described it.
struct WebSocketRequest {
  kj::Maybe<Refusal> refusal;
  // The client's Sec-WebSocket-Extensions offer.
  kj::Maybe<kj::String> offers;
};

class ServerResponseImpl final: public kj::HttpService::Response {
 public:
  ServerResponseImpl(::rust::Box<ServerResponse> impl,
      kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors,
      WebSocketCompression compressionMode,
      kj::Maybe<WebSocketRequest> webSocketRequest)
      : impl(kj::mv(impl)),
        webSocketErrors(webSocketErrors),
        compressionMode(compressionMode),
        webSocketRequest(kj::mv(webSocketRequest)) {}

  kj::Own<kj::AsyncOutputStream> send(kj::uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    auto sink = impl->send(statusCode, bytes(statusText), headers, kj::mv(expectedBodySize));
    return kj::heap<BodySinkStream>(kj::mv(sink));
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    auto& request = KJ_REQUIRE_NONNULL(webSocketRequest,
        "can't call acceptWebSocket() if the request headers didn't have Upgrade: WebSocket");
    KJ_IF_SOME(refused, request.refusal) {
      // As kj's HttpServer: answers, closes the connection, and fails the service's call.
      impl->send_error(
          refused.status, bytes(refused.statusText), bytes(kj::str("ERROR: ", refused.message)));
      kj::throwFatalException(
          KJ_EXCEPTION(FAILED, "received bad WebSocket handshake", refused.message));
    }
    // permessage-deflate, negotiated as kj's HttpServer does in each compression mode.
    kj::Maybe<kj::CompressionParameters> agreed;
    KJ_IF_SOME(offers, request.offers) {
      switch (compressionMode) {
        case WebSocketCompression::AUTOMATIC:
          agreed = kj::_::tryParseExtensionOffers(offers);
          break;
        case WebSocketCompression::MANUAL:
          KJ_IF_SOME(value, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
            KJ_IF_SOME(manualConfig, kj::_::tryParseExtensionOffers(value)) {
              agreed = kj::_::tryParseAllExtensionOffers(offers, manualConfig);
            }
          }
          break;
        case WebSocketCompression::NONE:
          break;
      }
    }
    kj::String agreement;
    KJ_IF_SOME(p, agreed) {
      agreement = kj::_::generateExtensionResponse(p);
    }
    auto io =
        impl->accept_websocket(headers, bytes(agreement == nullptr ? ""_kj : agreement.asPtr()));
    // A server's frames are unmasked (RFC 6455 section 5.1), hence no entropy source.
    return kj::newWebSocket(
        kj::heap<RustIoStream>(kj::mv(io)), kj::none, kj::mv(agreed), webSocketErrors);
  }

 private:
  ::rust::Box<ServerResponse> impl;
  kj::Maybe<kj::WebSocketErrorHandler&> webSocketErrors;
  WebSocketCompression compressionMode;
  kj::Maybe<WebSocketRequest> webSocketRequest;
};

class ConnectResponseImpl final: public kj::HttpService::ConnectResponse {
 public:
  explicit ConnectResponseImpl(::rust::Box<ConnectResponder> impl): impl(kj::mv(impl)) {}

  void accept(
      kj::uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_REQUIRE(statusCode >= 200 && statusCode < 300, "the statusCode must be 2xx for accept");
    impl->accept(statusCode, bytes(statusText), headers);
  }
  kj::Own<kj::AsyncOutputStream> reject(kj::uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_REQUIRE(statusCode < 200 || statusCode >= 300, "the statusCode must not be 2xx for reject.");
    auto sink = impl->reject(statusCode, bytes(statusText), headers, kj::mv(expectedBodySize));
    return kj::heap<BodySinkStream>(kj::mv(sink));
  }

 private:
  ::rust::Box<ConnectResponder> impl;
};

}  // namespace

void for_each_header(const kj::HttpHeaders& headers, Head& head) {
  headers.forEach(
      [&](kj::StringPtr name, kj::StringPtr value) { head.append(bytes(name), bytes(value)); });
}

kj::Own<kj::HttpHeaders> headers_from_block(const kj::HttpHeaderTable& table,
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
  auto headers = kj::heap<kj::HttpHeaders>(table);
  for (size_t i = 0; i < lens.size(); i += 2) {
    auto name = next(lens[i]);
    auto value = next(lens[i + 1]);
    headers->addPtrPtr(name, value);
  }
  headers->takeOwnership(kj::mv(owned));
  return headers;
}

kj::Own<kj::AsyncIoStream> new_rust_io_stream(::rust::Box<RustIo> io) {
  return kj::heap<RustIoStream>(kj::mv(io));
}

kj::Own<kj::AsyncInputStream> new_body_stream(::rust::Box<RustBody> body) {
  return kj::heap<BodyStream>(kj::mv(body));
}

kj::Own<kj::HttpService::Response> new_server_response(::rust::Box<ServerResponse> response,
    kj::HttpMethod method,
    const kj::HttpHeaders& headers,
    WebSocketCompression compression,
    kj::Maybe<const kj::WebSocketErrorHandler&> errors) {
  kj::Maybe<WebSocketRequest> webSocketRequest;
  if (headers.isWebSocket()) {
    webSocketRequest = WebSocketRequest{
      .refusal = websocketRefusal(method, headers),
      .offers =
          headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS).map([](kj::StringPtr offers) {
      return kj::str(offers);
    }),
    };
  }
  return kj::heap<ServerResponseImpl>(
      kj::mv(response), mutableHandler(errors), compression, kj::mv(webSocketRequest));
}

kj::Own<kj::HttpService::ConnectResponse> new_connect_response(
    ::rust::Box<ConnectResponder> response) {
  return kj::heap<ConnectResponseImpl>(kj::mv(response));
}

kj::Own<kj::WebSocket> new_client_websocket(kj::Own<kj::AsyncIoStream> stream,
    const WsCompression& compression,
    kj::Maybe<const kj::WebSocketErrorHandler&> errors) {
  return kj::newWebSocket(
      kj::mv(stream), entropySource(), fromRust(compression), mutableHandler(errors));
}

kj::Promise<void> pump_websockets(kj::Own<kj::WebSocket> a, kj::Own<kj::WebSocket> b) {
  auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
  promises.add(a->pumpTo(*b));
  promises.add(b->pumpTo(*a));
  return kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(a), kj::mv(b));
}

kj::Promise<void> pump_tunnel(kj::AsyncIoStream& connection, kj::Own<kj::AsyncIoStream> tunnel) {
  auto promises = kj::heapArrayBuilder<kj::Promise<void>>(2);
  promises.add(
      connection.pumpTo(*tunnel).then([&tunnel = *tunnel](uint64_t) { tunnel.shutdownWrite(); }));
  promises.add(
      tunnel->pumpTo(connection).then([&connection](uint64_t) { connection.shutdownWrite(); }));
  return kj::joinPromisesFailFast(promises.finish()).attach(kj::mv(tunnel));
}

void tls_starter_set(kj::TlsStarterCallback& starter, ::rust::Box<TlsStarter> start) {
  kj::Function<kj::Promise<void>(kj::StringPtr)> callback =
      [start = kj::mv(start)](kj::StringPtr expectedServerHostname) mutable -> kj::Promise<void> {
    // The name may be dropped once this returns (kj's contract), so the future gets a copy.
    auto host = kj::str(expectedServerHostname);
    auto promise = start->start(bytes(host.asPtr()));
    return hot(kj::mv(promise)).attach(kj::mv(host));
  };
  starter = kj::mv(callback);
}

// permessage-deflate, offered as kj's HttpClient offers it in each compression mode.
WsOffer websocket_offer(const kj::HttpHeaders& headers, WebSocketCompression mode) {
  kj::Vector<kj::CompressionParameters> extensions;
  switch (mode) {
    case WebSocketCompression::MANUAL:
      KJ_IF_SOME(value, headers.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
        extensions = kj::_::findValidExtensionOffers(value);
      }
      break;
    case WebSocketCompression::AUTOMATIC:
      extensions.add(kj::CompressionParameters());
      break;
    case WebSocketCompression::NONE:
      break;
  }
  WsOffer offer{};
  if (!extensions.empty()) {
    offer.has_offer = true;
    offer.offer = toRust(extensions.front());
    offer.extensions = ::rust::String(kj::_::generateExtensionRequest(extensions.asPtr()).cStr());
  }
  return offer;
}

WsCompression websocket_agreement(
    const WsOffer& offer, const kj::HttpHeaders& response, WebSocketCompression mode) {
  if (mode == WebSocketCompression::NONE) return toRust(kj::none);
  KJ_IF_SOME(agreement, response.get(kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS)) {
    kj::Maybe<kj::CompressionParameters> clientOffer;
    if (offer.has_offer) clientOffer = fromRust(offer.offer);
    KJ_SWITCH_ONEOF(kj::_::tryParseExtensionAgreement(clientOffer, agreement)) {
      KJ_CASE_ONEOF(exception, kj::Exception) {
        kj::throwFatalException(kj::mv(exception));
      }
      KJ_CASE_ONEOF(parameters, kj::CompressionParameters) {
        return toRust(parameters);
      }
    }
    KJ_UNREACHABLE;
  }
  return toRust(kj::none);
}

}  // namespace workerd::rust::kj_hyper
