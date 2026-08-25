#pragma once

#include "basics.h"

#include <workerd/api/messagechannel.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

class Blob;

class MessageEvent final: public Event {
 public:
  MessageEvent(jsg::Lock& js,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      jsg::JsRef<jsg::JsValue> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none,
      Trusted trusted = Trusted::NO);

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
    jsg::Optional<jsg::Ref<MessagePort>> source;
    jsg::Optional<kj::Array<jsg::Ref<MessagePort>>> ports;

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
  kj::Maybe<jsg::Ref<MessagePort>> getSource();

  kj::Array<jsg::Ref<MessagePort>> getPorts();

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
  kj::Maybe<jsg::Ref<MessagePort>> maybeSource;
  kj::Maybe<kj::String> maybeOrigin;

  // The runtime never attaches ports (we do not support transferring MessagePorts);
  // user-constructed events reflect the ports passed in their init.
  kj::Array<jsg::Ref<MessagePort>> ports;

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
  api::ErrorEvent, api::ErrorEvent::ErrorEventInit, api::MessageEvent,                             \
      api::MessageEvent::Initializer, api::PromiseRejectionEvent, api::OpenEvent

}  // namespace workerd::api
