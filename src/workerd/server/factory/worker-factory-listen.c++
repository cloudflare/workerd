// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The listeners' shims: the Rust rewriter's edits applied to kj headers, a response whose
// headers are edited on the way out, the `ConnectResponse` of a raw TCP socket, and a UDP flow as
// a `DatagramChannel` for `connect()` events.

#include "worker-factory-impl.h"

#include <workerd/api/sockets.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/websocket-error-handler.h>

#include <kj/compat/http.h>

namespace workerd::server {

namespace {

// A `HeaderEdit` with its header looked up in the table.
struct Edit {
  kj::HttpHeaderId id;
  kj::Maybe<kj::String> value;
};

kj::Array<Edit> resolveEdits(
    const kj::HttpHeaderTable& table, ::rust::Slice<const HeaderEdit> edits) {
  return KJ_MAP(edit, edits) -> Edit {
    auto name = kj::str(edit.name);
    return Edit{
      .id = KJ_REQUIRE_NONNULL(table.stringToId(name), "header is not in the header table", name),
      .value = edit.value.map([](const ::rust::String& value) { return kj::str(value); }),
    };
  };
}

// A response applying a socket's or external server's `injectResponseHeaders` before forwarding
// to the response it wraps.
class RewritingResponse final: public kj::HttpService::Response {
 public:
  RewritingResponse(kj::HttpService::Response& inner, kj::Array<Edit> edits)
      : inner(inner),
        edits(kj::mv(edits)) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    return inner.send(statusCode, statusText, rewrite(headers), expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return inner.acceptWebSocket(rewrite(headers));
  }

 private:
  kj::HttpService::Response& inner;
  kj::Array<Edit> edits;

  kj::HttpHeaders rewrite(const kj::HttpHeaders& headers) {
    auto rewritten = headers.cloneShallow();
    for (auto& edit: edits) {
      KJ_IF_SOME(value, edit.value) {
        rewritten.setPtr(edit.id, value);
      } else {
        rewritten.unset(edit.id);
      }
    }
    return rewritten;
  }
};

// The connect() answer of a raw TCP connection: there is no HTTP response to write.
class NullConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {}
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    return newNullOutputStream();
  }
};

// A Rust UDP flow as the DatagramChannel of a UdpConnectCustomEvent.
class UdpFlowChannel final: public DatagramChannel {
 public:
  explicit UdpFlowChannel(::rust::Box<UdpFlow> flow): flow(kj::mv(flow)) {}

  kj::Promise<kj::Maybe<kj::Array<kj::byte>>> receive() override {
    auto datagram = co_await flow->receive();
    if (datagram.ended) co_return kj::none;
    co_return kj::heapArray<kj::byte>(kj::from<Rust>(datagram.data));
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> datagram) override {
    co_await flow->send(datagram.as<Rust>());
  }

 private:
  ::rust::Box<UdpFlow> flow;
};

}  // namespace

kj::Own<kj::HttpHeaders> edit_headers(const kj::HttpHeaderTable& table,
    const kj::HttpHeaders& headers,
    ::rust::Slice<const HeaderEdit> edits,
    ::rust::Slice<const HeaderEdit> injected) {
  auto edited = kj::heap(headers.clone());
  for (auto slice: {edits, injected}) {
    for (auto& edit: resolveEdits(table, slice)) {
      KJ_IF_SOME(value, edit.value) {
        edited->set(edit.id, kj::mv(value));
      } else {
        edited->unset(edit.id);
      }
    }
  }
  return edited;
}

kj::Own<kj::HttpService::Response> new_rewriting_response(kj::HttpService::Response& inner,
    const kj::HttpHeaderTable& table,
    ::rust::Slice<const HeaderEdit> edits) {
  return kj::heap<RewritingResponse>(inner, resolveEdits(table, edits));
}

kj::Own<kj::HttpService::ConnectResponse> new_null_connect_response() {
  return kj::heap<NullConnectResponse>();
}

kj::Own<kj::WebSocketErrorHandler> new_jsgify_websocket_errors() {
  return kj::heap<JsgifyWebSocketErrors>();
}

kj::Own<WorkerInterface::CustomEvent> new_udp_connect_event(
    ::rust::Str address, ::rust::Box<UdpFlow> flow) {
  auto channel = kj::heap<UdpFlowChannel>(kj::mv(flow));
  return kj::heap<api::UdpConnectCustomEvent>(kj::str(address), *channel).attach(kj::mv(channel));
}

}  // namespace workerd::server
