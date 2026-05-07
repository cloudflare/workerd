// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/one-of.h>
#include <kj/string.h>

namespace workerd::api {

class WebSocketDataMessagePtr;

// Owned text-or-binary WebSocket data payload, with a domain-named, read-only inspection
// surface — `isText()` / `isBinary()` / `tryGetText()` / `tryGetBinary()` — plus
// uniform `size()`, `sendVia(kj::WebSocket&)`, `asPtr()`, and `asOneOf()` for
// `KJ_SWITCH_ONEOF` dispatch. Designed for queues and pump state in WebSocket auto-response
// and outgoing-message paths where a message is text-or-binary and flows from
// enqueue → eventually-send → drop.
//
// The wrapper is move-only (the contained `kj::String` / `kj::Array<byte>` are owned).
//
// Inspection delegates through a temporarily-constructed `WebSocketDataMessagePtr`. That has
// two consequences worth understanding:
//
//   * Callers cannot mutate the contained data through inspection — `tryGetText()` returns
//     a `kj::Maybe<kj::StringPtr>` (not `kj::String&`), and `KJ_SWITCH_ONEOF(msg.asOneOf())`
//     cases bind to `kj::StringPtr` / `kj::ArrayPtr<const byte>`. The owned-vs-borrowed
//     distinction is purely about lifetime; reads are always borrows.
//
//   * The storage type is hidden from the inspection surface. If we later change owned
//     storage (e.g. to `kj::ConstString`), no caller needs to update — they speak
//     `StringPtr` / `ArrayPtr<const byte>` regardless.
//
// `KJ_SWITCH_ONEOF` support is provided through `asOneOf()`, which returns a fresh
// `kj::OneOf<kj::StringPtr, kj::ArrayPtr<const byte>>` by value. Callers write
// `KJ_SWITCH_ONEOF(msg.asOneOf())` rather than `KJ_SWITCH_ONEOF(msg)` directly.
class WebSocketDataMessage {
 public:
  // Owned construction.
  explicit WebSocketDataMessage(kj::String text): data(kj::mv(text)) {}
  explicit WebSocketDataMessage(kj::Array<kj::byte> bytes): data(kj::mv(bytes)) {}

  // Lifts a `kj::OneOf` directly into a `WebSocketDataMessage`. Accepts either variant ordering —
  // `kj::OneOf`'s subset ctor handles the reordering, so callers can pass `kj::OneOf<kj::String,
  // kj::Array<byte>>` (matching our storage) or `kj::OneOf<kj::Array<byte>, kj::String>` (matching
  // `WebSocket::send`'s JSG parameter) without writing a per-variant switch.
  explicit WebSocketDataMessage(kj::OneOf<kj::String, kj::Array<kj::byte>> jsgInput)
      : data(kj::mv(jsgInput)) {}

  KJ_DISALLOW_COPY(WebSocketDataMessage);
  WebSocketDataMessage(WebSocketDataMessage&&) = default;
  WebSocketDataMessage& operator=(WebSocketDataMessage&&) = default;

  // Returns a non-owning view of this message. The returned `WebSocketDataMessagePtr`
  // borrows from `*this`; callers must keep `*this` alive for the duration of any use.
  // Defined inline at the bottom of this file, after `WebSocketDataMessagePtr`.
  inline WebSocketDataMessagePtr asPtr() const KJ_LIFETIMEBOUND;

  // Returns a `kj::OneOf` of borrowed views suitable for `KJ_SWITCH_ONEOF`. The borrowed views
  // inside reference `*this`'s storage. `*this` must outlive any use of borrows captured by
  // `KJ_CASE_ONEOF` bindings.
  inline kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>> asOneOf() const KJ_LIFETIMEBOUND;

  // Sends this message over `ws`, dispatching to the appropriate `kj::WebSocket::send`
  // overload based on which variant is held. Equivalent to `asPtr().sendVia(ws)`. The
  // message data (i.e. `*this`) must outlive the returned promise.
  inline kj::Promise<void> sendVia(kj::WebSocket& ws) const;

  // Number of bytes in the text or binary payload.
  inline size_t size() const;

  inline bool isText() const;
  inline bool isBinary() const;

  // Returns a non-owning view of the underlying text. Borrows from `*this`; the returned
  // pointer is valid for the lifetime of `*this`.
  inline kj::Maybe<kj::StringPtr> tryGetText() const KJ_LIFETIMEBOUND;

  // Returns a non-owning view of the underlying bytes. Borrows from `*this`; the returned
  // pointer is valid for the lifetime of `*this`.
  inline kj::Maybe<kj::ArrayPtr<const kj::byte>> tryGetBinary() const KJ_LIFETIMEBOUND;

  // Content-equality against a borrowed text/binary view. Returns false if the message is
  // holding the other variant. C++23 rewrite rules give symmetric `text == msg` /
  // `bytes == msg` for free.
  inline bool operator==(kj::StringPtr text) const;
  inline bool operator==(kj::ArrayPtr<const kj::byte> bytes) const;

 private:
  kj::OneOf<kj::String, kj::Array<kj::byte>> data;
};

// Non-owning counterpart to `WebSocketDataMessage`. Useful at API boundaries that accept a
// borrowed text-or-binary message (e.g. size checks against an inbound payload), and for
// dispatching a queued message onto the wire via `sendVia()`.
//
// Implicit construction from `kj::StringPtr` and `kj::ArrayPtr<const kj::byte>` is allowed —
// short-lived non-owning pointers across argument lists benefit from the convenience and
// there's no ownership to confuse.
class WebSocketDataMessagePtr {
 public:
  WebSocketDataMessagePtr(kj::StringPtr text): data(text) {}
  WebSocketDataMessagePtr(kj::ArrayPtr<const kj::byte> bytes): data(bytes) {}

  // Trivially copyable; no need to spell out copy/move ctors.

  // Returns a copy of the underlying `kj::OneOf<kj::StringPtr, kj::ArrayPtr<const byte>>` for use
  // with `KJ_SWITCH_ONEOF`. The borrowed pointers inside still reference whatever this Ptr was
  // constructed against; valid for that lifetime.
  kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>> asOneOf() const {
    return data;
  }

  // Dispatches to `kj::WebSocket::send(kj::StringPtr)` or
  // `kj::WebSocket::send(kj::ArrayPtr<const byte>)` depending on which variant is held. The
  // referenced bytes/string must outlive the returned promise.
  kj::Promise<void> sendVia(kj::WebSocket& ws) const {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        return ws.send(text);
      }
      KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
        return ws.send(bytes);
      }
    }
    KJ_UNREACHABLE;
  }

  // Number of bytes in the text or binary payload.
  size_t size() const {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        return text.size();
      }
      KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
        return bytes.size();
      }
    }
    KJ_UNREACHABLE;
  }

  // Returns a freshly-allocated owning copy of the referenced data. Useful at API boundaries
  // that accept a borrowed message and need to retain it (e.g. enqueueing for later send).
  WebSocketDataMessage toOwned() const {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(text, kj::StringPtr) {
        return WebSocketDataMessage(kj::str(text));
      }
      KJ_CASE_ONEOF(bytes, kj::ArrayPtr<const kj::byte>) {
        return WebSocketDataMessage(kj::heapArray(bytes));
      }
    }
    KJ_UNREACHABLE;
  }

  bool isText() const {
    return data.is<kj::StringPtr>();
  }
  bool isBinary() const {
    return data.is<kj::ArrayPtr<const kj::byte>>();
  }

  // Returns the underlying `kj::StringPtr` by value. Borrows from whatever this Ptr was constructed
  // against; valid for that lifetime.
  kj::Maybe<kj::StringPtr> tryGetText() const {
    KJ_IF_SOME(text, data.tryGet<kj::StringPtr>()) {
      return text;
    }
    return kj::none;
  }
  kj::Maybe<kj::ArrayPtr<const kj::byte>> tryGetBinary() const {
    KJ_IF_SOME(bytes, data.tryGet<kj::ArrayPtr<const kj::byte>>()) {
      return bytes;
    }
    return kj::none;
  }

  // Content-equality against a borrowed text/binary view. Returns false if this Ptr is
  // holding the other variant.
  bool operator==(kj::StringPtr text) const {
    KJ_IF_SOME(t, tryGetText()) {
      return t == text;
    }
    return false;
  }
  bool operator==(kj::ArrayPtr<const kj::byte> bytes) const {
    KJ_IF_SOME(b, tryGetBinary()) {
      return b == bytes;
    }
    return false;
  }

 private:
  kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>> data;
};

// ============================================================================
// `WebSocketDataMessage` inline definitions — placed after `WebSocketDataMessagePtr` is
// fully declared so its constructors are visible.
// ============================================================================

inline WebSocketDataMessagePtr WebSocketDataMessage::asPtr() const {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(text, kj::String) {
      return WebSocketDataMessagePtr(text.asPtr());
    }
    KJ_CASE_ONEOF(bytes, kj::Array<kj::byte>) {
      return WebSocketDataMessagePtr(bytes.asPtr());
    }
  }
  KJ_UNREACHABLE;
}

inline kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>> WebSocketDataMessage::asOneOf()
    const {
  return asPtr().asOneOf();
}

inline kj::Promise<void> WebSocketDataMessage::sendVia(kj::WebSocket& ws) const {
  return asPtr().sendVia(ws);
}

inline size_t WebSocketDataMessage::size() const {
  return asPtr().size();
}

inline bool WebSocketDataMessage::isText() const {
  return asPtr().isText();
}

inline bool WebSocketDataMessage::isBinary() const {
  return asPtr().isBinary();
}

inline kj::Maybe<kj::StringPtr> WebSocketDataMessage::tryGetText() const {
  return asPtr().tryGetText();
}

inline kj::Maybe<kj::ArrayPtr<const kj::byte>> WebSocketDataMessage::tryGetBinary() const {
  return asPtr().tryGetBinary();
}

inline bool WebSocketDataMessage::operator==(kj::StringPtr text) const {
  return asPtr() == text;
}

inline bool WebSocketDataMessage::operator==(kj::ArrayPtr<const kj::byte> bytes) const {
  return asPtr() == bytes;
}

}  // namespace workerd::api
