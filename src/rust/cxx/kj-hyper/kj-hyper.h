// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of kj-hyper's bridge (ffi.rs): kj interfaces implemented over Rust objects, and
// the kj plumbing the Rust client borrows. Included by the generated bridge header, so it
// declares the Rust types instead of including it.

#include <workerd/rust/kj/ffi.h>

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>

#include <cstring>

namespace workerd::rust::kj_hyper {

using WebSocketErrorHandler = kj::WebSocketErrorHandler;

struct Head;
struct RustIo;
struct RustBody;
struct ServerResponse;
struct ConnectResponder;
struct TlsStarter;
struct WsCompression;
struct WsOffer;
enum class WebSocketCompression : uint8_t;
enum class Ecdsa : uint8_t;

// --- Headers and methods.

void for_each_header(const kj::HttpHeaders& headers, Head& head);

// The ids a table gives the header names one connection's requests carry, each looked up once
// and then named by its slot here (body.rs's `HeaderIds` keeps the slots by name).
class HeaderIds {
 public:
  explicit HeaderIds(const kj::HttpHeaderTable& table): table(table) {}

  const kj::HttpHeaderTable& getTable() const {
    return table;
  }
  // The slot of the id the table gives `name`; -1 for a name the table lacks.
  int32_t resolve(::rust::Str name);
  kj::HttpHeaderId operator[](uint32_t slot) const {
    return ids[slot];
  }
  size_t size() const {
    return ids.size();
  }

 private:
  const kj::HttpHeaderTable& table;
  kj::Vector<kj::HttpHeaderId> ids;
};

kj::Own<HeaderIds> new_header_ids(const kj::HttpHeaderTable& table);
// Headers that own a copy of the block.
kj::Own<kj::HttpHeaders> headers_from_block(const kj::HttpHeaderTable& table,
    ::rust::Slice<const uint8_t> arena,
    ::rust::Slice<const uint32_t> fields);
// Headers that point into the block, with the slots `ids` resolved.
kj::Own<kj::HttpHeaders> headers_over_block(
    const HeaderIds& ids, ::rust::Slice<const uint8_t> arena, ::rust::Slice<const uint32_t> fields);
// `headers_over_block`, refilling headers made over the same table in place of what they held.
void refill_headers_over_block(kj::HttpHeaders& headers,
    const HeaderIds& ids,
    ::rust::Slice<const uint8_t> arena,
    ::rust::Slice<const uint32_t> fields);

bool ecdsa_verify(Ecdsa algorithm,
    ::rust::Slice<const uint8_t> publicKey,
    ::rust::Slice<const uint8_t> message,
    ::rust::Slice<const uint8_t> signature);

inline bool parse_method(::rust::Slice<const uint8_t> name, kj::HttpMethod& method) {
  // Every method kj knows has a short name, made a C string here without an allocation.
  char text[16];
  if (name.size() >= sizeof(text)) {
    return false;
  }
  memcpy(text, name.data(), name.size());
  text[name.size()] = '\0';
  KJ_IF_SOME(parsed, kj::tryParseHttpMethod(kj::StringPtr(text, name.size()))) {
    method = parsed;
    return true;
  }
  return false;
}

// --- Rust objects behind kj interfaces.

kj::Own<kj::AsyncIoStream> new_rust_io_stream(::rust::Box<RustIo> io);
kj::Own<kj::AsyncInputStream> new_body_stream(::rust::Box<RustBody> body);
kj::Own<kj::HttpService::Response> new_server_response(::rust::Box<ServerResponse> response,
    kj::HttpMethod method,
    const kj::HttpHeaders& headers,
    WebSocketCompression compression,
    kj::Maybe<const kj::WebSocketErrorHandler&> errors);
kj::Own<kj::HttpService::ConnectResponse> new_connect_response(
    ::rust::Box<ConnectResponder> response);
kj::Own<kj::WebSocket> new_client_websocket(kj::Own<kj::AsyncIoStream> stream,
    const WsCompression& compression,
    kj::Maybe<const kj::WebSocketErrorHandler&> errors);

// --- kj's own plumbing, for the client.

kj::Promise<void> pump_websockets(kj::Own<kj::WebSocket> a, kj::Own<kj::WebSocket> b);
kj::Promise<void> pump_tunnel(kj::AsyncIoStream& connection, kj::Own<kj::AsyncIoStream> tunnel);
void tls_starter_set(kj::TlsStarterCallback& starter, ::rust::Box<TlsStarter> start);
kj::Own<kj::TlsStarterCallback> new_tls_starter(::rust::Box<TlsStarter> start);
WsOffer websocket_offer(const kj::HttpHeaders& headers, WebSocketCompression mode);
WsCompression websocket_agreement(
    const WsOffer& offer, const kj::HttpHeaders& response, WebSocketCompression mode);

}  // namespace workerd::rust::kj_hyper
