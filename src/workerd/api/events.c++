#include "events.h"

#include "blob.h"
#include "messagechannel.h"

namespace workerd::api {

namespace {
constexpr kj::StringPtr kDefaultErrorEventName = "error"_kj;
constexpr kj::StringPtr kMessageEventName = "message"_kj;
constexpr kj::StringPtr kOpenEventName = "open"_kj;
constexpr kj::StringPtr kRejectionHandledEventName = "rejectionhandled"_kj;
constexpr kj::StringPtr kUnhandledRejectionEventName = "unhandledrejection"_kj;
}  // namespace

OpenEvent::OpenEvent(): Event(kOpenEventName) {}

MessageEvent::MessageEvent(jsg::Lock& js,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<kj::Array<const char>> origin,
    Event::Init init)
    : Event(kMessageEventName, kj::mv(init)),
      data(jsg::JsRef(js, data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(kj::mv(origin)) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<kj::Array<const char>> origin,
    Event::Init init)
    : Event(kMessageEventName, kj::mv(init)),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(kj::mv(origin)) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<kj::Array<const char>> origin,
    Event::Init init)
    : Event(kj::mv(type), kj::mv(init)),
      data(jsg::JsRef(js, kj::mv(data))),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(kj::mv(origin)) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data,
    kj::String lastEventId,
    kj::Maybe<jsg::Ref<MessagePort>> source,
    kj::Maybe<kj::Array<const char>> origin,
    Event::Init init)
    : Event(kj::mv(type), kj::mv(init)),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(kj::mv(origin)) {}

jsg::Ref<MessageEvent> MessageEvent::constructor(
    jsg::Lock& js, kj::String type, jsg::Optional<Initializer> maybeInitializer) {
  Initializer initializer = kj::mv(maybeInitializer).orDefault({});

  // Per the spec, MessageEventInit's data member defaults to null.
  kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data =
      jsg::JsRef<jsg::JsValue>(js, js.null());
  KJ_IF_SOME(d, initializer.data) {
    data = kj::mv(d);
  }

  // A MessageEvent's origin is "an origin, a string, or null". The origin getter only
  // serializes when it holds an actual origin; a string is returned as given. The
  // initializer's origin is a USVString, so it is stored verbatim and never parsed.
  kj::Maybe<kj::Array<const char>> origin;
  KJ_IF_SOME(o, initializer.origin) {
    origin = kj::Array<const char>(kj::heapArray<char>(o.asPtr()));
  }

  return js.alloc<MessageEvent>(js, kj::mv(type), kj::mv(data),
      kj::mv(initializer.lastEventId).orDefault(kj::String()), kj::mv(initializer.source),
      kj::mv(origin),
      Event::Init{
        .bubbles = initializer.bubbles,
        .cancelable = initializer.cancelable,
        .composed = initializer.composed,
      });
}

kj::OneOf<jsg::JsValue, jsg::Ref<Blob>> MessageEvent::getData(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(jsValue, jsg::JsRef<jsg::JsValue>) {
      return jsValue.getHandle(js);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return blob.addRef();
    }
  }
  KJ_UNREACHABLE;
}

kj::ArrayPtr<const char> MessageEvent::getOrigin() {
  // Per the spec, an origin that was never set is reported as the empty string, not null.
  return maybeOrigin.map([](auto& a) -> kj::ArrayPtr<const char> {
    return a.asPtr();
  }).orDefault(nullptr);
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
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(jsValue, jsg::JsRef<jsg::JsValue>) {
      tracker.trackField("data", jsValue);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      tracker.trackField("data", blob);
    }
  }
  tracker.trackField("source", maybeSource);
  tracker.trackField("lastEventId", lastEventId);
  tracker.trackField("origin", maybeOrigin);
}

void MessageEvent::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(jsValue, jsg::JsRef<jsg::JsValue>) {
      visitor.visit(jsValue);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      visitor.visit(blob);
    }
  }
  visitor.visit(maybeSource);
}

// ======================================================================================
ErrorEvent::ErrorEvent(ErrorEventInit init): Event(kDefaultErrorEventName), init(kj::mv(init)) {}

ErrorEvent::ErrorEvent(kj::String type, ErrorEventInit init)
    : Event(kj::mv(type)),
      init(kj::mv(init)) {}

ErrorEvent::ErrorEvent(jsg::Lock& js, jsg::JsValue error)
    : ErrorEvent(ErrorEventInit{.error = jsg::JsRef(js, error)}) {}

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

// ======================================================================================
namespace {
constexpr kj::StringPtr getPromiseRejectionEventName(v8::PromiseRejectEvent type) {
  switch (type) {
    case v8::PromiseRejectEvent::kPromiseRejectWithNoHandler:
      return kUnhandledRejectionEventName;
    case v8::PromiseRejectEvent::kPromiseHandlerAddedAfterReject:
      return kRejectionHandledEventName;
    default:
      // Events are not emitted for the other reject types.
      KJ_UNREACHABLE;
  }
}

}  // namespace

PromiseRejectionEvent::PromiseRejectionEvent(
    v8::PromiseRejectEvent type, jsg::V8Ref<v8::Promise> promise, jsg::Value reason)
    : Event(getPromiseRejectionEventName(type)),
      promise(kj::mv(promise)),
      reason(kj::mv(reason)) {}

}  // namespace workerd::api
