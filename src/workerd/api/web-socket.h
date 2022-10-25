// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/string.h>
#include <kj/compat/http.h>
#include "basics.h"
#include <workerd/io/io-context.h>

namespace workerd::api {

class MessageEvent: public Event {
public:
  MessageEvent(v8::Isolate* isolate, v8::Local<v8::Value> data)
      : Event("message"), data(isolate, data) {}
  MessageEvent(kj::String type, v8::Isolate* isolate, v8::Local<v8::Value> data)
      : Event(kj::mv(type)), data(isolate, data) {}

  struct Initializer {
    v8::Local<v8::Value> data;

    JSG_STRUCT(data);
    JSG_STRUCT_TS_OVERRIDE(MessageEventInit {
      data: ArrayBuffer | string;
    });
  };
  static jsg::Ref<MessageEvent> constructor(
      kj::String type, Initializer initializer, v8::Isolate* isolate) {
    return jsg::alloc<MessageEvent>(kj::mv(type), isolate, initializer.data);
  }

  v8::Local<v8::Value> getData(v8::Isolate* isolate) { return data.getHandle(isolate); }

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
  jsg::Value data;
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
  ErrorEvent(kj::String&& message, jsg::Value error, v8::Isolate* isolate)
      : Event("error"), message(kj::mv(message)), error(kj::mv(error)), isolate(isolate){}

  static jsg::Ref<ErrorEvent> constructor() = delete;

  // Due to the context in which we use this ErrorEvent class (internal errors), the getters for
  // filename, lineNo, and colNo are all falsy.
  kj::String getFilename() { return nullptr; }
  kj::StringPtr getMessage() { return message; }
  int getLineno() { return 0; }
  int getColno() { return 0; }
  v8::Local<v8::Value> getError() { return error.getHandle(isolate); }


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
  jsg::Value error;
  v8::Isolate* isolate;

  void visitForGc(jsg::GcVisitor& visitor);
};

class WebSocket: public EventTarget {
private:
  struct PackedWebSocket;
public:
  enum Locality {
    LOCAL,
    // This is one end of a local WebSocketPair. Do not use IoContext::registerPendingEvent()
    // when waiting on this WebSocket.

    REMOTE
    // This is a remote WebSocket. Use IoContext::registerPendingEvent() when waiting.
  };

  // WebSocket ready states.
  static constexpr int READY_STATE_CONNECTING = 0;
  static constexpr int READY_STATE_OPEN = 1;
  static constexpr int READY_STATE_CLOSING = 2;
  static constexpr int READY_STATE_CLOSED = 3;

  WebSocket(kj::Own<kj::WebSocket> native, Locality locality);
  WebSocket(kj::String url, Locality locality);
  // The JS WebSocket constructor needs to initiate a connection, but we need to return the
  // WebSocket object to the caller in Javascript immediately. We will defer the connection logic
  // to the `initConnection` method.

  void initConnection(jsg::Lock& js, kj::Promise<PackedWebSocket>);
  // We initiate a `new WebSocket()` connection and set up a continuation that handles the
  // response once it's available. This includes assigning the native websocket and dispatching the
  // relevant `open`/`error` events.

  kj::Promise<DeferredProxy<void>> couple(kj::Own<kj::WebSocket> other);
  // Pumps messages from this WebSocket to `other`, and from `other` to this, making sure to
  // register pending events as appropriate. Used to implement FetchEvent.respondWith().
  //
  // Only one of this or accept() is allowed to be invoked.
  //
  // As an exception to the usual KJ convention, it is not necessary for the JavaScript `WebSocket`
  // object to be kept live while waiting for the promise returned by couple() to complete. Instead,
  // the promise takes direct ownership of the underlying KJ-native WebSocket (as well as `other`).

  // ---------------------------------------------------------------------------
  // JS API.

  static jsg::Ref<WebSocket> constructor(jsg::Lock& js, kj::String url,
      jsg::Optional<kj::OneOf<kj::Array<kj::String>, kj::String>> protocols,
      CompatibilityFlags::Reader flags);
  // Creates a new outbound WebSocket.

  void accept(jsg::Lock& js);
  // Begin delivering events locally.

  void internalAccept(jsg::Lock& js);
  // Same as accept(), but websockets that are created with `new WebSocket()` in JS cannot call
  // accept(). Instead, we only permit the C++ constructor to call this "internal" version of accept()
  // so that the websocket can start processing messages once the connection has been established.

  void startReadLoop(jsg::Lock& js);
  // We defer the actual logic of accept() and internalAccept() to this method, since they largely
  // share code.

  void send(jsg::Lock& js, kj::OneOf<kj::Array<byte>, kj::String> message);
  void close(jsg::Lock& js, jsg::Optional<int> code, jsg::Optional<kj::String> reason);
  int getReadyState();

  bool isAccepted();
  bool isReleased();

  kj::Maybe<kj::String> getPreferredExtensions(kj::WebSocket::ExtensionsContext ctx);
  // For internal use only.
  // We need to access the underlying KJ WebSocket so we can determine the compression configuration
  // it uses (if any).


  kj::Maybe<kj::StringPtr> getUrl();
  kj::Maybe<kj::StringPtr> getProtocol();
  kj::Maybe<kj::StringPtr> getExtensions();

  JSG_RESOURCE_TYPE(WebSocket, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(EventTarget);
    JSG_METHOD(accept);
    JSG_METHOD(send);
    JSG_METHOD(close);

    JSG_STATIC_CONSTANT(READY_STATE_CONNECTING);
    JSG_STATIC_CONSTANT(READY_STATE_OPEN);
    JSG_STATIC_CONSTANT(READY_STATE_CLOSING);
    JSG_STATIC_CONSTANT(READY_STATE_CLOSED);

    // Previously, we were setting all properties as instance properties,
    // which broke the ability to subclass the Event object. With the
    // feature flag set, we instead attach the properties to the
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

  struct Native;

  struct AwaitingConnection {
    kj::Canceler canceler;
    // A canceler associated with the pending websocket connection for `new Websocket()`.
  };
  struct AwaitingAcceptanceOrCoupling {
    explicit AwaitingAcceptanceOrCoupling(kj::Own<kj::WebSocket> ws): ws(kj::mv(ws)) {}
    kj::Own<kj::WebSocket> ws;
  };
  struct Accepted {
    explicit Accepted(kj::Own<kj::WebSocket> ws, Native& native, IoContext& context);
    ~Accepted() noexcept(false);

    kj::Own<kj::WebSocket> ws;

    kj::Canceler canceler;
    // This canceler wraps the pump loop as a precaution to make sure we can't exit the Accepted
    // state with a pump task still happening asychronously. In practice the canceler should usually
    // be empty when destroyed because we do not leave the Accepted state if we're still pumping.
    // Even in the case of IoContext premature cancellation, the pump task should be canceled
    // by the IoContext before the Canceler is destroyed.

    kj::Promise<void> whenAbortedTask;
    // Listens for ws->whenAborted() and possibly triggers a proactive shutdown.

    kj::Maybe<kj::Own<ActorObserver>> actorMetrics;
  };
  struct Released {};

  struct Native {
    kj::OneOf<AwaitingConnection, AwaitingAcceptanceOrCoupling, Accepted, Released> state;

    bool isPumping = false;
    // Is there currently a task running to pump outgoing messages?

    bool closedOutgoing = false;
    // Has a Close message been enqueued for send? (It may still be in outgoingMessages. Check
    // closedOutgoing && !isPumping to check if it has gone out.)

    bool closedIncoming = false;
    // Has a Close message been received, or has a premature disconnection occurred?

    bool outgoingAborted = false;
    // Have we detected that the peer has stopped accepting messages? We may want to clean up more
    // proactively in this case.
  };
  IoOwn<Native> farNative;
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

  kj::Maybe<jsg::Value> error;
  // If any error has occurred.

  struct GatedMessage {
    kj::Maybe<kj::Promise<void>> outputLock;  // must wait for this before actually sending
    kj::WebSocket::Message message;
  };
  using OutgoingMessagesMap = kj::Table<GatedMessage, kj::InsertionOrderIndex>;
  IoOwn<OutgoingMessagesMap> outgoingMessages;
  // Queue of messages to be sent. This is wraped in a IoOwn so that the pump loop can safely
  // access the map without locking the isolate.

  Locality locality;

  // Contains a websocket and possibly some data from the WebSocketResponse headers.
  struct PackedWebSocket {
    kj::Own<kj::WebSocket> ws;
    kj::Maybe<kj::String> proto;
    kj::Maybe<kj::String> extensions;
  };

  void dispatchOpen(jsg::Lock& js);

  void ensurePumping(jsg::Lock& js);

  static kj::Promise<void> pump(
      IoContext& context, OutgoingMessagesMap& outgoingMessages, kj::WebSocket& ws);
  static kj::Promise<void> pumpAfterFrontOutputLock(
      IoContext& context, OutgoingMessagesMap& outgoingMessages, kj::WebSocket& ws);
  // Write messages from `outgoingMessages` into `ws`.
  //
  // These are not necessarily called under isolate lock, but they are called on the given
  // context's thread. They are declared `static` to prove they don't access the JavaScript
  // object's members in a thread-unsafe way. `outgoingMessages` and `ws` are both `IoOwn`ed
  // objects so are safe to access from the thread without the isolate lock. The whole task is
  // owned by the `IoContext` so it'll be canceled if the `IoContext` is destroyed.

  kj::Promise<void> readLoop(kj::WebSocket& ws);

  void reportError(jsg::Lock& js, kj::Exception&& e);
  void reportError(jsg::Lock& js, jsg::Value err);

  void assertNoError(jsg::Lock& js);
};

class WebSocketPair: public jsg::Object {
public:
  WebSocketPair(jsg::Ref<WebSocket> first, jsg::Ref<WebSocket> second)
      : sockets { kj::mv(first), kj::mv(second) } {}

  static jsg::Ref<WebSocketPair> constructor();

  jsg::Ref<WebSocket> getFirst() { return sockets[0].addRef(); }
  jsg::Ref<WebSocket> getSecond() { return sockets[1].addRef(); }

  JSG_RESOURCE_TYPE(WebSocketPair, CompatibilityFlags::Reader flags) {
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
