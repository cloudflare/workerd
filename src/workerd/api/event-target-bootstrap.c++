// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "event-target-bootstrap.h"

#include "basics.h"
#include "events.h"

namespace workerd::api {

namespace {

// EventTarget and Event are registered in EW_BASICS_ISOLATE_TYPES, so their handlers are always
// present. Asserting keeps the callers (raw bootstrap callbacks) free of a failure mode they
// could not act on.
const jsg::TypeHandler<jsg::Ref<EventTarget>>& eventTargetHandler(jsg::Lock& js) {
  return KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<EventTarget>>(),
      "EventTarget is missing from the isolate type list");
}

const jsg::TypeHandler<jsg::Ref<Event>>& eventHandler(jsg::Lock& js) {
  return KJ_ASSERT_NONNULL(
      js.tryGetTypeHandler<jsg::Ref<Event>>(), "Event is missing from the isolate type list");
}

jsg::Ref<EventTarget> requireEventTarget(
    jsg::Lock& js, jsg::JsValue target, kj::StringPtr functionName) {
  return JSG_REQUIRE_NONNULL(eventTargetHandler(js).tryUnwrap(js, target), TypeError, functionName,
      "() expects an EventTarget");
}

kj::String requireType(jsg::Lock& js, jsg::JsValue type, kj::StringPtr functionName) {
  auto str = JSG_REQUIRE_NONNULL(
      type.tryCast<jsg::JsString>(), TypeError, functionName, "() expects a string event type");
  return str.toString(js);
}

// The Function::Wrapper of a handler built from a JS function: converts as the HandlerFunction
// that JSG unwraps from a JS function does (an undefined result is no result).
jsg::Optional<jsg::Value> callJsEventHandler(jsg::Lock& js,
    v8::Local<v8::Value> receiver,
    v8::Local<v8::Function> fn,
    jsg::Ref<Event> event) {
  return js.withinHandleScope([&]() -> jsg::Optional<jsg::Value> {
    v8::Local<v8::Value> argv[] = {eventHandler(js).wrap(js, kj::mv(event))};
    auto result = jsg::check(fn->Call(js.v8Context(), receiver, kj::size(argv), argv));
    if (result->IsUndefined()) {
      return kj::none;
    }
    return jsg::Value(js.v8Isolate, result);
  });
}

}  // namespace

void EventTargetBootstrap::setListenerCountHook(
    jsg::Lock& js, jsg::JsValue target, jsg::JsValue type, jsg::JsValue hook) {
  static constexpr auto kName = "setListenerCountHook"_kjc;
  auto self = requireEventTarget(js, target, kName);
  auto typeStr = requireType(js, type, kName);
  auto fn = JSG_REQUIRE_NONNULL(
      hook.tryCast<jsg::JsFunction>(), TypeError, kName, "() expects a function");

  for (auto& entry: self->listenerCountHooks) {
    if (entry.type == typeStr) {
      entry.hook = fn.addRef(js);
      return;
    }
  }
  self->listenerCountHooks.add(EventTarget::ListenerCountHook{
    .type = kj::mv(typeStr),
    .hook = fn.addRef(js),
  });
}

jsg::JsValue EventTargetBootstrap::getEventHandlerAttribute(
    jsg::Lock& js, jsg::JsValue target, jsg::JsValue type) {
  static constexpr auto kName = "getEventHandlerAttribute"_kjc;
  auto self = requireEventTarget(js, target, kName);
  auto typeStr = requireType(js, type, kName);
  KJ_IF_SOME(value, self->getEventHandlerAttribute(js, typeStr)) {
    return value;
  }
  return js.null();
}

void EventTargetBootstrap::setEventHandlerAttribute(
    jsg::Lock& js, jsg::JsValue target, jsg::JsValue type, jsg::JsValue value) {
  static constexpr auto kName = "setEventHandlerAttribute"_kjc;
  auto self = requireEventTarget(js, target, kName);
  auto typeStr = requireType(js, type, kName);

  // Mirrors JSG's unwrapping of a setter's OneOf<HandlerFunction, JsValue> argument: a function
  // becomes the handler, invoked with the global object as its receiver; any other value is
  // passed through for setEventHandlerAttribute() to classify.
  using Handler = kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>;
  KJ_IF_SOME(fn, value.tryCast<jsg::JsFunction>()) {
    auto handler = EventTarget::HandlerFunction(&callJsEventHandler,
        jsg::Value(js.v8Isolate, js.v8Context()->Global()),
        jsg::V8Ref<v8::Function>(js.v8Isolate, v8::Local<v8::Function>(fn)));
    self->setEventHandlerAttribute(js, typeStr, Handler(kj::mv(handler)));
  } else {
    self->setEventHandlerAttribute(js, typeStr, Handler(value));
  }
}

jsg::JsValue EventTargetBootstrap::dispatchEvent(jsg::Lock& js,
    jsg::JsValue target,
    jsg::JsValue event,
    ReportListenerExceptions reportListenerExceptions) {
  auto self = requireEventTarget(js, target, "dispatchEvent"_kjc);
  auto ev = JSG_REQUIRE_NONNULL(
      eventHandler(js).tryUnwrap(js, event), TypeError, "dispatchEvent() expects an Event");

  auto policy = reportListenerExceptions == ReportListenerExceptions::YES
      ? EventTarget::DispatchExceptionPolicy::REPORT
      : EventTarget::DispatchExceptionPolicy::PROPAGATE;
  auto result = self->dispatchEventImpl(js, kj::mv(ev), policy);
  KJ_IF_SOME(exception, result.firstException) {
    auto record = js.objNoProto();
    record.set(js, "exception"_kj, exception.getHandle(js));
    return record;
  }
  return js.undefined();
}

jsg::JsValue EventTargetBootstrap::createTrustedMessageEvent(
    jsg::Lock& js, jsg::JsValue type, jsg::JsValue data, jsg::JsValue source) {
  static constexpr auto kName = "createTrustedMessageEvent"_kjc;
  auto typeStr = requireType(js, type, kName);
  kj::Maybe<messageevent::MessagePort> maybeSource;
  if (!source.isNullOrUndefined()) {
    maybeSource = JSG_REQUIRE_NONNULL(messageevent::MessagePort::tryFrom(js, source), TypeError,
        kName, "() expects a MessagePort source");
  }

  // MessageEvent is registered in EW_EVENTS_ISOLATE_TYPES alongside EventTarget.
  auto& handler = KJ_ASSERT_NONNULL(js.tryGetTypeHandler<jsg::Ref<MessageEvent>>(),
      "MessageEvent is missing from the isolate type list");
  return jsg::JsValue(handler.wrap(js,
      js.alloc<MessageEvent>(
          js, kj::mv(typeStr), data, kj::String(), kj::mv(maybeSource), kj::none, Trusted::YES)));
}

}  // namespace workerd::api
