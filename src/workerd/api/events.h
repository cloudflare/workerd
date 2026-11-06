#pragma once

#include "basics.h"

#include <workerd/api/messagechannel.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

class ActorState;
class Blob;

namespace messageevent {

// A MessagePort as a MessageEvent holds one: either the C++ api::MessagePort, or an instance of
// the per-isolate bootstrap's TypeScript implementation, whose constructor marks it with the
// private brand BRAND (`utils.setPrivate(port, BRAND, true)`). The class is named after the type
// users know, and lives in a namespace of its own, so that JSG's conversion errors name the type
// as before.
class MessagePort {
 public:
  static constexpr kj::StringPtr BRAND = "workerd:MessagePort"_kj;

  using JsgRttiDelegate = jsg::Ref<api::MessagePort>;

  explicit MessagePort(jsg::Ref<api::MessagePort> port): port(kj::mv(port)) {}

  // Returns kj::none unless `value` is a MessagePort of either implementation. Runs no JS.
  static kj::Maybe<MessagePort> tryFrom(jsg::Lock& js, jsg::JsValue value);

  MessagePort addRef(jsg::Lock& js);
  jsg::JsValue getHandle(jsg::Lock& js);

  static v8::Local<v8::Value> jsgWrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      MessagePort value) {
    return value.getHandle(js);
  }
  static kj::Maybe<MessagePort> jsgTryUnwrap(auto& typeWrapper,
      jsg::Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return tryFrom(js, jsg::JsValue(handle));
  }

  void visitForGc(jsg::GcVisitor& visitor);

  JSG_MEMORY_INFO(MessagePort) {
    KJ_SWITCH_ONEOF(port) {
      KJ_CASE_ONEOF(ref, jsg::Ref<api::MessagePort>) {
        tracker.trackField("port", ref);
      }
      KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
        tracker.trackField("port", object);
      }
    }
  }

 private:
  explicit MessagePort(jsg::JsRef<jsg::JsObject> object): port(kj::mv(object)) {}

  kj::OneOf<jsg::Ref<api::MessagePort>, jsg::JsRef<jsg::JsObject>> port;
};

}  // namespace messageevent

class ExtendableEvent: public Event {
 public:
  // Runtime-only (the JS constructor is deleted); always trusted.
  explicit ExtendableEvent(kj::StringPtr type): Event(type, {}, Trusted::YES) {}

  // While ExtendableEvent is defined by the spec to be constructable, there's really not a
  // lot of reason currently to do so, especially with the restriction that waitUntil can
  // only be called on trusted events (which have to originate from within the system).
  static jsg::Ref<ExtendableEvent> constructor(kj::String type) = delete;

  void waitUntil(kj::Promise<void> promise);

  jsg::Optional<jsg::Ref<ActorState>> getActorState(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ExtendableEvent) {
    JSG_INHERIT(Event);
    JSG_METHOD(waitUntil);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(actorState, getActorState);

    JSG_TS_OVERRIDE({ actorState: never });
    // Omit `actorState` from definitions
  }
};

// An implementation of the Web Platform Standard CustomEvent API
class CustomEvent: public Event {
 public:
  struct CustomEventInit final {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    jsg::Optional<jsg::JsRef<jsg::JsValue>> detail;
    JSG_STRUCT(bubbles, cancelable, composed, detail);

    operator Event::Init();
  };

  explicit CustomEvent(kj::String ownType, CustomEventInit init = CustomEventInit());

  static jsg::Ref<CustomEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<CustomEventInit> init);

  jsg::Optional<jsg::JsValue> getDetail(jsg::Lock& js);

  JSG_RESOURCE_TYPE(CustomEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_PROTOTYPE_PROPERTY(detail, getDetail);
    JSG_TS_OVERRIDE(<T = any> {
      get detail(): T;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  jsg::Optional<jsg::JsRef<jsg::JsValue>> detail;

  void visitForGc(jsg::GcVisitor& visitor);
};

class MessageEvent final: public Event {
 public:
  MessageEvent(jsg::Lock& js,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<messageevent::MessagePort> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      jsg::JsRef<jsg::JsValue> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<messageevent::MessagePort> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<messageevent::MessagePort> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<messageevent::MessagePort> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  // An origin already serialized, or none for a null origin. A distinct type, so that kj::none
  // passed for the constructors' urlForOrigin is not ambiguous.
  struct SerializedOrigin {
    kj::Maybe<kj::String> origin;
  };

  // A trusted event as the runtime fires one, from an implementation that keeps its origin
  // serialized (the bootstrap's EventSource).
  MessageEvent(jsg::Lock& js,
      kj::String type,
      const jsg::JsValue& data,
      kj::String lastEventId,
      kj::Maybe<messageevent::MessagePort> source,
      SerializedOrigin origin);

  // The spec's MessageEventInit dictionary. Only `data` is ever meaningful to the runtime
  // itself; the remaining members exist so that user-constructed events reflect the
  // standard surface.
  struct Initializer {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    jsg::Optional<jsg::JsRef<jsg::JsValue>> data;
    jsg::Optional<jsg::USVString> origin;
    jsg::Optional<kj::String> lastEventId;
    jsg::Optional<kj::Maybe<messageevent::MessagePort>> source;
    jsg::Optional<kj::Array<messageevent::MessagePort>> ports;

    JSG_STRUCT(bubbles, cancelable, composed, data, origin, lastEventId, source, ports);
    JSG_STRUCT_TS_OVERRIDE(MessageEventInit {
      data?: any;
    });
  };

  // For user-constructed events (the JS constructor path).
  MessageEvent(jsg::Lock& js, kj::String type, Initializer initializer);

  static jsg::Ref<MessageEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<Initializer> initializer);

  kj::OneOf<jsg::JsValue, jsg::Ref<Blob>> getData(jsg::Lock& js);

  kj::Maybe<kj::StringPtr> getOrigin();

  kj::StringPtr getLastEventId();

  // Per the spec, the source of a MessageEvent is one of a MessagePort,
  // ServiceWorker, WindowProxy, etc. The only one of these we actually
  // support is MessagePort, return that if its set or null if not.
  kj::Maybe<messageevent::MessagePort> getSource(jsg::Lock& js);

  kj::Array<messageevent::MessagePort> getPorts(jsg::Lock& js);

  JSG_RESOURCE_TYPE(MessageEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(data, getData);
    JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
    JSG_READONLY_INSTANCE_PROPERTY(lastEventId, getLastEventId);
    JSG_READONLY_INSTANCE_PROPERTY(source, getSource);
    JSG_READONLY_INSTANCE_PROPERTY(ports, getPorts);

    JSG_TS_ROOT();
    JSG_TS_OVERRIDE({ readonly data: any; });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  // Blob is used only by web-socket.h/c++
  kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data;
  kj::String lastEventId;
  kj::Maybe<messageevent::MessagePort> maybeSource;
  kj::Maybe<kj::String> maybeOrigin;

  // The runtime never attaches ports (we do not support transferring MessagePorts);
  // user-constructed events reflect the ports passed in their init.
  kj::Array<messageevent::MessagePort> ports;

  void visitForGc(jsg::GcVisitor& visitor);
};

class OpenEvent final: public Event {
 public:
  OpenEvent();
  static jsg::Ref<OpenEvent> constructor() = delete;
  JSG_RESOURCE_TYPE(OpenEvent) {
    JSG_INHERIT(Event);
  }
};

class ErrorEvent final: public Event {
 public:
  struct ErrorEventInit {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    jsg::Optional<kj::String> message;
    jsg::Optional<kj::String> filename;
    jsg::Optional<int32_t> lineno;
    jsg::Optional<int32_t> colno;
    jsg::Optional<jsg::JsRef<jsg::JsValue>> error;
    JSG_STRUCT(bubbles, cancelable, composed, message, filename, lineno, colno, error);
  };

  ErrorEvent(ErrorEventInit init);
  ErrorEvent(kj::String type, ErrorEventInit init);
  ErrorEvent(jsg::Lock& js, jsg::JsValue error);

  static jsg::Ref<ErrorEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<ErrorEventInit> init);

  kj::StringPtr getFilename();
  kj::StringPtr getMessage();
  int getLineno();
  int getColno();
  jsg::JsValue getError(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ErrorEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_PROTOTYPE_PROPERTY(filename, getFilename);
    JSG_READONLY_PROTOTYPE_PROPERTY(message, getMessage);
    JSG_READONLY_PROTOTYPE_PROPERTY(lineno, getLineno);
    JSG_READONLY_PROTOTYPE_PROPERTY(colno, getColno);
    JSG_READONLY_PROTOTYPE_PROPERTY(error, getError);

    JSG_TS_ROOT();
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  ErrorEventInit init;

  void visitForGc(jsg::GcVisitor& visitor);
};

class CloseEvent: public Event {
 public:
  // Runtime-only (the JS constructor uses the (type, ...) overload); always trusted.
  CloseEvent(uint code, kj::String reason, bool clean);
  CloseEvent(kj::String type, int code, kj::String reason, bool clean, Init init = {});

  struct Initializer {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    jsg::Optional<int> code;
    jsg::Optional<jsg::USVString> reason;
    jsg::Optional<bool> wasClean;

    JSG_STRUCT(bubbles, cancelable, composed, code, reason, wasClean);
    JSG_STRUCT_TS_OVERRIDE(CloseEventInit);
  };
  static jsg::Ref<CloseEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<Initializer> initializer);

  int getCode();
  kj::StringPtr getReason();
  bool getWasClean();

  JSG_RESOURCE_TYPE(CloseEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(code, getCode);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
    JSG_READONLY_INSTANCE_PROPERTY(wasClean, getWasClean);

    JSG_TS_ROOT();
    // CloseEvent will be referenced from the `WebSocketEventMap` define
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  int code;
  kj::String reason;
  bool clean;
};

// ======================================================================================
class PromiseRejectionEvent final: public Event {
 public:
  PromiseRejectionEvent(
      v8::PromiseRejectEvent type, jsg::V8Ref<v8::Promise> promise, jsg::Value reason);

  static jsg::Ref<PromiseRejectionEvent> constructor(kj::String type) = delete;

  jsg::V8Ref<v8::Promise> getPromise(jsg::Lock& js) {
    return promise.addRef(js);
  }
  jsg::Value getReason(jsg::Lock& js) {
    return reason.addRef(js);
  }

  JSG_RESOURCE_TYPE(PromiseRejectionEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_INSTANCE_PROPERTY(promise, getPromise);
    JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("promise", promise);
    tracker.trackField("reason", reason);
  }

 private:
  jsg::V8Ref<v8::Promise> promise;
  jsg::Value reason;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(promise, reason);
  }
};

#define EW_EVENTS_ISOLATE_TYPES                                                                    \
  api::ExtendableEvent, api::CustomEvent, api::CustomEvent::CustomEventInit, api::ErrorEvent,      \
      api::ErrorEvent::ErrorEventInit, api::MessageEvent, api::MessageEvent::Initializer,          \
      api::PromiseRejectionEvent, api::OpenEvent, api::CloseEvent, api::CloseEvent::Initializer

}  // namespace workerd::api
