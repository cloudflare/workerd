#include "events.h"

namespace workerd::api {

ErrorEvent::ErrorEvent(kj::String type, ErrorEventInit init)
    : Event(kj::mv(type)),
      init(kj::mv(init)) {}

jsg::Ref<ErrorEvent> ErrorEvent::constructor(
    jsg::Lock& js, kj::String type, jsg::Optional<ErrorEventInit> init) {
  return js.alloc<ErrorEvent>(kj::mv(type), kj::mv(init).orDefault({}));
}

kj::StringPtr ErrorEvent::getFilename() {
  return init.filename.orDefault(nullptr);
}

kj::StringPtr ErrorEvent::getMessage() {
  return init.message.orDefault(nullptr);
}

int ErrorEvent::getLineno() {
  return init.lineno.orDefault(0);
}

int ErrorEvent::getColno() {
  return init.colno.orDefault(0);
}

jsg::JsValue ErrorEvent::getError(jsg::Lock& js) {
  KJ_IF_SOME(error, init.error) {
    return error.getHandle(js);
  } else {
    return js.undefined();
  }
}

void ErrorEvent::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("message", init.message);
  tracker.trackField("filename", init.filename);
  tracker.trackField("error", init.error);
}

void ErrorEvent::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(init.error);
}

}  // namespace workerd::api
