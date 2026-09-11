// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj-rs/convert.h>
#include <rust/cxx.h>

#include <kj/compat/http.h>

namespace kj::rust {

struct HttpConnectSettings;
struct HttpHeaderEntry;

// --- Async IO

using AsyncInputStream = kj::AsyncInputStream;
using AsyncOutputStream = kj::AsyncOutputStream;
using AsyncIoStream = kj::AsyncIoStream;

inline kj::Promise<size_t> async_input_stream_try_read(
    AsyncInputStream& stream, ::rust::Slice<kj::byte> buffer, size_t minBytes) {
  return stream.tryRead(buffer.data(), minBytes, buffer.size());
}

inline kj::Maybe<uint64_t> async_input_stream_try_get_length(AsyncInputStream& stream) {
  return stream.tryGetLength();
}

inline kj::Promise<void> async_output_stream_write(
    AsyncOutputStream& stream, ::rust::Slice<const kj::byte> buffer) {
  return stream.write(kj::from<kj_rs::Rust>(buffer));
}

inline kj::Promise<void> async_output_stream_when_write_disconnected(AsyncOutputStream& stream) {
  return stream.whenWriteDisconnected();
}

// AsyncIoStream variants: a two-way stream must be readable and writable *concurrently*
// (e.g. a tunnel pump reads and writes the same stream from two futures), so
// these take the stream itself rather than requiring the caller to split it into its
// AsyncInputStream/AsyncOutputStream bases (two simultaneous Pin<&mut> views of one
// object are not expressible through the bridge).
inline kj::Promise<size_t> async_io_stream_try_read(
    AsyncIoStream& stream, ::rust::Slice<kj::byte> buffer, size_t minBytes) {
  return stream.tryRead(buffer.data(), minBytes, buffer.size());
}

inline kj::Promise<void> async_io_stream_write(
    AsyncIoStream& stream, ::rust::Slice<const kj::byte> buffer) {
  return stream.write(kj::from<kj_rs::Rust>(buffer));
}

inline void async_io_stream_shutdown_write(AsyncIoStream& stream) {
  stream.shutdownWrite();
}

// --- kj::HttpHeaders ffi

using BuiltinIndicesEnum = kj::HttpHeaders::BuiltinIndicesEnum;
using HttpHeaderTable = kj::HttpHeaderTable;
using HttpHeaders = kj::HttpHeaders;
using HttpHeaderId = kj::HttpHeaderId;

// Normalize a kj::Own<T> so it can safely cross the FFI boundary as a kj-rs KjOwn.
//
// KjOwn erases the pointee type and disposes through kj::Own<void> with the *stored* pointer.
// That is only safe if the disposer ignores the pointer or the stored pointer equals the
// most-derived object address; a concrete type using multiple inheritance (e.g. kj::NullStream,
// where AsyncOutputStream is a non-first base) would otherwise pass a shifted pointer to
// HeapDisposer and corrupt the heap. attach() re-homes ownership in a DisposableOwnedBundle
// whose disposeImpl() ignores the pointer argument entirely, making the erasure safe.
template <typename T>
inline kj::Own<T> normalizeForRust(kj::Own<T> own) {
  return own.attach();
}

inline kj::Own<kj::HttpHeaders> new_http_headers(const HttpHeaderTable& table) {
  // There is no C++ stack frame to hold the new instance, so we heap allocate it for Rust.
  return kj::heap<kj::HttpHeaders>(table);
}

inline kj::Own<kj::HttpHeaders> clone_shallow(const HttpHeaders& headers) {
  // there is no c++ stack frame to hold the new instance,
  // so sadly we have to heap allocate it.
  return kj::heap(headers.cloneShallow());
}

inline void clear_headers(HttpHeaders& headers) {
  headers.clear();
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

// Case-insensitive lookup of a header by name. Note that kj::HttpHeaders::forEach only visits
// headers that have been explicitly set, so a header registered in the HttpHeaderTable but never
// set will not be found (returns none, matching get_header/get_header_by_id).
inline kj::Maybe<::rust::Slice<const kj::byte>> get_header_by_name(
    const HttpHeaders& headers, ::rust::Str requestedName) {
  auto requested = kj::ArrayPtr<const char>(requestedName.data(), requestedName.size());
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  kj::Maybe<kj::StringPtr> result;
  headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
    // kj::HttpHeaders::get() (used by get_header/get_header_by_id) returns the first set value, so
    // keep the first match here too and ignore any subsequent duplicate header names.
    if (result != kj::none) return;
    if (name.size() != requested.size()) return;
    for (size_t i = 0; i < name.size(); ++i) {
      if (lower(name[i]) != lower(requested[i])) return;
    }
    result = value;
  });
  return result.map([](auto header) { return header.asBytes().template as<kj_rs::Rust>(); });
}

// Returns every set header as a (name, value) pair via forEach. Defined out-of-line in ffi.c++
// because HttpHeaderEntry is generated into the cxx bridge header, which ffi.h cannot include
// without creating an include cycle.
::rust::Vec<HttpHeaderEntry> get_all_headers(const HttpHeaders& headers);

// Appends a header by name, taking ownership of copies of both name and value. kj-http rejects
// values containing '\0', '\r' or '\n' with an exception (surfaced to Rust as Result::Err); any
// other bytes (including non-UTF-8 obs-text) pass through unchanged.
inline void add_header(
    HttpHeaders& headers, ::rust::Str name, ::rust::Slice<const kj::byte> value) {
  headers.add(kj::str(name), kj::str(kj::from<kj_rs::Rust>(value).asChars()));
}

// Batch-appends a whole request's headers borrowing from a single arena buffer, replacing the
// two per-header kj::String allocations of add_header() (name + value) with ONE allocation for
// the entire header block. This mirrors what kj::HttpServer does natively: it parses request
// headers in place and stores borrowed StringPtrs into the connection read buffer rather than
// heap-allocating an owned String per field. Here the borrowed-from buffer is a kj-owned copy of
// the Rust arena, attached to `headers` via takeOwnership so the StringPtrs stay valid for the
// life of the HttpHeaders (the same lifetime contract kj::HttpServer relies on).
//
// `arena` is packed by the Rust caller as, for each header in add() order: name bytes, a '\0',
// value bytes, a '\0'. `lens` is the flat [name_len, value_len, name_len, value_len, ...]
// sequence (two u32 per header), so each field's span is [off, off+len) with a readable NUL at
// off+len -- exactly the [ptr, len+1) content span kj::StringPtr requires (asArray/size derive
// from content.size()-1, and addNoCheck calls name.cStr()).
//
// Validation and ordering are identical to repeated add_header()/add(String,String) calls: each
// field goes through addPtrPtr(), which runs requireValidHeaderName/requireValidHeaderValue and
// then addNoCheck (indexed storage + duplicate-concatenation). The first invalid header throws,
// surfaced to Rust as Result::Err, exactly as the per-call path did; any fields added before it
// are discarded with the HttpHeaders by the caller (which sends a 400), matching the old loop.
inline void add_headers_arena(
    HttpHeaders& headers, ::rust::Slice<const kj::byte> arena, ::rust::Slice<const uint32_t> lens) {
  // One heap allocation: copy the packed bytes into kj-owned storage whose address is stable for
  // the life of `headers`. kj::Array move (in takeOwnership) transfers the pointer without
  // reallocating, so `base` remains valid for the borrowed StringPtrs added below.
  auto owned = kj::heapArray<char>(reinterpret_cast<const char*>(arena.data()), arena.size());
  const char* base = owned.begin();
  headers.takeOwnership(kj::mv(owned));

  size_t pos = 0;
  for (size_t i = 0; i + 1 < lens.size(); i += 2) {
    size_t nameLen = lens[i];
    size_t valueLen = lens[i + 1];
    // The public ptr+size StringPtr ctor: it forms content = [ptr, len+1) and (in debug)
    // asserts ptr[len] == '\0' -- exactly the arena's NUL, which also gives addNoCheck's
    // name.cStr() a valid C string.
    kj::StringPtr name(base + pos, nameLen);
    pos += nameLen + 1;
    kj::StringPtr value(base + pos, valueLen);
    pos += valueLen + 1;
    headers.addPtrPtr(name, value);
  }
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
  // normalizeForRust: send() may return a stream whose concrete type uses multiple inheritance
  // (e.g. kj::NullStream for HEAD responses in kj's HttpClientAdapter), which KjOwn cannot
  // dispose safely without normalization.
  return normalizeForRust(
      response.send(statusCode, kj::str(statusText), headers, expectedBodySize));
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
  // normalizeForRust: see response_send.
  return normalizeForRust(
      response.reject(statusCode, kj::str(statusText), headers, expectedBodySize));
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
