// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// C++ glue for the hyper-backed HTTP layer (see server.rs / client.rs / ws.rs): factories that
// wrap Rust-side objects as KJ interfaces — inbound request bodies and responses for the C++
// kj::HttpService, plus WebSocket sessions, CONNECT tunnel streams, and ConnectResponse objects
// used by both the inbound server and the outbound client. Declarations only; definitions live
// in hyper-server-ffi.c++ (which can include the generated bridge header). This header is
// included by the generated cxx bridge, so it must not include ffi.rs.h itself.

#include <rust/cxx.h>

#include <kj/compat/http.h>

namespace workerd::rust::kj_hyper {

// Generated cxx bridge types (see lib.rs); forward-declared to avoid an include cycle.
struct HyperRequestBody;
struct ServeRequestBody;
struct HyperResponseSender;
struct HyperResponseBodySink;
struct HyperConnectResponder;
struct WsSession;
struct HyperTunnel;

kj::Own<kj::AsyncInputStream> new_hyper_request_body_stream(::rust::Box<HyperRequestBody> body);
kj::Own<kj::AsyncInputStream> new_serve_request_body_stream(::rust::Box<ServeRequestBody> body);
kj::Own<kj::HttpService::Response> new_hyper_response(::rust::Box<HyperResponseSender> sender);
// The response-body kj::AsyncOutputStream on its own, for Response implementations living
// outside this crate whose send() produces a HyperResponseBodySink through the shared
// translate module.
kj::Own<kj::AsyncOutputStream> new_hyper_response_body_stream(
    ::rust::Box<HyperResponseBodySink> sink);
kj::Own<kj::WebSocket> new_rust_websocket(::rust::Box<WsSession> session);
kj::Own<kj::AsyncIoStream> new_tunnel_stream(::rust::Box<HyperTunnel> tunnel);
kj::Own<kj::HttpService::ConnectResponse> new_hyper_connect_response(
    ::rust::Box<HyperConnectResponder> responder);

// Drive the C++ kj::HttpService::request()/connect() on a SHARED service reference. kj services
// are shared-reentrant — kj::HttpServer dispatches concurrent request()/connect() calls through
// one kj::HttpService& — and the inbound hyper server multiplexes many keep-alive requests onto
// the one workerd HttpService, so the Rust bridge (ffi.rs) holds the service as a shared
// `&HttpService` and these shims perform the (non-const) kj call on it. That way two multiplexed
// requests never alias an exclusive Rust `&mut` (the shared-receiver soundness rule for
// reentrant C++ objects called from Rust). `service` is const only at the FFI ABI (it reflects
// Rust's shared borrow); the pointee is a live, mutable kj service, so recovering the non-const
// reference for the virtual call is well-defined. Both are coroutines so a service that throws
// *synchronously* (e.g. the default connect(), which KJ_UNIMPLEMENTEDs) yields a rejected promise
// instead of an exception escaping the promise-returning FFI shim.
kj::Promise<void> hyper_service_request(const kj::HttpService& service,
    kj::HttpMethod method,
    ::rust::Slice<const kj::byte> url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response);

kj::Promise<void> hyper_service_connect(const kj::HttpService& service,
    ::rust::Slice<const kj::byte> host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& response);

}  // namespace workerd::rust::kj_hyper
