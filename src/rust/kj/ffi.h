// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj-rs/convert.h>
#include <rust/cxx.h>

#include <kj/compat/http.h>

namespace kj::rust {

struct HttpConnectSettings;

// --- Async IO

using AsyncInputStream = kj::AsyncInputStream;
using AsyncOutputStream = kj::AsyncOutputStream;
using AsyncIoStream = kj::AsyncIoStream;

inline kj::Promise<void> async_output_stream_write(
    AsyncOutputStream& stream, ::rust::Slice<const kj::byte> buffer) {
  return stream.write(kj::from<kj_rs::Rust>(buffer));
}

inline kj::Promise<void> async_output_stream_when_write_disconnected(AsyncOutputStream& stream) {
  return stream.whenWriteDisconnected();
}

// --- kj::HttpHeaders ffi

using BuiltinIndicesEnum = kj::HttpHeaders::BuiltinIndicesEnum;
using HttpHeaderTable = kj::HttpHeaderTable;
using HttpHeaders = kj::HttpHeaders;
using HttpHeaderId = kj::HttpHeaderId;

inline kj::Own<kj::HttpHeaders> new_http_headers(const HttpHeaderTable& table) {
  // There is no C++ stack frame to hold the new instance, so we heap allocate it for Rust.
  return kj::heap<kj::HttpHeaders>(table);
}

inline kj::Own<kj::HttpHeaders> clone_shallow(const HttpHeaders& headers) {
  // there is no c++ stack frame to hold the new instance,
  // so sadly we have to heap allocate it.
  return kj::heap(headers.cloneShallow());
}

inline kj::HttpHeaderId toHeaderId(BuiltinIndicesEnum id) {
  switch (id) {
    case kj::HttpHeaders::BuiltinIndicesEnum::CONNECTION:
      return kj::HttpHeaderId::CONNECTION;
    case kj::HttpHeaders::BuiltinIndicesEnum::KEEP_ALIVE:
      return kj::HttpHeaderId::KEEP_ALIVE;
    case kj::HttpHeaders::BuiltinIndicesEnum::TE:
      return kj::HttpHeaderId::TE;
    case kj::HttpHeaders::BuiltinIndicesEnum::TRAILER:
      return kj::HttpHeaderId::TRAILER;
    case kj::HttpHeaders::BuiltinIndicesEnum::UPGRADE:
      return kj::HttpHeaderId::UPGRADE;
    case kj::HttpHeaders::BuiltinIndicesEnum::CONTENT_LENGTH:
      return kj::HttpHeaderId::CONTENT_LENGTH;
    case kj::HttpHeaders::BuiltinIndicesEnum::TRANSFER_ENCODING:
      return kj::HttpHeaderId::TRANSFER_ENCODING;
    case kj::HttpHeaders::BuiltinIndicesEnum::SEC_WEBSOCKET_KEY:
      return kj::HttpHeaderId::SEC_WEBSOCKET_KEY;
    case kj::HttpHeaders::BuiltinIndicesEnum::SEC_WEBSOCKET_VERSION:
      return kj::HttpHeaderId::SEC_WEBSOCKET_VERSION;
    case kj::HttpHeaders::BuiltinIndicesEnum::SEC_WEBSOCKET_ACCEPT:
      return kj::HttpHeaderId::SEC_WEBSOCKET_ACCEPT;
    case kj::HttpHeaders::BuiltinIndicesEnum::SEC_WEBSOCKET_EXTENSIONS:
      return kj::HttpHeaderId::SEC_WEBSOCKET_EXTENSIONS;
    case kj::HttpHeaders::BuiltinIndicesEnum::HOST:
      return kj::HttpHeaderId::HOST;
    case kj::HttpHeaders::BuiltinIndicesEnum::DATE:
      return kj::HttpHeaderId::DATE;
    case kj::HttpHeaders::BuiltinIndicesEnum::LOCATION:
      return kj::HttpHeaderId::LOCATION;
    case kj::HttpHeaders::BuiltinIndicesEnum::CONTENT_TYPE:
      return kj::HttpHeaderId::CONTENT_TYPE;
    case kj::HttpHeaders::BuiltinIndicesEnum::RANGE:
      return kj::HttpHeaderId::RANGE;
    case kj::HttpHeaders::BuiltinIndicesEnum::CONTENT_RANGE:
      return kj::HttpHeaderId::CONTENT_RANGE;
      break;
  }
}

inline void set_header(HttpHeaders& headers, BuiltinIndicesEnum id, ::rust::Str value) {
  headers.set(toHeaderId(id), kj::str(value));
}

inline kj::Maybe<::rust::Slice<const kj::byte>> get_header(
    const HttpHeaders& headers, BuiltinIndicesEnum id) {
  auto header = headers.get(toHeaderId(id));
  return header.map([](auto header) { return header.asBytes().template as<kj_rs::Rust>(); });
}

inline kj::Maybe<::rust::Slice<const kj::byte>> get_header_by_id(
    const HttpHeaders& headers, const HttpHeaderId& id) {
  auto header = headers.get(id);
  return header.map([](auto header) { return header.asBytes().template as<kj_rs::Rust>(); });
}

// --- kj::HttpService ffi
using AsyncInputStream = kj::AsyncInputStream;
using AsyncIoStream = kj::AsyncIoStream;
using ConnectResponse = kj::HttpService::ConnectResponse;
using HttpMethod = kj::HttpMethod;
using HttpService = kj::HttpService;
using HttpServiceResponse = kj::HttpService::Response;
using TlsStarterCallback = kj::TlsStarterCallback;

inline kj::Own<AsyncOutputStream> response_send(HttpServiceResponse& response,
    uint32_t statusCode,
    ::rust::Str statusText,
    const HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  return response.send(statusCode, kj::str(statusText), headers, expectedBodySize);
}

inline void connect_response_accept(ConnectResponse& response,
    uint32_t statusCode,
    ::rust::Str statusText,
    const HttpHeaders& headers) {
  response.accept(statusCode, kj::str(statusText), headers);
}

inline kj::Own<AsyncOutputStream> connect_response_reject(ConnectResponse& response,
    uint32_t statusCode,
    ::rust::Str statusText,
    const HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  return response.reject(statusCode, kj::str(statusText), headers, expectedBodySize);
}

inline kj::Promise<void> request(HttpService& service,
    HttpMethod method,
    ::rust::Slice<const kj::byte> url,
    const HttpHeaders& headers,
    AsyncInputStream& request_body,
    HttpServiceResponse& response) {
  auto strUrl = kj::str(kj::from<kj_rs::Rust>(url).asChars());
  co_await service.request(method, strUrl, headers, request_body, response);
}

kj::Promise<void> connect(HttpService& service,
    ::rust::Slice<const kj::byte> host,
    const HttpHeaders& headers,
    AsyncIoStream& connection,
    ConnectResponse& response,
    HttpConnectSettings settings);

}  // namespace kj::rust
