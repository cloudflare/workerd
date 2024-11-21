#pragma once

#include "basics.h"

#include <workerd/jsg/jsg.h>

namespace workerd::api {

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

#define EW_EVENTS_ISOLATE_TYPES api::ErrorEvent, api::ErrorEvent::ErrorEventInit

}  // namespace workerd::api
