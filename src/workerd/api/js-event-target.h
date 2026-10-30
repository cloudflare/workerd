// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>

#include <kj/one-of.h>

namespace workerd::api {

class JsEventTarget;

// An abstraction of an Event, backed by either a C++ Event (basics.h, or any subclass) or a
// TypeScript-implemented Event (src/per_isolate/events). The API is limited strictly to what the
// C++ side of workerd needs.
class JsEvent final {
 public:
  using Impl = kj::OneOf<jsg::Ref<Event>, jsg::JsRef<jsg::JsObject>>;

  // Adopt a C++ event.
  template <typename T>
    requires(kj::canConvert<T&, Event&>())
  JsEvent(jsg::Ref<T> event): impl(jsg::Ref<Event>(kj::mv(event))) {}

  // Adopt a TypeScript-implemented event. No brand validation happens here: the caller asserts
  // the object really is one.
  JsEvent(jsg::Lock& js, jsg::JsRef<jsg::JsObject> obj);

  JsEvent(JsEvent&&) = default;
  JsEvent& operator=(JsEvent&&) = default;
  KJ_DISALLOW_COPY(JsEvent);

 private:
  Impl impl;

  // The event's JS object, creating a C++ event's wrapper if it has none.
  jsg::JsValue getHandle(jsg::Lock& js);

  friend class JsEventTarget;
};

// An abstraction of an EventTarget, backed by either a C++ EventTarget (basics.h) or a
// TypeScript-implemented EventTarget (src/per_isolate/events). The API is limited strictly to
// what the C++ side of workerd needs: the runtime firing events at its own targets (the global
// scope, AbortSignal, WebSocket, ...) and asking which listeners they have.
class JsEventTarget final {
 public:
  using Impl = kj::OneOf<jsg::Ref<EventTarget>, jsg::JsRef<jsg::JsObject>>;
  using DispatchExceptionPolicy = EventTarget::DispatchExceptionPolicy;
  using DispatchResult = EventTarget::DispatchResult;

  // Adopt one of the runtime's C++ event targets. This is an autogate dispatch point: under
  // typescript-eventtarget, a target whose JS wrapper carries the TypeScript EventTarget's state
  // is adopted as a TypeScript target, since that is where its listeners are. Otherwise, and
  // always without the autogate, the C++ EventTarget is used.
  static JsEventTarget from(jsg::Lock& js, EventTarget& target);

  // Adopt a TypeScript-implemented event target. No brand validation happens here: the caller
  // asserts the object really is one.
  JsEventTarget(jsg::Lock& js, jsg::JsRef<jsg::JsObject> obj);

  JsEventTarget(JsEventTarget&&) = default;
  JsEventTarget& operator=(JsEventTarget&&) = default;
  KJ_DISALLOW_COPY(JsEventTarget);

  // Create a trusted event of the given type, neither bubbling nor cancelable, for the runtime
  // to fire at this target. The event has the same implementation as the target, so that
  // dispatch() accepts it. `type` must outlive the event, as Event's StringPtr constructor
  // requires; pass a string literal.
  JsEvent createEvent(jsg::Lock& js, kj::StringPtr type);

  // Dispatch the event at this target, as the runtime: a trusted event keeps its isTrusted.
  // Under PROPAGATE, the first listener exception ends the dispatch and is thrown; under REPORT,
  // listener exceptions are reported and the dispatch continues, and the first is returned in
  // the result. The policy is applied as given: callers that want the compat-flag fallback pass
  // EventTarget::effectiveExceptionPolicy()'s result.
  //
  // A C++ target accepts only a C++ event.
  DispatchResult dispatch(jsg::Lock& js,
      JsEvent event,
      DispatchExceptionPolicy policy = DispatchExceptionPolicy::PROPAGATE);

  // The number of listeners registered for `type` with addEventListener() (including an
  // activated event handler attribute's), not counting a legacy on<type> property.
  size_t getListenerCount(jsg::Lock& js, kj::StringPtr type);

  // The event types that have at least one listener.
  kj::Array<kj::String> getListenerTypes(jsg::Lock& js);

  // Remove every listener, for teardown. Listener count observers are not notified.
  void removeAllListeners(jsg::Lock& js);

 private:
  explicit JsEventTarget(Impl impl): impl(kj::mv(impl)) {}

  Impl impl;
};

}  // namespace workerd::api
