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

struct RustIo;
struct RustBody;
struct ServerResponse;
struct ConnectResponder;

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

inline kj::Promise<size_t> kjReadEndTryRead(
    KjStreamReadEnd& end, ::rust::Slice<uint8_t> buffer, size_t minBytes) {
  return end.share->stream->tryRead(buffer.data(), minBytes, buffer.size());
}

// Another end on the same stream, owned by a hangup watch so it may outlive the other ends.
inline kj::Own<KjStreamReadEnd> kjReadEndWhenWriteDisconnected(const KjStreamReadEnd& end) {
  return kj::heap<KjStreamReadEnd>(const_cast<KjStreamReadEnd&>(end).share.addRef());
}

inline kj::Promise<void> kjShareWhenWriteDisconnected(KjStreamReadEnd& end) {
  return end.share->stream->whenWriteDisconnected();
}

inline kj::Promise<void> kjWriteEndWrite(
    KjStreamWriteEnd& end, ::rust::Slice<const uint8_t> buffer) {
  return end.share->stream->write(kj::arrayPtr(buffer.data(), buffer.size()));
}

inline void kjWriteEndShutdownWrite(KjStreamWriteEnd& end) {
  end.share->stream->shutdownWrite();
}

// --- Rust objects as kj interfaces (defined in kj-hyper.c++).

kj::Own<kj::AsyncIoStream> newRustIoStream(::rust::Box<RustIo> io);
kj::Own<kj::AsyncInputStream> newBodyStream(::rust::Box<RustBody> body);
kj::Own<kj::HttpService::Response> newServerResponse(::rust::Box<ServerResponse> response);
kj::Own<kj::HttpService::ConnectResponse> newConnectResponse(
    ::rust::Box<ConnectResponder> response);

kj::Promise<void> serviceRequest(const kj::HttpService& service,
    ::rust::Slice<const uint8_t> method,
    ::rust::Slice<const uint8_t> url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& body,
    kj::HttpService::Response& response);

kj::Promise<void> serviceConnect(const kj::HttpService& service,
    ::rust::Slice<const uint8_t> host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& response);

// --- Dialing for the pooling client.

// Where the pooling client's connections come from: a kj Network (per authority) or a single
// NetworkAddress, so kj's peer filters and TLS networks apply.
class HttpConnector {
 public:
  virtual ~HttpConnector() noexcept(false) = default;
  virtual kj::Promise<kj::Own<kj::AsyncIoStream>> connect(kj::StringPtr authority, bool https) = 0;
};

struct Dialed {
  kj::Maybe<kj::Own<kj::AsyncIoStream>> stream;
};

inline kj::Own<Dialed> newDialed() {
  return kj::heap<Dialed>();
}

inline kj::Promise<void> connectorConnect(const HttpConnector& connector,
    ::rust::Slice<const uint8_t> authority,
    bool https,
    Dialed& out) {
  auto str = kj::str(kj::arrayPtr(authority.data(), authority.size()).asChars());
  out.stream = co_await const_cast<HttpConnector&>(connector).connect(str, https);
}

inline kj::Own<kj::AsyncIoStream> takeDialed(Dialed& dialed) {
  return KJ_ASSERT_NONNULL(kj::mv(dialed.stream));
}

}  // namespace workerd::rust::kj_hyper
