// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/web-socket.h>
#include <workerd/io/observer.h>
#include <workerd/io/trace.h>

#include <kj/compat/http.h>
#include <kj/function.h>
#include <kj/refcount.h>

namespace workerd {

// Why a HibernationManager's WebSockets can't be handed off to another manager. Returned by
// checkWebSocketHandoff(), and carried by the failures of detachWebSocketsForHandoff() that it
// causes (see getWebSocketHandoffBlocker()).
enum class WebSocketHandoffBlocker : uint8_t {
  UNSUPPORTED,
  EVENT_IN_PROGRESS,
  WEBSOCKET_AWAKE,
  WEBSOCKET_CLOSED,
  NO_TRANSPORT,
  STATE_TOO_LARGE,
  LOOPBACK_ARRIVED,
  WEBSOCKETS_CHANGED,
  TRANSPORT_MISCOUNTED,
};

inline kj::StringPtr KJ_STRINGIFY(WebSocketHandoffBlocker blocker) {
  switch (blocker) {
    case WebSocketHandoffBlocker::UNSUPPORTED:
      return "the HibernationManager can't hand off WebSockets"_kj;
    case WebSocketHandoffBlocker::EVENT_IN_PROGRESS:
      return "an event is in progress"_kj;
    case WebSocketHandoffBlocker::WEBSOCKET_AWAKE:
      return "a WebSocket is awake"_kj;
    case WebSocketHandoffBlocker::WEBSOCKET_CLOSED:
      return "a WebSocket has sent its Close"_kj;
    case WebSocketHandoffBlocker::NO_TRANSPORT:
      return "a WebSocket has no transport"_kj;
    case WebSocketHandoffBlocker::STATE_TOO_LARGE:
      return "the WebSockets' state is too large"_kj;
    case WebSocketHandoffBlocker::LOOPBACK_ARRIVED:
      return "a loopback arrived"_kj;
    case WebSocketHandoffBlocker::WEBSOCKETS_CHANGED:
      return "the WebSockets or their settings changed"_kj;
    case WebSocketHandoffBlocker::TRANSPORT_MISCOUNTED:
      return "a WebSocket received more messages than its transport sent"_kj;
  }
  KJ_UNREACHABLE;
}

constexpr kj::Exception::DetailTypeId WEBSOCKET_HANDOFF_BLOCKER_DETAIL_ID = 0x8f41c2d7a96b3e05ull;

// The failure of a handoff that `blocker` prevents.
inline kj::Exception makeWebSocketHandoffException(WebSocketHandoffBlocker blocker) {
  auto exception = KJ_EXCEPTION(FAILED, "hibernatable WebSockets can't be handed off", blocker);
  exception.setDetail(WEBSOCKET_HANDOFF_BLOCKER_DETAIL_ID,
      kj::heapArray<kj::byte>({static_cast<kj::byte>(blocker)}));
  return exception;
}

// What prevented the handoff that `exception` reports, or kj::none if no blocker did, such as when
// a transport failed to pause.
inline kj::Maybe<WebSocketHandoffBlocker> getWebSocketHandoffBlocker(
    const kj::Exception& exception) {
  KJ_IF_SOME(detail, exception.getDetail(WEBSOCKET_HANDOFF_BLOCKER_DETAIL_ID)) {
    if (detail.size() == 1) {
      return static_cast<WebSocketHandoffBlocker>(detail[0]);
    }
  }
  return kj::none;
}

// checkWebSocketHandoff() counts these toward the WebSockets' state besides its strings. They allow
// for the fixed-size state of each socket and each of its tags, and for the framing of the encoding
// that carries it to another process, such as Cap'n Proto's pointers and padding.
constexpr size_t WEBSOCKET_HANDOFF_BYTES_PER_SOCKET = 128;
constexpr size_t WEBSOCKET_HANDOFF_BYTES_PER_TAG = 16;

// The transport currently carrying a hibernatable WebSocket to its client, and how many of the
// messages it sent the socket has received. The HibernationManager keeps one per socket. Whatever
// connects the socket to a transport attaches it: couple(), for the request whose response carries
// the socket's client end, or an embedder that reconnects an adopted socket.
class HibernatableTransport {
 public:
  // Makes `handle` the socket's transport, replacing any previous one, and restarts the count of
  // received messages, which is compared against the count the transport reports when paused.
  // `handle` is kj::none if what connected the socket supplied none. The socket then receives in a
  // way that a handoff would interrupt, so from then on it can't be handed off, and it keeps no
  // handle that a later transport supplies.
  void attach(kj::Maybe<kj::Own<HibernatableTransportHandle>> handle) {
    KJ_IF_SOME(callback, onAttach) {
      callback();
    }
    if (handle == kj::none) {
      attachedWithoutHandle = true;
    }
    if (attachedWithoutHandle) {
      this->handle = kj::none;
    } else {
      this->handle = kj::mv(handle);
    }
    consumedCount = 0;
  }

  kj::Maybe<HibernatableTransportHandle&> getHandle() {
    return handle;
  }

  // False once the socket was connected to a transport without a handle. Until the socket is
  // connected, it may yet get one.
  bool mayHaveHandle() const {
    return !attachedWithoutHandle;
  }

  // Messages, including Close, that the socket has received since its transport was attached.
  uint64_t getConsumedCount() const {
    return consumedCount;
  }

  void countConsumed() {
    ++consumedCount;
  }

  // Sets what attach() calls before it replaces the handle, until this is called again. A
  // detachment in progress uses it to learn that the pause it holds, which the old handle may still
  // be taking, no longer describes the socket's transport.
  void setOnAttach(kj::Maybe<kj::Function<void()>> callback) {
    onAttach = kj::mv(callback);
  }

 private:
  kj::Maybe<kj::Own<HibernatableTransportHandle>> handle;
  bool attachedWithoutHandle = false;
  uint64_t consumedCount = 0;
  kj::Maybe<kj::Function<void()>> onAttach;
};

// Hibernated WebSockets taken out of their HibernationManager for a handoff, with the manager
// settings that apply to them. adoptWebSockets() puts them into a manager again: the same one if
// the handoff is abandoned, or the one taking over. An embedder that moves them to another process
// builds them there from their state, each with the new process's end of its connection.
struct DetachedHibernatableWebSockets {
  struct Socket {
    // The Durable Object's end of the connection.
    kj::Rc<kj::WebSocket> ws;

    // The receive on `ws` that was waiting for the next message when the socket was detached, if
    // any. Cancelling it can end what feeds `ws`, such as the pump from a coupled WebSocket, so an
    // adopter takes the next message from it rather than calling ws->receive() again.
    kj::Maybe<kj::Promise<kj::WebSocket::Message>> pendingReceive;

    api::WebSocket::HibernationPackage package;
    kj::Array<kj::String> tags;
    kj::Maybe<kj::Date> autoResponseTimestamp;
    kj::Maybe<tracing::SpanContext> userSpanContext;

    // Shared with the manager that adopts the socket. Nothing else reaches it after adoption, so an
    // embedder that will reconnect the adopted socket to another transport keeps a reference to
    // attach() that transport through.
    kj::Rc<HibernatableTransport> transport;

    // The pause that detachment took on the transport, or kj::none for a socket built some other
    // way. Dropping it resumes the transport.
    kj::Maybe<HibernatableTransportHandle::Pause> pause;
  };

  struct AutoResponse {
    kj::String request;
    kj::String response;
  };

  kj::Maybe<AutoResponse> autoResponse;
  kj::Maybe<uint32_t> eventTimeoutMs;
  kj::Array<Socket> sockets;
};

// Hibernated WebSockets that detachWebSocketsForHandoff() has readied for a handoff: their
// transports are paused, and each socket has received everything its transport sent. They stay in
// their manager until take() takes them out, so dropping this first leaves them there and resumes
// their transports. Until then, the manager abandons the handoff on anything that would change
// them.
class HibernatableWebSocketDetachment {
 public:
  virtual ~HibernatableWebSocketDetachment() noexcept(false) = default;

  // Takes the sockets out of their manager, leaving it empty, or throws why the handoff was
  // abandoned, leaving them in place. Call at most once.
  virtual DetachedHibernatableWebSockets take() = 0;
};

}  // namespace workerd
