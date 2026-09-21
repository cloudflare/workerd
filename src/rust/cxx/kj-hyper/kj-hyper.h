// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of kj-hyper's bridge (ffi.rs): seams the Rust code calls. Included by the
// generated bridge header, so it declares Rust types instead of including it.

#include "kj-rs-io/async-io.h"

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/refcount.h>

#if KJ_NO_RTTI
#error "kj-hyper recognizes its own and kj-rs-io's streams by dynamic_cast"
#endif

namespace workerd::rust::kj_hyper {

// kj's types under the bridge's names.
using HttpHeaders = kj::HttpHeaders;
using AsyncIoStream = kj::AsyncIoStream;

struct Head;
struct RustIo;
struct RustBody;
struct ServerResponse;
struct ConnectResponder;
enum class Scheme : uint8_t;

// --- Taking streams apart.

inline bool isTokioStream(const kj::AsyncIoStream& stream) {
  return kj::dynamicDowncastIfAvailable<const kj_rs_io::TokioAsyncIoStream>(stream) != kj::none;
}

inline ::rust::Box<kj_rs_io::TokioStream> releaseTokioStream(kj::Own<kj::AsyncIoStream> stream) {
  return KJ_ASSERT_NONNULL(kj::dynamicDowncastIfAvailable<kj_rs_io::TokioAsyncIoStream>(*stream))
      .release();
}

inline kj::Own<kj::AsyncIoStream> wrapTokioStream(::rust::Box<kj_rs_io::TokioStream> stream) {
  return kj::heap<kj_rs_io::TokioAsyncIoStream>(kj::mv(stream));
}

bool isReleasableRustIo(const kj::AsyncIoStream& stream);
::rust::Box<RustIo> releaseRustIo(kj::Own<kj::AsyncIoStream> stream);

// --- The two directions of a foreign kj stream, each its own object sharing the stream, so
// Rust can drive a read and a write concurrently through exclusive borrows.

struct KjStreamShare final: public kj::Refcounted {
  explicit KjStreamShare(kj::Own<kj::AsyncIoStream> stream): stream(kj::mv(stream)) {}
  kj::Own<kj::AsyncIoStream> stream;
};

struct KjStreamReadEnd {
  kj::Rc<KjStreamShare> share;
};

struct KjStreamWriteEnd {
  kj::Rc<KjStreamShare> share;
};

inline kj::Own<KjStreamReadEnd> kjStreamReadEnd(kj::Own<kj::AsyncIoStream> stream) {
  return kj::heap<KjStreamReadEnd>(kj::rc<KjStreamShare>(kj::mv(stream)));
}

inline kj::Own<KjStreamWriteEnd> kjStreamWriteEnd(KjStreamReadEnd& read) {
  return kj::heap<KjStreamWriteEnd>(read.share.addRef());
}

// Another end on the same stream, for an observation that may outlive the other ends.
inline kj::Own<KjStreamReadEnd> kjReadEndShare(KjStreamReadEnd& end) {
  return kj::heap<KjStreamReadEnd>(end.share.addRef());
}

inline kj::Promise<size_t> kjReadEndTryRead(
    KjStreamReadEnd& end, ::rust::Slice<uint8_t> buffer, size_t minBytes) {
  return end.share->stream->tryRead(buffer.data(), minBytes, buffer.size());
}

inline kj::Promise<void> kjReadEndWhenWriteDisconnected(KjStreamReadEnd& end) {
  return end.share->stream->whenWriteDisconnected();
}

inline kj::Promise<void> kjWriteEndWrite(
    KjStreamWriteEnd& end, ::rust::Slice<const uint8_t> buffer) {
  return end.share->stream->write(kj::arrayPtr(buffer.data(), buffer.size()));
}

inline void kjWriteEndShutdownWrite(KjStreamWriteEnd& end) {
  end.share->stream->shutdownWrite();
}

// Every header, in kj's order, appended to `head`.
void forEachHeader(const kj::HttpHeaders& headers, Head& head);

// --- Serving.

// A connection's per-request context: kj's HttpServer around the application. Failures and
// missing responses go to the settings' error handler, which answers through a response that
// closes the connection after it is sent; refused WebSocket handshakes are answered as kj's server
// answers them. Requests on one connection share it, and may be in flight at once.
class HttpDispatcher {
 public:
  HttpDispatcher(
      const kj::HttpHeaderTable& table, kj::HttpService& service, kj::HttpServerSettings& settings);

  kj::Promise<void> request(::rust::Slice<const uint8_t> method,
      ::rust::Slice<const uint8_t> url,
      ::rust::Slice<const uint8_t> headerArena,
      ::rust::Slice<const uint32_t> headerLens,
      ::rust::Box<RustBody> body,
      ::rust::Box<ServerResponse> response) const;

  kj::Promise<void> connect(::rust::Slice<const uint8_t> host,
      ::rust::Slice<const uint8_t> headerArena,
      ::rust::Slice<const uint32_t> headerLens,
      ::rust::Box<RustIo> tunnel,
      ::rust::Box<ConnectResponder> response) const;

 private:
  const kj::HttpHeaderTable& table;
  kj::HttpService& service;
  kj::HttpServerSettings& settings;
  kj::HttpServerErrorHandler defaultHandler;
  kj::HttpServerErrorHandler& handler;
};

inline kj::Promise<void> dispatchRequest(const HttpDispatcher& dispatcher,
    ::rust::Slice<const uint8_t> method,
    ::rust::Slice<const uint8_t> url,
    ::rust::Slice<const uint8_t> headerArena,
    ::rust::Slice<const uint32_t> headerLens,
    ::rust::Box<RustBody> body,
    ::rust::Box<ServerResponse> response) {
  return dispatcher.request(method, url, headerArena, headerLens, kj::mv(body), kj::mv(response));
}

inline kj::Promise<void> dispatchConnect(const HttpDispatcher& dispatcher,
    ::rust::Slice<const uint8_t> host,
    ::rust::Slice<const uint8_t> headerArena,
    ::rust::Slice<const uint32_t> headerLens,
    ::rust::Box<RustIo> tunnel,
    ::rust::Box<ConnectResponder> response) {
  return dispatcher.connect(host, headerArena, headerLens, kj::mv(tunnel), kj::mv(response));
}

// --- Dialing for the pooling client.

// Where the pooling client's connections come from: a kj Network (per authority) or a single
// NetworkAddress, so kj's peer filters and TLS networks apply. Connectors are shared: many dials
// may be in flight on one.
class HttpConnector {
 public:
  virtual ~HttpConnector() noexcept(false) = default;
  virtual kj::Promise<kj::Own<kj::AsyncIoStream>> connect(
      kj::StringPtr authority, Scheme scheme) const = 0;
};

inline kj::Promise<kj::Own<kj::AsyncIoStream>> connectorConnect(
    const HttpConnector& connector, ::rust::Slice<const uint8_t> authority, Scheme scheme) {
  auto name = kj::str(kj::arrayPtr(authority.data(), authority.size()).asChars());
  co_return co_await connector.connect(name, scheme);
}

}  // namespace workerd::rust::kj_hyper
