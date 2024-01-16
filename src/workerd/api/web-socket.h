// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <kj/compat/http.h>
#include "basics.h"
#include <workerd/io/io-context.h>
#include <stdlib.h>

namespace workerd {
  class ActorObserver;
}

namespace workerd::api {

template <typename T> struct DeferredProxy;

class MessageEvent: public Event {
public:
  MessageEvent(jsg::Lock& js, const jsg::JsValue& data)
      : Event("message"), data(jsg::JsRef(js, data)) {}
  MessageEvent(jsg::Lock& js, jsg::JsRef<jsg::JsValue> data)
      : Event("message"), data(kj::mv(data)) {}
  MessageEvent(jsg::Lock& js, kj::String type, const jsg::JsValue& data)
      : Event(kj::mv(type)), data(jsg::JsRef(js, kj::mv(data))) {}
  MessageEvent(jsg::Lock& js, kj::String type, jsg::JsRef<jsg::JsValue> data)
      : Event(kj::mv(type)), data(kj::mv(data)) {}

  struct Initializer {
    jsg::JsRef<jsg::JsValue> data;

    JSG_STRUCT(data);
    JSG_STRUCT_TS_OVERRIDE(MessageEventInit {
      data: ArrayBuffer | string;
    });
  };
  static jsg::Ref<MessageEvent> constructor(jsg::Lock& js,
                                            kj::String type,
                                            Initializer initializer) {
    return jsg::alloc<MessageEvent>(js, kj::mv(type), kj::mv(initializer.data));
  }

  jsg::JsValue getData(jsg::Lock& js) { return data.getHandle(js); }

  jsg::Unimplemented getOrigin() { return jsg::Unimplemented(); }
  jsg::Unimplemented getLastEventId() { return jsg::Unimplemented(); }
  jsg::Unimplemented getSource() { return jsg::Unimplemented(); }
  jsg::Unimplemented getPorts() { return jsg::Unimplemented(); }

  JSG_RESOURCE_TYPE(MessageEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(data, getData);

    JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
    JSG_READONLY_INSTANCE_PROPERTY(lastEventId, getLastEventId);
    JSG_READONLY_INSTANCE_PROPERTY(source, getSource);
    JSG_READONLY_INSTANCE_PROPERTY(ports, getPorts);

    JSG_TS_ROOT();
    // MessageEvent will be referenced from the `WebSocketEventMap` define
    JSG_TS_OVERRIDE({
      readonly data: ArrayBuffer | string;
    });
  }

private:
  jsg::JsRef<jsg::JsValue> data;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(data);
  }
};

class CloseEvent: public Event {
public:
  CloseEvent(uint code, kj::String reason, bool clean)
      : Event("close"), code(code), reason(kj::mv(reason)), clean(clean) {}
  CloseEvent(kj::String type, int code, kj::String reason, bool clean)
      : Event(kj::mv(type)), code(code), reason(kj::mv(reason)), clean(clean) {}

  struct Initializer {
    jsg::Optional<int> code;
    jsg::Optional<kj::String> reason;
    jsg::Optional<bool> wasClean;

    JSG_STRUCT(code, reason, wasClean);
    JSG_STRUCT_TS_OVERRIDE(CloseEventInit);
  };
  static jsg::Ref<CloseEvent> constructor(kj::String type, Initializer initializer) {
    return jsg::alloc<CloseEvent>(kj::mv(type),
        initializer.code.orDefault(0),
        kj::mv(initializer.reason).orDefault(nullptr),
        initializer.wasClean.orDefault(false));
  }

  int getCode() { return code; }
  kj::StringPtr getReason() { return reason; }
  bool getWasClean() { return clean; }

  JSG_RESOURCE_TYPE(CloseEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(code, getCode);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
    JSG_READONLY_INSTANCE_PROPERTY(wasClean, getWasClean);

    JSG_TS_ROOT();
    // CloseEvent will be referenced from the `WebSocketEventMap` define
  }

private:
  int code;
  kj::String reason;
  bool clean;
};

class ErrorEvent: public Event {
public:
  ErrorEvent(jsg::Lock& js, kj::String&& message, jsg::JsRef<jsg::JsValue> error)
      : Event("error"), message(kj::mv(message)), error(kj::mv(error)) {}

  static jsg::Ref<ErrorEvent> constructor() = delete;

  // Due to the context in which we use this ErrorEvent class (internal errors), the getters for
  // filename, lineNo, and colNo are all falsy.
  kj::String getFilename() { return nullptr; }
  kj::StringPtr getMessage() { return message; }
  int getLineno() { return 0; }
  int getColno() { return 0; }
  jsg::JsValue getError(jsg::Lock& js) { return error.getHandle(js); }


  JSG_RESOURCE_TYPE(ErrorEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(filename, getFilename);
    JSG_READONLY_INSTANCE_PROPERTY(message, getMessage);
    JSG_READONLY_INSTANCE_PROPERTY(lineno, getLineno);
    JSG_READONLY_INSTANCE_PROPERTY(colno, getColno);
    JSG_READONLY_INSTANCE_PROPERTY(error, getError);

    JSG_TS_ROOT();
    // ErrorEvent will be referenced from the `WebSocketEventMap` define
  }

private:
  kj::String message;
  jsg::JsRef<jsg::JsValue> error;

  void visitForGc(jsg::GcVisitor& visitor);
};

// The forward declaration is necessary so we can make some
// WebSocket methods accessible to WebSocketPair via friend declaration.
class WebSocket;

class WebSocketPair: public jsg::Object {
public:
  WebSocketPair(jsg::Ref<WebSocket> first, jsg::Ref<WebSocket> second)
      : sockets { kj::mv(first), kj::mv(second) } {}

  static jsg::Ref<WebSocketPair> constructor();

  jsg::Ref<WebSocket> getFirst() { return sockets[0].addRef(); }
  jsg::Ref<WebSocket> getSecond() { return sockets[1].addRef(); }

  JSG_RESOURCE_TYPE(WebSocketPair) {
    // TODO(soon): These really should be using an indexed property handler rather
    // than named instance properties but jsg does not yet have support for that.
    JSG_READONLY_INSTANCE_PROPERTY(0, getFirst);
    JSG_READONLY_INSTANCE_PROPERTY(1, getSecond);

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

private:
  jsg::Ref<WebSocket> sockets[2];
};

class WebSocket: public EventTarget {
private:
  // Forward declarations.
  struct PackedWebSocket;
  struct Native;
public:
  enum Locality {
    // This is one end of a local WebSocketPair. Do not use IoContext::registerPendingEvent()
    // when waiting on this WebSocket.
    LOCAL,

    // This is a remote WebSocket. Use IoContext::registerPendingEvent() when waiting.
    REMOTE
  };

  // WebSocket ready states.
  static constexpr int READY_STATE_CONNECTING = 0;
  static constexpr int READY_STATE_OPEN = 1;
  static constexpr int READY_STATE_CLOSING = 2;
  static constexpr int READY_STATE_CLOSED = 3;

  // Creates the Native object when we recreate the WebSocket when waking from hibernation.
  IoOwn<Native> initNative(
      IoContext& ioContext,
      kj::WebSocket& ws,
      kj::Array<kj::StringPtr> tags,
      bool closedOutgoingConn);

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
  };

  // This WebSocket constructor is only used when WebSockets wake up from hibernation.
  // It will immediately set the `state` to `Accepted`, but it limits the behavior by specifying it
  // as `Hibernatable` -- thereby making most api::WebSocket methods inaccessible.
  WebSocket(jsg::Lock& js, IoContext& ioContext, kj::WebSocket& ws, HibernationPackage package);

  // Similar to how the JS `constructor()` creates a WebSocket, when waking from hibernation
  // we want to be able to recreate WebSockets from C++ that will be delivered to JS code.
  static jsg::Ref<WebSocket> hibernatableFromNative(
      jsg::Lock& js,
      kj::WebSocket& ws,
      HibernationPackage package);

  // The JS WebSocket constructor needs to initiate a connection, but we need to return the
  // WebSocket object to the caller in Javascript immediately. We will defer the connection logic
  // to the `initConnection` method.
  WebSocket(kj::Own<kj::WebSocket> native, Locality locality);

  // The JS WebSocket constructor needs to initiate a connection, but we need to return the
  // WebSocket object to the caller in Javascript immediately. We will defer the connection logic
  // to the `initConnection` method.
  WebSocket(kj::String url, Locality locality);

  // We initiate a `new WebSocket()` connection and set up a continuation that handles the
  // response once it's available. This includes assigning the native websocket and dispatching the
  // relevant `open`/`error` events.
  void initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket>);

  // Pumps messages from this WebSocket to `other`, and from `other` to this, making sure to
  // register pending events as appropriate. Used to implement FetchEvent.respondWith().
  //
  // Only one of this or accept() is allowed to be invoked.
  //
  // As an exception to the usual KJ convention, it is not necessary for the JavaScript `WebSocket`
  // object to be kept live while waiting for the promise returned by couple() to complete. Instead,
  // the promise takes direct ownership of the underlying KJ-native WebSocket (as well as `other`).
  kj::Promise<DeferredProxy<void>> couple(kj::Own<kj::WebSocket> other);

  // Extract the kj::WebSocket from this api::WebSocket (if applicable). The kj::WebSocket will be
  // owned elsewhere, but the api::WebSocket will retain a reference.
  kj::Own<kj::WebSocket> acceptAsHibernatable(kj::Array<kj::StringPtr> tags);

  void tryReleaseNative(jsg::Lock& js);

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

  // Can only be called on one end of a WebSocketPair.
  // Relevant for WebSocket Hibernation: `couple()` will only allow IoContext to
  // go away if the end returned in the Response is REMOTE.
  void setRemoteOnPair();

  // Should only be called on one end of a WebSocketPair.
  // Relevant for WebSocket Hibernation: the end we return in the Response must be in the
  // AwaitingAcceptanceOrCoupling state.
  bool pairIsAwaitingCoupling();

  HibernationPackage buildPackageForHibernation();

  // ---------------------------------------------------------------------------
  // JS API.

  // Creates a new outbound WebSocket.
  static jsg::Ref<WebSocket> constructor(jsg::Lock& js, kj::String url,
      jsg::Optional<kj::OneOf<kj::Array<kj::String>, kj::String>> protocols);

  // Begin delivering events locally.
  void accept(jsg::Lock& js);

  // Same as accept(), but websockets that are created with `new WebSocket()` in JS cannot call
  // accept(). Instead, we only permit the C++ constructor to call this "internal" version of accept()
  // so that the websocket can start processing messages once the connection has been established.
  void internalAccept(jsg::Lock& js);

  // We defer the actual logic of accept() and internalAccept() to this method, since they largely
  // share code.
  void startReadLoop(jsg::Lock& js);

  void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message);
  void close(jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<kj::String> reason);

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

  // For internal use only.
  // We need to access the underlying KJ WebSocket so we can determine the compression configuration
  // it uses (if any).
  kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx);

  kj::Maybe<kj::StringPtr> getUrl();
  kj::Maybe<kj::StringPtr> getProtocol();
  kj::Maybe<kj::StringPtr> getExtensions();

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
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(readyState, getReadyState);
      JSG_READONLY_INSTANCE_PROPERTY(url, getUrl);
      JSG_READONLY_INSTANCE_PROPERTY(protocol, getProtocol);
      JSG_READONLY_INSTANCE_PROPERTY(extensions, getExtensions);
    }

    JSG_TS_DEFINE(type WebSocketEventMap = {
      close: CloseEvent;
      message: MessageEvent;
      open: Event;
      error: ErrorEvent;
    });
    JSG_TS_OVERRIDE(extends EventTarget<WebSocketEventMap>);
  }

private:
  kj::Maybe<kj::String> url;
  kj::Maybe<kj::String> protocol = kj::String();
  kj::Maybe<kj::String> extensions = kj::String();
  kj::Maybe<kj::Date> autoResponseTimestamp;
  // All WebSockets have this property. It starts out null but can
  // be assigned to any serializable value. The property will survive hibernation.
  // We have to serialize each time we call the setter so we can determine if the size limit
  // has been breached.
  kj::Maybe<kj::Array<byte>> serializedAttachment;

  // Tracks farNative->closedOutgoing, but we need to access it when we trigger Hibernation so it
  // cannot be `IoOwn`ed as `farNative` is. This informs the HibernatableWebSocket if we called
  // `close()`, thereby preventing calls to `send()` even after we wake from hibernation.
  bool closedOutgoingForHib = false;

  // Maximum size of a WebSocket attachment.
  inline static const size_t MAX_ATTACHMENT_SIZE = 1024 * 2;

  struct AwaitingConnection {
    // A canceler associated with the pending websocket connection for `new Websocket()`.
    kj::Canceler canceler;
  };
  struct AwaitingAcceptanceOrCoupling {
    explicit AwaitingAcceptanceOrCoupling(kj::Own<kj::WebSocket> ws): ws(kj::mv(ws)) {}
    kj::Own<kj::WebSocket> ws;
  };
  struct Accepted {
    // A `Hibernatable` WebSocket shares a sub-set of behavior that's already implemented for an
    // `Accepted` WebSocket, so we can think of it a sub-state.
    struct Hibernatable {
      kj::WebSocket& ws;
      // If we have initiated a hibernatable error/close event, we need to take back ownership of
      // the kj::WebSocket so any final queued messages will deliver. We store this owned websocket
      // in `attachedForClose`. Since the `ws` reference is still valid, we prevent usage of
      // `attachedForClose` directly in favor of using continuing to use `ws` directly.
      kj::Maybe<kj::Own<void>> attachedForClose;

      // We can't move the state to Released after the Hibernatable Close/Error event runs, since
      // we don't have a request on the thread by the time the event completes.
      //
      // If we are "releasing", we may prevent the websocket from doing certain things like calling
      // send/close. We're more restrictive if we're delivering an Error than delivering a Close.
      HibernatableReleaseState releaseState = HibernatableReleaseState::NONE;

      // There are two possible states for tagsRef:
      //  1. kj::Array<kj::StringPtr>
      //      - Tags are owned by the HibernationManager, we just reference them to save memory.
      //  2. kj::Array<kj::String>
      //      - We're going to be dispatching a Close or an Error event, i.e. the
      //        HibernatableWebSocket is free to go away. We can no longer rely on tags stored in
      //        the HibernationManager, so instead we copy the data into the api::WebSocket.
      //
      // We could just copy all tags into api::WebSocket everytime we reactivate/wake from
      // hibernation, but it could add up to 2.56KB of memory for each websocket.
      // With a maximum of 32k websockets, that could put a lot of memory pressure on the DO.
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

      // Transitions our Hibernatable websocket to a "Releasing" state.
      // The websocket will transition to `Released` when convenient.
      void initiateHibernatableRelease(jsg::Lock& js,
          kj::Own<kj::WebSocket> ws,
          kj::Array<kj::String> tags,
          HibernatableReleaseState state);

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

    // This canceler wraps the pump loop as a precaution to make sure we can't exit the Accepted
    // state with a pump task still happening asychronously. In practice the canceler should usually
    // be empty when destroyed because we do not leave the Accepted state if we're still pumping.
    // Even in the case of IoContext premature cancellation, the pump task should be canceled
    // by the IoContext before the Canceler is destroyed.
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

    // Has a Close message been enqueued for send? (It may still be in outgoingMessages. Check
    // closedOutgoing && !isPumping to check if it has gone out.)
    bool closedOutgoing = false;

    // Has a Close message been received, or has a premature disconnection occurred?
    bool closedIncoming = false;

    // Have we detected that the peer has stopped accepting messages? We may want to clean up more
    // proactively in this case.
    bool outgoingAborted = false;
  };

  // The underlying native WebSocket (or a promise that will emplace one).
  //
  // The state transitions look like so:
  // - Starts as `AwaitingConnection` if the `WebSocket(url, locality, ...)` ctor is used.
  // - Starts as `AwaitingAcceptanceOrCoupling` if the `WebSocket(native, locality)` ctor is used.
  // - Transitions from `AwaitingConnection` to `AwaitingAcceptanceOrCoupling` when the native
  //   connection is established and to `Accepted` once the read loop starts.
  // - Transitions from `AwaitingConnection` to `Released` when connection establishment fails.
  // - Transitions from `AwaitingAcceptanceOrCoupling` to `Accepted` when it is accepted.
  // - Transitions from `AwaitingAcceptanceOrCoupling` to `Released` when it is coupled to another
  //   web socket.
  // - Transitions from `Accepted` to `Released` when outgoing pump is done and either both
  //   directions have seen "close" messages or an error has occured.
  IoOwn<Native> farNative;

  // If any error has occurred.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> error;

  struct GatedMessage {
    kj::Maybe<kj::Promise<void>> outputLock;  // must wait for this before actually sending
    kj::WebSocket::Message message;
    size_t pendingAutoResponses = 0;
  };
  using OutgoingMessagesMap = kj::Table<GatedMessage, kj::InsertionOrderIndex>;
  // Queue of messages to be sent. This is wraped in a IoOwn so that the pump loop can safely
  // access the map without locking the isolate.
  IoOwn<OutgoingMessagesMap> outgoingMessages;

  // Keep track of current hibernatable websockets auto-response status to avoid racing
  // between regular websocket messages, and auto-responses.
  struct AutoResponse {
    kj::Promise<void> ongoingAutoResponse = kj::READY_NOW;
    std::deque<kj::String> pendingAutoResponseDeque;
    size_t queuedAutoResponses = 0;
    bool isPumping = false;
    bool isClosed = false;
  };

  AutoResponse autoResponseStatus;

  Locality locality;

  // Contains a websocket and possibly some data from the WebSocketResponse headers.
  struct PackedWebSocket {
    kj::Own<kj::WebSocket> ws;
    kj::Maybe<kj::String> proto;
    kj::Maybe<kj::String> extensions;
  };

  // If we created this WebSocket inside a critical section (ex. a blockConcurrencyWhile callback)
  // then we need to get the InputGate::Lock and pass it to context.run() when delivering events.
  kj::Maybe<InputGate::CriticalSection&> maybeCriticalSection;

  // So that each end of a WebSocketPair can keep track of its pair.
  kj::Maybe<jsg::Ref<WebSocket>> maybePair;

  void setMaybePair(jsg::Ref<WebSocket> other);

  friend jsg::Ref<WebSocketPair> WebSocketPair::constructor();

  void dispatchOpen(jsg::Lock& js);

  void ensurePumping(jsg::Lock& js);

  // Write messages from `outgoingMessages` into `ws`.
  //
  // These are not necessarily called under isolate lock, but they are called on the given
  // context's thread. They are declared `static` to prove they don't access the JavaScript
  // object's members in a thread-unsafe way. `outgoingMessages` and `ws` are both `IoOwn`ed
  // objects so are safe to access from the thread without the isolate lock. The whole task is
  // owned by the `IoContext` so it'll be canceled if the `IoContext` is destroyed.
  static kj::Promise<void> pump(
      IoContext& context, OutgoingMessagesMap& outgoingMessages, kj::WebSocket& ws, Native& native,
      AutoResponse& autoResponse);

  kj::Promise<kj::Maybe<kj::Exception>> readLoop();

  void reportError(jsg::Lock& js, kj::Exception&& e);
  void reportError(jsg::Lock& js, jsg::JsRef<jsg::JsValue> err);

  void assertNoError(jsg::Lock& js);
};

#define EW_WEBSOCKET_ISOLATE_TYPES \
  api::CloseEvent,                 \
  api::CloseEvent::Initializer,    \
  api::MessageEvent,               \
  api::MessageEvent::Initializer,  \
  api::ErrorEvent,                 \
  api::WebSocket,                  \
  api::WebSocketPair
// The list of websocket.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
