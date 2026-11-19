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

#include <atomic>
#include <cstdlib>

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

// A test hook. With WORKERD_TEST_REPORT_LOOP in the environment, the response to a request from
// an HTTP socket has an `x-workerd-test-loop` header with three numbers: the event loop that
// serves the request, the thread that dispatched the request, and the thread that sends the
// response's head. The threads differ when the loop moved to another thread meanwhile
// (`workerd serve --loops`). The numbers are the order in which loops and threads first
// reported, not system ids.
bool reportsLoop() {
  static const bool enabled = getenv("WORKERD_TEST_REPORT_LOOP") != nullptr;
  return enabled;
}

uint reportedThread() {
  static std::atomic<uint> threads = 0;
  static thread_local const uint thread = threads.fetch_add(1, std::memory_order_relaxed);
  return thread;
}

uint reportedLoop() {
  static std::atomic<uint> loops = 0;
  static const kj::EventLoopLocal<kj::Maybe<uint>> loop;
  KJ_IF_SOME(number, *loop) {
    return number;
  }
  return loop->emplace(loops.fetch_add(1, std::memory_order_relaxed));
}

// A response applying a socket's or external server's `injectResponseHeaders` before forwarding
// to the response it wraps.
class RewritingResponse final: public kj::HttpService::Response {
 public:
  RewritingResponse(kj::HttpService::Response& inner, kj::Array<Edit> edits)
      : inner(inner),
        edits(kj::mv(edits)) {
    if (reportsLoop()) dispatchThread = reportedThread();
  }

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
  // See `reportsLoop()`.
  kj::Maybe<uint> dispatchThread;

  kj::HttpHeaders rewrite(const kj::HttpHeaders& headers) {
    auto rewritten = headers.cloneShallow();
    for (auto& edit: edits) {
      KJ_IF_SOME(value, edit.value) {
        rewritten.setPtr(edit.id, value);
      } else {
        rewritten.unset(edit.id);
      }
    }
    KJ_IF_SOME(dispatched, dispatchThread) {
      rewritten.addPtr("x-workerd-test-loop"_kj,
          kj::str(reportedLoop(), ' ', dispatched, ' ', reportedThread()));
    }
    return rewritten;
  }
};

// The user Worker's answer to a request for a remote image, sent only if
// `should_block_image_response` lets it through, else replaced by a 403 whose body `finish()`
// writes.
class ImageFilterResponse final: public kj::HttpService::Response {
 public:
  explicit ImageFilterResponse(kj::HttpService::Response& inner): inner(inner) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    auto type = headers.get(kj::HttpHeaderId::CONTENT_TYPE).orDefault(""_kj);
    if (!should_block_image_response(
            static_cast<uint16_t>(statusCode), type.asBytes().as<Rust>())) {
      return inner.send(statusCode, statusText, headers, expectedBodySize);
    }
    auto blocked = headers.cloneShallow();
    blocked.clear();
    blocked.setPtr(kj::HttpHeaderId::CONTENT_TYPE, "text/plain;charset=UTF-8");
    body = inner.send(403, "Forbidden", blocked, BLOCKED.size());
    return newNullOutputStream();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return inner.acceptWebSocket(headers);
  }

  kj::Promise<void> finish() {
    KJ_IF_SOME(out, body) {
      co_await out->write(BLOCKED.asBytes());
    }
  }

 private:
  static constexpr kj::StringPtr BLOCKED = "Blocked"_kj;
  kj::HttpService::Response& inner;
  kj::Maybe<kj::Own<kj::AsyncOutputStream>> body;
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

kj::Own<kj::HttpService::Response> new_image_filter_response(kj::HttpService::Response& inner) {
  return kj::heap<ImageFilterResponse>(inner);
}

kj::Promise<void> image_filter_finish(kj::HttpService::Response& response) {
  return kj::downcast<ImageFilterResponse>(response).finish();
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
