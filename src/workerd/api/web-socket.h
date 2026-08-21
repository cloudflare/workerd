// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "basics.h"
#include "events.h"

#include <workerd/io/io-gate.h>
#include <workerd/io/observer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/checked-queue.h>
#include <workerd/util/strong-bool.h>

#include <kj/compat/http.h>

#include <cstdlib>
#include <list>

namespace workerd {
class ActorObserver;
}

namespace workerd::api {

class Blob;

template <typename T>
struct DeferredProxy;

class CloseEvent: public Event {
 public:
  CloseEvent(uint code, kj::String reason, bool clean)
      : Event("close"),
        code(code),
        reason(kj::mv(reason)),
        clean(clean) {}
  CloseEvent(kj::String type, int code, kj::String reason, bool clean)
      : Event(kj::mv(type)),
        code(code),
        reason(kj::mv(reason)),
        clean(clean) {}

  struct Initializer {
    jsg::Optional<int> code;
    jsg::Optional<jsg::USVString> reason;
    jsg::Optional<bool> wasClean;

    JSG_STRUCT(code, reason, wasClean);
    JSG_STRUCT_TS_OVERRIDE(CloseEventInit);
  };
  static jsg::Ref<CloseEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<Initializer> initializer) {
    Initializer init = kj::mv(initializer).orDefault({});
    return js.alloc<CloseEvent>(kj::mv(type), init.code.orDefault(0),
        kj::mv(init.reason).orDefault(jsg::USVString(kj::str())), init.wasClean.orDefault(false));
  }

  int getCode() {
    return code;
  }
  kj::StringPtr getReason() {
    return reason;
  }
  bool getWasClean() {
    return clean;
  }

  JSG_RESOURCE_TYPE(CloseEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(code, getCode);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
    JSG_READONLY_INSTANCE_PROPERTY(wasClean, getWasClean);

    JSG_TS_ROOT();
    // CloseEvent will be referenced from the `WebSocketEventMap` define
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("reason", reason);
  }

 private:
  int code;
  kj::String reason;
  bool clean;
};

WD_STRONG_BOOL(AllowHalfOpen);

// The forward declaration is necessary so we can make some
// WebSocket methods accessible to WebSocketPair via friend declaration.
class WebSocket;

class WebSocketPair: public jsg::Object {
 private:
  struct IteratorState final {
    jsg::Ref<WebSocketPair> pair;
    size_t index = 0;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(pair);
    }

    JSG_MEMORY_INFO(IteratorState) {
      tracker.trackField("pair", pair);
    }
  };

 public:
  WebSocketPair(jsg::Ref<WebSocket> first, jsg::Ref<WebSocket> second)
      : sockets{kj::mv(first), kj::mv(second)} {}

  static jsg::Ref<WebSocketPair> constructor(jsg::Lock& js);

  jsg::Ref<WebSocket> getFirst() {
    return sockets[0].addRef();
  }
  jsg::Ref<WebSocket> getSecond() {
    return sockets[1].addRef();
  }

  JSG_ITERATOR(PairIterator, entries, jsg::Ref<WebSocket>, IteratorState, iteratorNext);

  JSG_RESOURCE_TYPE(WebSocketPair) {
    // TODO(soon): These really should be using an indexed property handler rather
    // than named instance properties but jsg does not yet have support for that.
    JSG_READONLY_INSTANCE_PROPERTY(0, getFirst);
    JSG_READONLY_INSTANCE_PROPERTY(1, getSecond);
    JSG_ITERABLE(entries);

    JSG_TS_OVERRIDE(const WebSocketPair: {
      new (): { 0: WebSocket; 1: WebSocket };
    });
    // Ensure correct typing with `Object.values()`.
    // Without this override, the generated definition will look like:
    //
    // ```ts
    // declare class WebSocketPair {
    //   constructor();
    //   readonly 0: WebSocket;
    //   readonly 1: WebSocket;
    // }
    // ```
    //
    // Trying to call `Object.values(new WebSocketPair())` will result
    // in the following `any` typed values:
    //
    // ```ts
    // const [one, two] = Object.values(new WebSocketPair());
    //       // ^? const one: any
    // ```
    //
    // With this override in place, `one` and `two` will be typed `WebSocket`.
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  jsg::Ref<WebSocket> sockets[2];

  static kj::Maybe<jsg::Ref<WebSocket>> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= 2) {
      return kj::none;
    }
    return state.pair->sockets[state.index++].addRef();
  }

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(sockets[0]);
    visitor.visit(sockets[1]);
  }
};

class WebSocketAdapter;

class WebSocket: public EventTarget {
 public:
  // WebSocket ready states.
  static constexpr int READY_STATE_CONNECTING = 0;
  static constexpr int READY_STATE_OPEN = 1;
  static constexpr int READY_STATE_CLOSING = 2;
  static constexpr int READY_STATE_CLOSED = 3;

  // Some properties of the `api::WebSocket` that need to survive hibernation. When we initiate
  // the hibernation process, we want to move these properties out of the `api::WebSocket`.
  // When we recreate the websocket due to activity, we move the properties back in.
  struct HibernationPackage {
    kj::Maybe<kj::String> url;
    kj::Maybe<kj::String> protocol;
    kj::Maybe<kj::String> extensions;
    kj::Maybe<kj::Array<byte>> serializedAttachment;

    // `maybeTags` is only non-empty when we're recreating the api::WebSocket.
    // We don't need to populate it when hibernating because the tags are already
    // stored in the HibernationManager.
    kj::Maybe<kj::Array<kj::StringPtr>> maybeTags;

    // True forever once the JS WebSocket calls `close()`.
    bool closedOutgoingConnection = false;

    // Whether the WebSocket allows half-open close state.
    AllowHalfOpen allowHalfOpen = AllowHalfOpen::YES;
  };

  ~WebSocket() noexcept(false) = default;

  // This WebSocket constructor is only used when WebSockets wake up from hibernation.
  // It will immediately set the `state` to `Accepted`, but it limits the behavior by specifying it
  // as `Hibernatable` -- thereby making most api::WebSocket methods inaccessible.
  WebSocket(jsg::Lock& js, IoContext& ioContext, kj::WebSocket& ws, HibernationPackage package);

  // Similar to how the JS `constructor()` creates a WebSocket, when waking from hibernation
  // we want to be able to recreate WebSockets from C++ that will be delivered to JS code.
  static jsg::Ref<WebSocket> hibernatableFromNative(
      jsg::Lock& js, kj::WebSocket& ws, HibernationPackage package);

  // The JS WebSocket constructor needs to initiate a connection, but we need to return the
  // WebSocket object to the caller in Javascript immediately. We will defer the connection logic
  // to the `initConnection` method.
  WebSocket(jsg::Lock& js, kj::Own<kj::WebSocket> native);

  // The JS WebSocket constructor needs to initiate a connection, but we need to return the
  // WebSocket object to the caller in Javascript immediately. We will defer the connection logic
  // to the `initConnection` method.
  WebSocket(jsg::Lock& js, kj::String url);

  // Pumps messages from this WebSocket to `other`, and from `other` to this, making sure to
  // register pending events as appropriate. Used to connect a websocket to a client via an HTTP
  // response.
  //
  // Only one of this or accept() is allowed to be invoked.
  //
  // As an exception to the usual KJ convention, it is not necessary for the JavaScript `WebSocket`
  // object to be kept live while waiting for the promise returned by couple() to complete. Instead,
  // the promise takes direct ownership of the underlying KJ-native WebSocket (as well as `other`).
  kj::Promise<DeferredProxy<void>> couple(
      jsg::Lock& js, kj::Own<kj::WebSocket> other, RequestObserver& request);

  // Extract the kj::WebSocket from this api::WebSocket (if applicable). The kj::WebSocket will be
  // owned elsewhere, but the api::WebSocket will retain a reference.
  kj::Own<kj::WebSocket> acceptAsHibernatable(kj::Array<kj::StringPtr> tags);

  // Accesses the tags of the hibernatable websocket.
  kj::Array<kj::StringPtr> getHibernatableTags();

  enum class HibernatableReleaseState {
    // The way we release Hibernatable WebSockets slightly differs from regular WebSockets.
    // We can't access the isolate after the event runs. `NONE` indicates we are not releasing.
    NONE,
    CLOSE,
    ERROR
  };

  // Called when a Hibernatable WebSocket wants to dispatch a close/error event, this modifies
  // our `Accepted` state to prepare the state to transition to `Released`.
  void initiateHibernatableRelease(jsg::Lock& js,
      kj::Own<kj::WebSocket> ws,
      kj::Array<kj::String> tags,
      HibernatableReleaseState releaseState);

  bool awaitingHibernatableError();

  bool awaitingHibernatableRelease();

  // Should only be called on one end of a WebSocketPair.
  // Relevant for WebSocket Hibernation: the end we return in the Response must be in the
  // AwaitingAcceptanceOrCoupling state.
  bool peerIsAwaitingCoupling(jsg::Lock& js);

  HibernationPackage buildPackageForHibernation();

  // ---------------------------------------------------------------------------
  // JS API.

  struct AcceptOptions {
    jsg::Optional<bool> allowHalfOpen;

    JSG_STRUCT(allowHalfOpen);
  };

  // Creates a new outbound WebSocket.
  static jsg::Ref<WebSocket> constructor(jsg::Lock& js,
      kj::String url,
      jsg::Optional<kj::OneOf<kj::Array<kj::String>, kj::String>> protocols);

  // Begin delivering events locally.
  void accept(jsg::Lock& js, jsg::Optional<AcceptOptions> options);

  // Same as accept(), but websockets that are created with `new WebSocket()` in JS cannot call
  // accept(). Instead, we only permit the C++ constructor to call this "internal" version of accept()
  // so that the websocket can start processing messages once the connection has been established.
  void internalAccept(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs);

  void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message);
  void close(jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<jsg::USVString> reason);

  // Used to get/set the attachment for hibernation.
  // If the object isn't serialized, it will not survive hibernation.
  void serializeAttachment(jsg::Lock& js, jsg::JsValue attachment);

  // Used to get/set the attachment for hibernation.
  // If the object isn't serialized, it will not survive hibernation.
  kj::Maybe<jsg::JsValue> deserializeAttachment(jsg::Lock& js);

  // Used to get/store the last auto request/response timestamp for this WebSocket.
  // These methods are c++ only and are not exposed to our js interface.
  // Also used to track hibernatable websockets auto-response sends.
  void setAutoResponseStatus(kj::Maybe<kj::Date> time, kj::Promise<void> autoResponsePromise);

  // Used to get/store the last auto request/response timestamp for this WebSocket.
  // These methods are c++ only and are not exposed to our js interface.
  kj::Maybe<kj::Date> getAutoResponseTimestamp();

  kj::Promise<void> sendAutoResponse(kj::String message, kj::WebSocket& ws);

  int getReadyState();

  bool isAccepted();
  bool isReleased();

  // True iff the underlying adapter is in the post-connect / pre-accept state.
  bool isAwaitingCoupling();

  // True iff the underlying adapter is in the Accepted-Hibernatable sub-state.
  bool isHibernatable();

  // Attach an observer that records traffic on this WebSocket. Used by `couple()` when
  // the local end terminates in this worker.
  void setObserver(kj::Own<WebSocketObserver> observer);

  // For internal use only.
  // We need to access the underlying KJ WebSocket so we can determine the compression configuration
  // it uses (if any).
  kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx);

  kj::Maybe<kj::StringPtr> getUrl();
  kj::Maybe<kj::StringPtr> getProtocol();
  kj::Maybe<kj::StringPtr> getExtensions();

  kj::StringPtr getBinaryType();
  void setBinaryType(kj::String value);

  JSG_RESOURCE_TYPE(WebSocket, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(EventTarget);
    JSG_METHOD(accept);
    JSG_METHOD(send);
    JSG_METHOD(close);
    JSG_METHOD(serializeAttachment);
    JSG_METHOD(deserializeAttachment);

    JSG_STATIC_CONSTANT(READY_STATE_CONNECTING);
    JSG_STATIC_CONSTANT_NAMED(CONNECTING, WebSocket::READY_STATE_CONNECTING);

    JSG_STATIC_CONSTANT(READY_STATE_OPEN);
    JSG_STATIC_CONSTANT_NAMED(OPEN, WebSocket::READY_STATE_OPEN);

    JSG_STATIC_CONSTANT(READY_STATE_CLOSING);
    JSG_STATIC_CONSTANT_NAMED(CLOSING, WebSocket::READY_STATE_CLOSING);

    JSG_STATIC_CONSTANT(READY_STATE_CLOSED);
    JSG_STATIC_CONSTANT_NAMED(CLOSED, WebSocket::READY_STATE_CLOSED);

    // Previously, we were setting all properties as instance properties,
    // which broke the ability to subclass the Event object. With the
    // compatibility flag set, we instead attach the properties to the
    // prototype.
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(readyState, getReadyState);
      JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);
      JSG_READONLY_PROTOTYPE_PROPERTY(protocol, getProtocol);
      JSG_READONLY_PROTOTYPE_PROPERTY(extensions, getExtensions);
      JSG_PROTOTYPE_PROPERTY(binaryType, getBinaryType, setBinaryType);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(readyState, getReadyState);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);
      JSG_READONLY_INSTANCE_PROPERTY(protocol, getProtocol);
      JSG_READONLY_INSTANCE_PROPERTY(extensions, getExtensions);
      JSG_INSTANCE_PROPERTY(binaryType, getBinaryType, setBinaryType);
    }

    JSG_TS_DEFINE(type WebSocketEventMap = {
      close: CloseEvent;
      message: MessageEvent;
      open: Event;
      error: ErrorEvent;
    });
    JSG_TS_OVERRIDE(extends EventTarget<WebSocketEventMap> {
      get binaryType(): "blob" | "arraybuffer";
      set binaryType(value: "blob" | "arraybuffer");
    });
  }

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

  // Forwarders for callers that need direct access to the operational adapter. Most external
  // call sites should use the public methods above, which delegate; the impl getters are for
  // sites that need to thread through to adapter-specific machinery.
  void setPeer(jsg::WeakRef<WebSocket> peer);

 private:
  // The operational implementation. Today this is always a `LegacyWebSocketAdapter` (created
  // in each `WebSocket` constructor below); a follow-up commit introduces a
  // `HibernatableWebSocketAdapter` swapped in at `acceptAsHibernatable()` and on revival.
  //
  // WARNING — DO NOT replace this `kj::Own` after `visitForGc()` has traced through it. V8 marks
  // the traced children (Refs/V8Refs held by the adapter) weak on the trace pass; moving them via
  // the kj::Own swap would leave them weak without anyone tracing them again, and GC can then
  // collect them out from under the new adapter. Today this is moot because the adapter is never
  // swapped; the hibernatable rewrite that does swap it must either (a) defer the swap until after
  // migrating any traced state, (b) wait for the upcoming jsg::Own<T> which integrates with
  // cppgc to handle this correctly, or (c) re-trigger tracing post-swap.
  kj::Own<WebSocketAdapter> impl;
};

#define EW_WEBSOCKET_ISOLATE_TYPES                                                                 \
  api::CloseEvent, api::CloseEvent::Initializer, api::WebSocket, api::WebSocket::AcceptOptions,    \
      api::WebSocketPair, api::WebSocketPair::PairIterator,                                        \
      api::WebSocketPair::PairIterator::                                                           \
          Next  // The list of websocket.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

// Abstract base for the operational implementation of an `api::WebSocket`.
//
// The eventual plan: `api::WebSocket` becomes a thin JSG-facing shell that owns a
// `kj::Own<WebSocketAdapter> impl` and forwards every method to it. The current
// `WebSocket` body — state machine, outgoing-message queue, pump, auto-response, read
// loop, couple(), peer-tracking, identity — will move into a `LegacyWebSocketAdapter`
// implementation in a follow-up commit. A future `HibernatableWebSocketAdapter` will
// live as a sibling implementation, swapped into the shell's impl slot at
// `acceptAsHibernatable()` (and on revival from hibernation).
//
// This commit just adds the abstract base; no behavior changes.
class WebSocketAdapter {
 public:
  WebSocketAdapter() = default;
  WebSocketAdapter(const WebSocketAdapter&) = delete;
  WebSocketAdapter(WebSocketAdapter&&) = delete;
  WebSocketAdapter& operator=(const WebSocketAdapter&) = delete;
  WebSocketAdapter& operator=(WebSocketAdapter&&) = delete;

  // Result of an outbound `new WebSocket(url)` connection attempt: a freshly-opened
  // `kj::WebSocket` plus the negotiated subprotocol / extension strings extracted from
  // the upgrade response headers. The JSG factory `WebSocket::constructor` kicks off an
  // HTTP upgrade asynchronously, then passes a `kj::Promise<PackedWebSocket>` to
  // `initConnection()` below, which on resolution transitions the adapter from
  // AwaitingConnection to AwaitingAcceptanceOrCoupling and fires the `open` event.
  struct PackedWebSocket {
    kj::Own<kj::WebSocket> ws;
    kj::Maybe<kj::String> proto;
    kj::Maybe<kj::String> extensions;
  };

  // -------------------------------------------------------------------------
  // JSG-exposed surface (what JS code calls). The shell will forward verbatim.
  // -------------------------------------------------------------------------

  virtual void accept(jsg::Lock& js, jsg::Optional<WebSocket::AcceptOptions> options) = 0;
  virtual void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message) = 0;
  virtual void close(
      jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<jsg::USVString> reason) = 0;
  virtual void serializeAttachment(jsg::Lock& js, jsg::JsValue attachment) = 0;
  virtual kj::Maybe<jsg::JsValue> deserializeAttachment(jsg::Lock& js) = 0;
  virtual int getReadyState() = 0;
  virtual kj::Maybe<kj::StringPtr> getUrl() = 0;
  virtual kj::Maybe<kj::StringPtr> getProtocol() = 0;
  virtual kj::Maybe<kj::StringPtr> getExtensions() = 0;
  virtual kj::StringPtr getBinaryType() = 0;
  virtual void setBinaryType(kj::String value) = 0;

  // -------------------------------------------------------------------------
  // Internal-but-forwarded surface (called from C++ across the codebase).
  // -------------------------------------------------------------------------

  // Initiates the `new WebSocket(url)` outbound connection. Called once during shell
  // construction for the URL ctor.
  virtual void initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket> packedWsPromise) = 0;

  // Pumps messages between this WebSocket and `other`. Only valid in the post-connect /
  // pre-accept state.
  virtual kj::Promise<DeferredProxy<void>> couple(
      jsg::Lock& js, kj::Own<kj::WebSocket> other, RequestObserver& request) = 0;

  // Like `accept()` but called by C++ rather than JS — used by the URL-ctor success
  // continuation.
  virtual void internalAccept(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) = 0;

  // State predicates.
  virtual bool isAccepted() = 0;
  virtual bool isReleased() = 0;

  // True iff the adapter is in its post-connect / pre-accept state. Used by
  // `peerIsAwaitingCoupling()` on the other end of a WebSocketPair.
  virtual bool isAwaitingCoupling() = 0;

  // True iff the adapter is in the Accepted-Hibernatable sub-state. Used by `couple()`
  // to detect when one end of a pair is owned by the HibernationManager.
  virtual bool isHibernatable() = 0;

  // Attaches an observer that records websocket traffic. Called from `couple()` on the
  // peer end when the local end is terminating in this worker.
  virtual void setObserver(kj::Own<WebSocketObserver> observer) = 0;

  // Hibernation transitions: extracts the kj::WebSocket so the HibernationManager can
  // assume ownership; the adapter retains a bare reference for sends.
  virtual kj::Own<kj::WebSocket> acceptAsHibernatable(kj::Array<kj::StringPtr> tags) = 0;

  // HibernationManager coordination: signals that the underlying connection is winding
  // down and the adapter should arrange to deliver its terminal close/error event.
  virtual void initiateHibernatableRelease(jsg::Lock& js,
      kj::Own<kj::WebSocket> ws,
      kj::Array<kj::String> tags,
      WebSocket::HibernatableReleaseState releaseState) = 0;
  virtual bool awaitingHibernatableError() = 0;
  virtual bool awaitingHibernatableRelease() = 0;

  // Builds a package containing the per-WebSocket state that survives hibernation.
  virtual WebSocket::HibernationPackage buildPackageForHibernation() = 0;

  virtual kj::Array<kj::StringPtr> getHibernatableTags() = 0;
  virtual kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx) = 0;

  // Auto-response coordination (called by the HibernationManager's readLoop, possibly
  // without an enclosing IoContext on the thread).
  virtual void setAutoResponseStatus(
      kj::Maybe<kj::Date> time, kj::Promise<void> autoResponsePromise) = 0;
  virtual kj::Maybe<kj::Date> getAutoResponseTimestamp() = 0;
  virtual kj::Promise<void> sendAutoResponse(kj::String message, kj::WebSocket& ws) = 0;

  // -------------------------------------------------------------------------
  // Peer tracking (the other end of a WebSocketPair).
  // -------------------------------------------------------------------------

  virtual void setPeer(jsg::WeakRef<WebSocket> peer) = 0;
  virtual bool peerIsAwaitingCoupling(jsg::Lock& js) = 0;

  // -------------------------------------------------------------------------
  // GC + memory tracking. The shell's JSG-driven `visitForGc` and `visitForMemoryInfo`
  // will forward to these.
  // -------------------------------------------------------------------------

  virtual void visitForGc(jsg::GcVisitor& visitor) = 0;
  virtual void visitForMemoryInfo(jsg::MemoryTracker& tracker) const = 0;
};

// LegacyWebSocketAdapter holds the operational implementation of an `api::WebSocket` using the
// pre-EW-10817 machinery: a single state-machine state, an outgoing-message queue, an
// auto-response sub-state, and a single pump task. Today this is the adapter chosen for every
// accepted WebSocket — both regular sockets handled within the request lifecycle and
// hibernatable sockets managed by the HibernationManager.
//
// The body of this class is the pre-extraction `api::WebSocket` body, transplanted as-is.
// Keeping the legacy code structure unchanged is a deliberate goal: the legacy path must
// behave identically to before, and the cleanest way to ensure that is to keep its code
// identical to before. A forthcoming `HibernatableWebSocketAdapter` will live as a sibling
// implementation that replaces this adapter for hibernatable WebSockets only.
class LegacyWebSocketAdapter final: public WebSocketAdapter {
 private:
  // Forward declarations.
  struct Native;

 public:
  // Constructor for the JS `new WebSocket(url)` path.
  LegacyWebSocketAdapter(jsg::Lock& js, WebSocket& shell, kj::String url);

  // Constructor for one end of a WebSocketPair (post-connect, pre-accept).
  LegacyWebSocketAdapter(jsg::Lock& js, WebSocket& shell, kj::Own<kj::WebSocket> native);

  // Constructor for revival from hibernation — straight to the Accepted state in the
  // Hibernatable sub-state.
  LegacyWebSocketAdapter(jsg::Lock& js,
      WebSocket& shell,
      IoContext& ioContext,
      kj::WebSocket& ws,
      WebSocket::HibernationPackage package);

  // ---------------------------------------------------------------------------
  // WebSocketAdapter overrides — see WebSocketAdapter for full method docs.
  // ---------------------------------------------------------------------------

  void accept(jsg::Lock& js, jsg::Optional<WebSocket::AcceptOptions> options) override;
  void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message) override;
  void close(jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<jsg::USVString> reason) override;
  void serializeAttachment(jsg::Lock& js, jsg::JsValue attachment) override;
  kj::Maybe<jsg::JsValue> deserializeAttachment(jsg::Lock& js) override;
  int getReadyState() override;
  kj::Maybe<kj::StringPtr> getUrl() override;
  kj::Maybe<kj::StringPtr> getProtocol() override;
  kj::Maybe<kj::StringPtr> getExtensions() override;
  kj::StringPtr getBinaryType() override;
  void setBinaryType(kj::String value) override;

  void initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket> packedWsPromise) override;
  kj::Promise<DeferredProxy<void>> couple(
      jsg::Lock& js, kj::Own<kj::WebSocket> other, RequestObserver& request) override;
  void internalAccept(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs) override;

  bool isAccepted() override;
  bool isReleased() override;
  bool isAwaitingCoupling() override;
  bool isHibernatable() override;
  void setObserver(kj::Own<WebSocketObserver> observer) override;

  kj::Own<kj::WebSocket> acceptAsHibernatable(kj::Array<kj::StringPtr> tags) override;
  void initiateHibernatableRelease(jsg::Lock& js,
      kj::Own<kj::WebSocket> ws,
      kj::Array<kj::String> tags,
      WebSocket::HibernatableReleaseState releaseState) override;
  bool awaitingHibernatableError() override;
  bool awaitingHibernatableRelease() override;

  WebSocket::HibernationPackage buildPackageForHibernation() override;
  kj::Array<kj::StringPtr> getHibernatableTags() override;
  kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx) override;

  void setAutoResponseStatus(
      kj::Maybe<kj::Date> time, kj::Promise<void> autoResponsePromise) override;
  kj::Maybe<kj::Date> getAutoResponseTimestamp() override;
  kj::Promise<void> sendAutoResponse(kj::String message, kj::WebSocket& ws) override;

  void setPeer(jsg::WeakRef<WebSocket> peer) override;
  bool peerIsAwaitingCoupling(jsg::Lock& js) override;

  void visitForGc(jsg::GcVisitor& visitor) override;
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const override;

  // TODO(EW-10817): Re-enable when WebSocket::acceptAsHibernatable consults the
  // `hibernatable-websocket-refactor` autogate. This helper yields the kj::Own<kj::WebSocket>
  // out of `AwaitingAcceptanceOrCoupling` without transitioning state, so the shell can hand
  // it to the HibernationManager and replace this adapter with a
  // `HibernatableWebSocketAdapter` atomically.
  //
  // kj::Own<kj::WebSocket> extractForHibernatableTransition();

 private:
  // ---------------------------------------------------------------------------
  // Internal state machine — represented inline as a `kj::OneOf`. This is the
  // pre-EW-10817 layout, transplanted verbatim.
  // ---------------------------------------------------------------------------

  enum class BinaryType { BLOB, ARRAYBUFFER };

  struct AwaitingConnection {
    // A canceler associated with the pending websocket connection for `new WebSocket()`.
    kj::Canceler canceler;
  };
  struct AwaitingAcceptanceOrCoupling {
    explicit AwaitingAcceptanceOrCoupling(kj::Own<kj::WebSocket> ws): ws(kj::mv(ws)) {}
    kj::Own<kj::WebSocket> ws;
  };
  struct Accepted {
    // A `Hibernatable` WebSocket shares a sub-set of behavior that's already implemented for
    // an `Accepted` WebSocket, so we can think of it a sub-state.
    struct Hibernatable {
      kj::WebSocket& ws;
      // If we have initiated a hibernatable error/close event, we need to take back ownership
      // of the kj::WebSocket so any final queued messages will deliver. We store this owned
      // websocket in `attachedForClose`. Since the `ws` reference is still valid, we prevent
      // usage of `attachedForClose` directly in favor of using `ws` directly.
      kj::Maybe<kj::Own<void>> attachedForClose;

      // We can't move the state to Released after the Hibernatable Close/Error event runs,
      // since we don't have a request on the thread by the time the event completes. If we
      // are "releasing", we may prevent the websocket from doing certain things like calling
      // send/close. We're more restrictive if we're delivering an Error than delivering a
      // Close.
      WebSocket::HibernatableReleaseState releaseState = WebSocket::HibernatableReleaseState::NONE;

      // There are two possible states for tagsRef:
      //   1. kj::Array<kj::StringPtr> — tags owned by the HibernationManager; we just
      //      reference them to save memory.
      //   2. kj::Array<kj::String> — we're going to be dispatching a Close or an Error event
      //      and the HibernatableWebSocket is free to go away mid-dispatch; copy locally.
      kj::OneOf<kj::Array<kj::StringPtr>, kj::Array<kj::String>> tagsRef;
    };

    explicit Accepted(kj::Own<kj::WebSocket> ws, Native& native, IoContext& context);
    explicit Accepted(Hibernatable ws, Native& native, IoContext& context);

    ~Accepted() noexcept(false);

    // A simple wrapper to make it easier to access the underlying kj::WebSocket.
    class WrappedWebSocket {
     public:
      explicit WrappedWebSocket(Hibernatable ws);
      explicit WrappedWebSocket(kj::Own<kj::WebSocket> ws);

      kj::WebSocket* operator->();
      kj::WebSocket& operator*();

      kj::Maybe<kj::Own<kj::WebSocket>&> getIfNotHibernatable();
      kj::Maybe<Hibernatable&> getIfHibernatable();
      kj::Array<kj::StringPtr> getHibernatableTags();

      // Transitions our Hibernatable websocket to a "Releasing" state. The websocket will
      // transition to `Released` when convenient.
      void initiateHibernatableRelease(jsg::Lock& js,
          kj::Own<kj::WebSocket> ws,
          kj::Array<kj::String> tags,
          WebSocket::HibernatableReleaseState state);

      bool isAwaitingRelease();
      bool isAwaitingError();

     private:
      kj::OneOf<kj::Own<kj::WebSocket>, Hibernatable> inner;
    };

    WrappedWebSocket ws;

    bool isHibernatable();

    kj::Promise<void> createAbortTask(Native& native, IoContext& context);
    // Listens for ws->whenAborted() and possibly triggers a proactive shutdown.
    kj::Promise<void> whenAbortedTask = nullptr;

    kj::Maybe<kj::Own<ActorObserver>> actorMetrics;

    // Wraps the pump loop so that we can cancel it when leaving the Accepted state.
    kj::Canceler canceler;
  };

  struct Released {};
  using NativeState =
      kj::OneOf<AwaitingConnection, AwaitingAcceptanceOrCoupling, Accepted, Released>;
  friend kj::StringPtr KJ_STRINGIFY(const NativeState&);

  struct Native {
    NativeState state;

    // Is there currently a task running to pump outgoing messages?
    bool isPumping = false;

    // Has a Close message been enqueued for send? (It may still be in outgoingMessages.
    // Check closedOutgoing && !isPumping to check if it has gone out.)
    bool closedOutgoing = false;

    // Has a Close message been received, or has a premature disconnection occurred?
    bool closedIncoming = false;

    // Have we detected that the peer has stopped accepting messages?
    bool outgoingAborted = false;
  };

  struct GatedMessage {
    kj::Maybe<kj::Promise<void>> outputLock;  // must wait for this before actually sending
    kj::WebSocket::Message message;
    size_t pendingAutoResponses = 0;
  };
  using OutgoingMessagesMap = kj::Table<GatedMessage, kj::InsertionOrderIndex>;

  // Tracks hibernatable auto-response status to avoid racing between regular websocket
  // messages and auto-responses.
  struct AutoResponse {
    using OwnedAutoResponsePromise =
        kj::OneOf<IoOwn<kj::Promise<void>>, kj::Own<kj::Promise<void>>>;
    kj::Maybe<OwnedAutoResponsePromise> ongoingAutoResponse;
    workerd::util::Queue<kj::String> pendingAutoResponseDeque;
    size_t queuedAutoResponses = 0;
    bool isPumping = false;
    bool isClosed = false;

    JSG_MEMORY_INFO(AutoResponse) {
      tracker.trackFieldWithSize("ongoingAutoResponse", sizeof(kj::Promise<void>));
      pendingAutoResponseDeque.forEach(
          [&](const kj::String& message) { tracker.trackField(nullptr, message); });
    }
  };

  // Maximum allowed size for WebSocket messages
  inline static const size_t SUGGESTED_MAX_MESSAGE_SIZE = 1u << 20;

  // Maximum size of a WebSocket attachment.
  inline static const size_t MAX_ATTACHMENT_SIZE = 1024 * 16;

  // ---------------------------------------------------------------------------
  // Internal helpers (private).
  // ---------------------------------------------------------------------------

  // Creates a fresh `Native` set up in the Accepted-Hibernatable state. Used by the
  // hibernation-revival constructor.
  IoOwn<Native> initNative(IoContext& ioContext,
      kj::WebSocket& ws,
      kj::Array<kj::StringPtr> tags,
      bool closedOutgoingConn);

  void dispatchOpen(jsg::Lock& js);
  void ensurePumping(jsg::Lock& js);

  // Defers to readLoop; broken out separately so that both `accept(opts)` and
  // `internalAccept` (the URL-ctor success continuation) can launch the loop with shared
  // setup logic.
  void startReadLoop(jsg::Lock& js, kj::Maybe<kj::Own<InputGate::CriticalSection>> cs);

  // Returns the number of pending auto-responses that should be sent before the next
  // outgoing message, advancing the queued counter. Called each time a `GatedMessage` is
  // inserted into `outgoingMessages` to guarantee ordering between user sends and
  // auto-pongs.
  size_t getPendingAutoResponseCount();

  // Drains queued outgoing messages (and any auto-responses) onto the wire. Runs without
  // an isolate lock on the IoContext's thread; cancellation propagates through
  // `Accepted::canceler` when the active state is torn down. The function is `static`
  // (taking `outgoingMessages`/`autoResponse`/`observer`/`native` by reference) to make
  // it explicit that the pump must not touch JSG-owned state without the isolate lock.
  static kj::Promise<void> pump(IoContext& context,
      OutgoingMessagesMap& outgoingMessages,
      kj::WebSocket& ws,
      Native& native,
      AutoResponse& autoResponse,
      kj::Maybe<kj::Own<WebSocketObserver>>& observer);

  kj::Promise<kj::Maybe<kj::Exception>> readLoop(
      kj::Maybe<kj::Own<InputGate::CriticalSection>> cs, size_t maxMessageSize);

  void reportError(jsg::Lock& js, kj::Exception&& e);
  void reportError(jsg::Lock& js, jsg::JsRef<jsg::JsValue> err);

  void assertNoError(jsg::Lock& js);

  void tryReleaseNative(jsg::Lock& js);

  // ---------------------------------------------------------------------------
  // Members. The pre-EW-10817 `api::WebSocket` layout, with the addition of `shell` (the
  // back-reference for event dispatch / JSG_THIS).
  // ---------------------------------------------------------------------------

  // Back-reference to the JSG-visible shell. Used to dispatch events
  // (`shell.dispatchEventImpl(...)`) and to build a strong `jsg::Ref<WebSocket>` from
  // within adapter coroutines (`jsg::_jsgThis(&shell)`). The shell strictly outlives
  // this adapter — the adapter is owned by `shell.impl`.
  WebSocket& shell;

  kj::Maybe<kj::String> url;
  kj::Maybe<kj::String> protocol = kj::String();
  kj::Maybe<kj::String> extensions = kj::String();
  // The binaryType attribute per the WHATWG WebSocket spec. Defaults to "blob" when the
  // websocket_standard_binary_type compat flag is enabled, "arraybuffer" otherwise.
  BinaryType binaryType_ = BinaryType::ARRAYBUFFER;

  kj::Maybe<kj::Date> autoResponseTimestamp;
  // All WebSockets have this property. Starts null; can be assigned any serializable
  // value. Survives hibernation. Re-serialized on each setter call to enforce the size
  // limit.
  kj::Maybe<kj::Array<byte>> serializedAttachment;

  // Tracks farNative->closedOutgoing, but we need to access it when we trigger Hibernation
  // so it cannot be `IoOwn`ed as `farNative` is. Informs the HibernatableWebSocket if we
  // called `close()`, preventing send() after revival.
  bool closedOutgoingForHib = false;

  // When YES, a server-initiated close does NOT automatically send a reciprocal close
  // frame, leaving readyState as CLOSING (2) when the close event fires. When NO (the
  // spec-compliant default with the `web_socket_auto_reply_to_close` compat flag), a
  // close reply is sent automatically and readyState is CLOSED (3) when the close event
  // fires.
  AllowHalfOpen allowHalfOpen = AllowHalfOpen::YES;

  // The underlying native WebSocket (or a promise that will emplace one). State
  // transition diagram:
  //   - Starts as AwaitingConnection if the `WebSocket(url)` ctor is used.
  //   - Starts as AwaitingAcceptanceOrCoupling if the `WebSocket(native)` ctor is used.
  //   - Transitions from AwaitingConnection to AwaitingAcceptanceOrCoupling when the
  //     native connection is established and to Accepted once the read loop starts.
  //   - Transitions from AwaitingConnection to Released when connection establishment
  //     fails.
  //   - Transitions from AwaitingAcceptanceOrCoupling to Accepted when it is accepted.
  //   - Transitions from AwaitingAcceptanceOrCoupling to Released when it is coupled to
  //     another web socket.
  //   - Transitions from Accepted to Released when outgoing pump is done and either both
  //     directions have seen "close" messages or an error has occurred.
  IoOwn<Native> farNative;

  // If any error has occurred.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> error;

  // Queue of messages to be sent. Wrapped in an IoOwn so the pump loop can safely access
  // the map without locking the isolate.
  IoOwn<OutgoingMessagesMap> outgoingMessages;

  AutoResponse autoResponseStatus;

  kj::Maybe<kj::Own<WebSocketObserver>> observer;

  // The other end of a WebSocketPair, when applicable. Held as a WeakRef to avoid a
  // strong-ref cycle that would prevent GC of either end.
  kj::Maybe<jsg::WeakRef<WebSocket>> peer;
};

}  // namespace workerd::api
