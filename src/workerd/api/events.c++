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

namespace messageevent {

kj::Maybe<MessagePort> MessagePort::tryFrom(jsg::Lock& js, jsg::JsValue value) {
  KJ_IF_SOME(handler, js.tryGetTypeHandler<jsg::Ref<api::MessagePort>>()) {
    KJ_IF_SOME(port, handler.tryUnwrap(js, value)) {
      return MessagePort(kj::mv(port));
    }
  }
  KJ_IF_SOME(object, value.tryCast<jsg::JsObject>()) {
    // A private symbol is invisible to user code, so the brand cannot be copied onto an
    // impostor, and reading it runs no JS, even on a proxy.
    if (object.hasPrivate(js, BRAND)) {
      return MessagePort(object.addRef(js));
    }
  }
  return kj::none;
}

MessagePort MessagePort::addRef(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(port) {
    KJ_CASE_ONEOF(ref, jsg::Ref<api::MessagePort>) {
      return MessagePort(ref.addRef());
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return MessagePort(object.addRef(js));
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsValue MessagePort::getHandle(jsg::Lock& js) {
  KJ_SWITCH_ONEOF(port) {
    KJ_CASE_ONEOF(ref, jsg::Ref<api::MessagePort>) {
      // Only reachable once a C++ MessagePort exists, which requires the type to be registered.
      auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<api::MessagePort>>(),
          "MessagePort is missing from the isolate type list");
      return jsg::JsValue(handler.wrap(js, ref.addRef()));
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      return object.getHandle(js);
    }
  }
  KJ_UNREACHABLE;
}

void MessagePort::visitForGc(jsg::GcVisitor& visitor) {
  KJ_SWITCH_ONEOF(port) {
    KJ_CASE_ONEOF(ref, jsg::Ref<api::MessagePort>) {
      visitor.visit(ref);
    }
    KJ_CASE_ONEOF(object, jsg::JsRef<jsg::JsObject>) {
      visitor.visit(object);
    }
  }
}

}  // namespace messageevent

// Runtime-only; always trusted.
OpenEvent::OpenEvent(): Event(kOpenEventName, {}, Trusted::YES) {}

MessageEvent::MessageEvent(jsg::Lock& js,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<messageevent::MessagePort> source,
    kj::Maybe<jsg::Url&> urlForOrigin,
    Trusted trusted)
    : Event(kMessageEventName, {}, trusted),
      data(jsg::JsRef(js, data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return kj::str(url.getOrigin()); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    jsg::JsRef<jsg::JsValue> data,
    kj::String lastEventId,
    kj::Maybe<messageevent::MessagePort> source,
    kj::Maybe<jsg::Url&> urlForOrigin,
    Trusted trusted)
    : Event(kMessageEventName, {}, trusted),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return kj::str(url.getOrigin()); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    const jsg::JsValue& data,
    kj::String lastEventId,
    kj::Maybe<messageevent::MessagePort> source,
    kj::Maybe<jsg::Url&> urlForOrigin,
    Trusted trusted)
    : Event(kj::mv(type), {}, trusted),
      data(jsg::JsRef(js, kj::mv(data))),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return kj::str(url.getOrigin()); })) {}
MessageEvent::MessageEvent(jsg::Lock& js,
    kj::String type,
    kj::OneOf<jsg::JsRef<jsg::JsValue>, jsg::Ref<Blob>> data,
    kj::String lastEventId,
    kj::Maybe<messageevent::MessagePort> source,
    kj::Maybe<jsg::Url&> urlForOrigin,
    Trusted trusted)
    : Event(kj::mv(type), {}, trusted),
      data(kj::mv(data)),
      lastEventId(kj::mv(lastEventId)),
      maybeSource(kj::mv(source)),
      maybeOrigin(urlForOrigin.map([](auto& url) { return kj::str(url.getOrigin()); })) {}

MessageEvent::MessageEvent(jsg::Lock& js, kj::String type, Initializer initializer)
    : Event(kj::mv(type),
          Event::Init{
            .bubbles = initializer.bubbles,
            .cancelable = initializer.cancelable,
            .composed = initializer.composed,
          }),
      data(kj::mv(initializer.data).orDefault([&] { return jsg::JsRef(js, js.null()); })),
      lastEventId(kj::mv(initializer.lastEventId).orDefault(kj::String())),
      maybeSource(kj::mv(initializer.source).orDefault(kj::none)),
      // Per the spec, origin defaults to the empty string for user-constructed events.
      maybeOrigin(kj::mv(initializer.origin)
                      .map([](jsg::USVString&& origin) -> kj::String { return kj::mv(origin); })
                      .orDefault(kj::String())),
      ports(kj::mv(initializer.ports).orDefault([] {
        return kj::Array<messageevent::MessagePort>();
      })) {}

jsg::Ref<MessageEvent> MessageEvent::constructor(
    jsg::Lock& js, kj::String type, jsg::Optional<Initializer> initializer) {
  return js.alloc<MessageEvent>(js, kj::mv(type), kj::mv(initializer).orDefault({}));
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

kj::Maybe<kj::StringPtr> MessageEvent::getOrigin() {
  return maybeOrigin.map([](kj::String& origin) -> kj::StringPtr { return origin; });
}

kj::StringPtr MessageEvent::getLastEventId() {
  return lastEventId;
}

// Per the spec, the source of a MessageEvent is one of a MessagePort,
// ServiceWorker, WindowProxy, etc. The only one of these we actually
// support is MessagePort, return that if its set or null if not.
kj::Maybe<messageevent::MessagePort> MessageEvent::getSource(jsg::Lock& js) {
  KJ_IF_SOME(port, maybeSource) {
    return port.addRef(js);
  }
  return kj::none;
}
kj::Array<messageevent::MessagePort> MessageEvent::getPorts(jsg::Lock& js) {
  // The runtime never attaches ports (we don't support transferring MessagePorts), so this
  // is empty except for user-constructed events that passed ports in their init.
  return KJ_MAP(port, ports) { return port.addRef(js); };
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
  for (auto& port: ports) {
    tracker.trackField("port", port);
  }
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
  visitor.visitAll(ports);
}

// ======================================================================================
// Runtime-only (the JS constructor uses the (type, init) overload); always trusted.
ErrorEvent::ErrorEvent(ErrorEventInit init)
    : Event(kDefaultErrorEventName, {}, Trusted::YES),
      init(kj::mv(init)) {}

ErrorEvent::ErrorEvent(kj::String type, ErrorEventInit init)
    : Event(kj::mv(type),
          Event::Init{
            .bubbles = init.bubbles,
            .cancelable = init.cancelable,
            .composed = init.composed,
          }),
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
    // Runtime-only; always trusted.
    : Event(getPromiseRejectionEventName(type), {}, Trusted::YES),
      promise(kj::mv(promise)),
      reason(kj::mv(reason)) {}

}  // namespace workerd::api
