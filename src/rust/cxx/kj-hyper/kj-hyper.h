// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of kj-hyper's bridge (ffi.rs): kj interfaces implemented over Rust objects, and
// the kj plumbing the Rust client borrows. Included by the generated bridge header, so it
// declares the Rust types instead of including it.

#include "kj-rs-io/async-io.h"

#include <workerd/rust/kj/ffi.h>

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>

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

// --- Headers and methods.

void for_each_header(const kj::HttpHeaders& headers, Head& head);
kj::Own<kj::HttpHeaders> headers_from_block(const kj::HttpHeaderTable& table,
    ::rust::Slice<const uint8_t> arena,
    ::rust::Slice<const uint32_t> lens);

inline bool parse_method(::rust::Slice<const uint8_t> name, kj::HttpMethod& method) {
  auto text = kj::str(kj::arrayPtr(name.data(), name.size()).asChars());
  KJ_IF_SOME(parsed, kj::tryParseHttpMethod(text)) {
    method = parsed;
    return true;
  }
  return false;
}

// --- Rust objects behind kj interfaces.

inline kj::Own<kj::AsyncIoStream> wrap_tokio_stream(::rust::Box<kj_rs_io::TokioStream> stream) {
  return kj::heap<kj_rs_io::TokioAsyncIoStream>(kj::mv(stream));
}

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
WsOffer websocket_offer(const kj::HttpHeaders& headers, WebSocketCompression mode);
WsCompression websocket_agreement(
    const WsOffer& offer, const kj::HttpHeaders& response, WebSocketCompression mode);

}  // namespace workerd::rust::kj_hyper
