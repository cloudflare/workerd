#pragma once

#include "basics.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api {

class CloseEvent: public Event {
 public:
  CloseEvent(): Event("close") {}

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
    jsg::Optional<kj::String> reason;
    jsg::Optional<bool> wasClean;

    JSG_STRUCT(code, reason, wasClean);
    JSG_STRUCT_TS_OVERRIDE(CloseEventInit);
  };
  static jsg::Ref<CloseEvent> constructor(jsg::Optional<kj::String> type = kj::none,
      jsg::Optional<Initializer> initializer = kj::none) {
    KJ_IF_SOME(t, type) {
      Initializer init = kj::mv(initializer).orDefault({});
      return jsg::alloc<CloseEvent>(kj::mv(t), init.code.orDefault(0),
          kj::mv(init.reason).orDefault(nullptr), init.wasClean.orDefault(false));
    }

    return jsg::alloc<CloseEvent>();
  }

  int getCode() {
    return code;
  }
  kj::Maybe<kj::StringPtr> getReason() {
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
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("reason", reason);
  }

 private:
  kj::uint code{};
  kj::Maybe<kj::String> reason;
  bool clean{};
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

  ErrorEvent(kj::String type, ErrorEventInit init);

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

#define EW_EVENTS_ISOLATE_TYPES                                                                    \
  api::ErrorEvent, api::ErrorEvent::ErrorEventInit, api::CloseEvent, api::CloseEvent::Initializer

}  // namespace workerd::api
