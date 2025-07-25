#include "events.h"

#include "messagechannel.h"

namespace workerd::api {

MessageEvent::MessageEvent(jsg::Lock& js,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<jsg::Url&> urlForOrigin)
    : Event("message"),
      data(jsg::JsRef(js, data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return url.getOrigin(); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<jsg::Url&> urlForOrigin)
    : Event("message"),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return url.getOrigin(); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<jsg::Url&> urlForOrigin)
    : Event(kj::mv(type)),
      data(jsg::JsRef(js, kj::mv(data))),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return url.getOrigin(); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<jsg::Url&> urlForOrigin)
    : Event(kj::mv(type)),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return url.getOrigin(); })) {}

jsg::Ref<MessageEvent> MessageEvent::constructor(
    jsg::Lock& js, kj::String type, Initializer initializer) {
  return js.alloc<MessageEvent>(js, kj::mv(type), kj::mv(initializer.data));
}

jsg::JsValue MessageEvent::getData(jsg::Lock& js) {
  return data.getHandle(js);
}

kj::Maybe<kj::ArrayPtr<const char>> MessageEvent::getOrigin() {
  return maybeOrigin.map([](auto& a) -> kj::ArrayPtr<const char> { return a.asPtr(); });
}

kj::StringPtr MessageEvent::getLastEventId() {
  return lastEventId;
}

// Per the spec, the source of a MessageEvent is one of a MessagePort,
// ServiceWorker, WindowProxy, etc. The only one of these we actually
// support is MessagePort, return that if its set or null if not.
kj::Maybe<jsg::Ref<MessagePort>> MessageEvent::getSource() {
  return maybeSource.map([](auto& port) mutable -> jsg::Ref<MessagePort> { return port.addRef(); });
}
kj::ArrayPtr<jsg::Ref<MessagePort>> MessageEvent::getPorts() {
  // We don't support transferring MessagePorts in MessageEvent
  // for now, so we return an empty array. Later we might support
  // this.
  return nullptr;
}

void MessageEvent::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("data", data);
  tracker.trackField("source", maybeSource);
}

void MessageEvent::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(data);
  visitor.visit(maybeSource);
}

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
