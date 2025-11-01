#pragma once

#include "basics.h"

#include <workerd/api/messagechannel.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

namespace workerd::api {

class MessageEvent final: public Event {
 public:
  MessageEvent(jsg::Lock& js,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none);

  MessageEvent(jsg::Lock& js,
      jsg::JsRef<jsg::JsValue> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      const jsg::JsValue& data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none);

  MessageEvent(jsg::Lock& js,
      kj::String type,
      jsg::JsRef<jsg::JsValue> data,
      kj::String lastEventId = kj::String(),
      kj::Maybe<jsg::Ref<MessagePort>> source = kj::none,
      kj::Maybe<jsg::Url&> urlForOrigin = kj::none);

  struct Initializer {
    jsg::JsRef<jsg::JsValue> data;

    JSG_STRUCT(data);
    JSG_STRUCT_TS_OVERRIDE(MessageEventInit {
      data: ArrayBuffer | string;
    });
  };
  static jsg::Ref<MessageEvent> constructor(
      jsg::Lock& js, kj::String type, Initializer initializer);

  jsg::JsValue getData(jsg::Lock& js);

  kj::Maybe<kj::ArrayPtr<const char>> getOrigin();

  kj::StringPtr getLastEventId();

  // Per the spec, the source of a MessageEvent is one of a MessagePort,
  // ServiceWorker, WindowProxy, etc. The only one of these we actually
  // support is MessagePort, return that if its set or null if not.
  kj::Maybe<jsg::Ref<MessagePort>> getSource();

  kj::ArrayPtr<jsg::Ref<MessagePort>> getPorts();

  JSG_RESOURCE_TYPE(MessageEvent) {
    JSG_INHERIT(Event);

    JSG_READONLY_INSTANCE_PROPERTY(data, getData);
    JSG_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
    JSG_READONLY_INSTANCE_PROPERTY(lastEventId, getLastEventId);
    JSG_READONLY_INSTANCE_PROPERTY(source, getSource);
    JSG_READONLY_INSTANCE_PROPERTY(ports, getPorts);

    JSG_TS_ROOT();
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 private:
  jsg::JsRef<jsg::JsValue> data;
  kj::String lastEventId;
  kj::Maybe<jsg::Ref<MessagePort>> maybeSource;
  kj::Maybe<kj::Array<const char>> maybeOrigin;

  void visitForGc(jsg::GcVisitor& visitor);
};

class OpenEvent final: public Event {
 public:
  OpenEvent(): Event("open"_kjc) {}
  static jsg::Ref<OpenEvent> constructor() = delete;
  JSG_RESOURCE_TYPE(OpenEvent) {
    JSG_INHERIT(Event);
  }
};

class ErrorEvent: public Event {
 public:
  struct ErrorEventInit {
    jsg::Optional<kj::String> message;
    jsg::Optional<kj::String> filename;
    jsg::Optional<int32_t> lineno;
    jsg::Optional<int32_t> colno;
    jsg::Optional<jsg::JsRef<jsg::JsValue>> error;
    JSG_STRUCT(message, filename, lineno, colno, error);
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
class PromiseRejectionEvent: public Event {
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
