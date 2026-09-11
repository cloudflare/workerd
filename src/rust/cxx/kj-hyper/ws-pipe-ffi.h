// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

// C++ -> Rust bridge shims over kj::WebSocket, for the Rust WebSocket pipe (ws_pipe.rs): the
// pipe's pump-adoption states operate on *foreign* kj::WebSocket objects (the real sockets a
// pipe end is pumped to/from), so the Rust state machine needs to drive them. The mirror image
// of new_rust_websocket() (which wraps a Rust WebSocket AS a kj::WebSocket). This header is
// included by the generated cxx bridge, so it must not include ffi.rs.h itself.
//
// Aliasing/threading contract (the same discipline as kj-rs-http/ffi.h's stream shims):
// kj::WebSocket forbids concurrent same-direction operations — the pipe state machine enforces
// at most one receive and one send/pump in flight per direction, exactly as kj's own
// WebSocketPipeImpl does — and every call happens on the KJ event-loop thread owning the
// socket. Each `WebSocket&` is a live mutable object whose lifetime the caller guarantees for
// the duration of the returned promise (kj's pump contract: pumped-to/from sockets outlive the
// pump promise). getPreferredExtensions takes a shared reference recovered to non-const inside
// (the shared-receiver rule): kj calls it on sockets with pumps in flight, so the Rust side
// must never hold an exclusive borrow for it.

#include <rust/cxx.h>

#include <kj/compat/http.h>

namespace workerd::rust::kj_hyper {

inline kj::Promise<void> websocket_send_text(
    const kj::WebSocket& ws, ::rust::Slice<const uint8_t> text) {
  return const_cast<kj::WebSocket&>(ws).send(
      kj::ArrayPtr<const char>(reinterpret_cast<const char*>(text.data()), text.size()));
}

inline kj::Promise<void> websocket_send_binary(
    const kj::WebSocket& ws, ::rust::Slice<const uint8_t> data) {
  return const_cast<kj::WebSocket&>(ws).send(
      kj::ArrayPtr<const kj::byte>(data.data(), data.size()));
}

inline kj::Promise<void> websocket_close(
    const kj::WebSocket& ws, uint16_t code, ::rust::Slice<const uint8_t> reason) {
  return const_cast<kj::WebSocket&>(ws).close(code,
      kj::str(
          kj::ArrayPtr<const char>(reinterpret_cast<const char*>(reason.data()), reason.size())));
}

inline void websocket_disconnect(const kj::WebSocket& ws) {
  const_cast<kj::WebSocket&>(ws).disconnect();
}

inline kj::Promise<void> websocket_when_aborted(const kj::WebSocket& ws) {
  return const_cast<kj::WebSocket&>(ws).whenAborted();
}

// Full-pump splice: connect two foreign sockets directly (kj's pump-meets-pump adoption).
inline kj::Promise<void> websocket_pump_to(const kj::WebSocket& from, const kj::WebSocket& to) {
  return const_cast<kj::WebSocket&>(from).pumpTo(const_cast<kj::WebSocket&>(to));
}

// Received-message holder: websocket_receive_into() fills it, the accessors flatten it for the
// bridge (kind 0 = text, 1 = binary, 2 = close; data() is the text/payload/reason bytes). An
// opaque C++ type + Promise<void> keeps the bridged shapes simple.
class PipeWsMessage final {
 public:
  kj::Promise<void> receiveFrom(const kj::WebSocket& ws, size_t maxSize) {
    message = co_await const_cast<kj::WebSocket&>(ws).receive(maxSize);
  }

  uint8_t kind() const {
    KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(message)) {
      KJ_CASE_ONEOF(text, kj::String) {
        return 0;
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        return 1;
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        return 2;
      }
    }
    KJ_UNREACHABLE;
  }

  ::rust::Slice<const uint8_t> data() const {
    KJ_SWITCH_ONEOF(KJ_ASSERT_NONNULL(message)) {
      KJ_CASE_ONEOF(text, kj::String) {
        return {text.asBytes().begin(), text.size()};
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        return {data.begin(), data.size()};
      }
      KJ_CASE_ONEOF(close, kj::WebSocket::Close) {
        return {close.reason.asBytes().begin(), close.reason.size()};
      }
    }
    KJ_UNREACHABLE;
  }

  uint16_t closeCode() const {
    return KJ_ASSERT_NONNULL(message).get<kj::WebSocket::Close>().code;
  }

 private:
  kj::Maybe<kj::WebSocket::Message> message;
};

inline kj::Own<PipeWsMessage> new_pipe_ws_message() {
  return kj::heap<PipeWsMessage>();
}

inline kj::Promise<void> websocket_receive_into(
    const kj::WebSocket& ws, size_t maxSize, PipeWsMessage& out) {
  return out.receiveFrom(ws, maxSize);
}

inline uint64_t websocket_received_byte_count(const kj::WebSocket& ws) {
  return const_cast<kj::WebSocket&>(ws).receivedByteCount();
}

// Forwarding for getPreferredExtensions through an active pump. `ws` is const only at the FFI
// ABI (Rust holds pump targets as shared pointers because a pump promise concurrently holds
// the socket); the pointee is a live mutable socket, so recovering the non-const reference for
// the virtual call is well-defined (the shared-receiver rule; see hyper-server-ffi.h).
// Returns true and fills `out` iff the socket expressed a preference.
inline bool websocket_get_preferred_extensions(
    const kj::WebSocket& ws, bool isRequestContext, ::rust::String& out) {
  auto ctx = isRequestContext ? kj::WebSocket::ExtensionsContext::REQUEST
                              : kj::WebSocket::ExtensionsContext::RESPONSE;
  KJ_IF_SOME(preferred, const_cast<kj::WebSocket&>(ws).getPreferredExtensions(ctx)) {
    out = ::rust::String(preferred.begin(), preferred.size());
    return true;
  }
  return false;
}

}  // namespace workerd::rust::kj_hyper
