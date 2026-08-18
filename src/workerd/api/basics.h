// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// This file defines Event- and EventTarget-related APIs.
//
// TODO(cleanup): Rename to events.h?

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/external-pusher.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/canceler.h>
#include <workerd/util/strong-bool.h>

#include <kj/function.h>
#include <kj/map.h>

namespace workerd::api {

class EventTarget;
class AbortSignal;
class AbortController;
class ActorState;

WD_STRONG_BOOL(Trusted);

// An implementation of the Web Platform Standard Event API
class Event: public jsg::Object {
 public:
  struct Init final {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    JSG_STRUCT(bubbles, cancelable, composed);
  };

  // Only events constructed by the runtime itself may pass Trusted::YES.
  inline explicit Event(kj::String ownType, Init init = {}, Trusted trusted = Trusted::NO)
      : ownType(kj::mv(ownType)),
        type(this->ownType) {
    flags.trusted = trusted == Trusted::YES;
    flags.bubbles = init.bubbles.orDefault(false);
    flags.cancelable = init.cancelable.orDefault(false);
    flags.composed = init.composed.orDefault(false);
  }

  inline explicit Event(kj::StringPtr type, Init init = {}, Trusted trusted = Trusted::NO)
      : type(type) {
    flags.trusted = trusted == Trusted::YES;
    flags.bubbles = init.bubbles.orDefault(false);
    flags.cancelable = init.cancelable.orDefault(false);
    flags.composed = init.composed.orDefault(false);
  }

  inline bool isPreventDefault() const {
    return flags.preventedDefault;
  }
  inline void clearPreventDefault() {
    flags.preventedDefault = false;
  }

  void beginDispatch(jsg::Ref<EventTarget> target);
  inline void endDispatch() {
    flags.isBeingDispatched = false;
  }

  inline bool isStopped() const {
    return flags.stopped;
  }

  static jsg::Ref<Event> constructor(jsg::Lock& js, kj::String type, jsg::Optional<Init> init);
  kj::StringPtr getType();

  inline void stopImmediatePropagation() {
    flags.stopped = true;
  }
  inline void preventDefault() {
    flags.preventedDefault = true;
  }

  // The only phases we actually use are NONE and AT_TARGET but we provide
  // all of them to meet spec compliance.
  enum Phase {
    NONE,
    CAPTURING_PHASE,
    AT_TARGET,
    BUBBLING_PHASE,
  };

  inline int getEventPhase() const {
    return flags.isBeingDispatched ? AT_TARGET : NONE;
  }

  // Much of the following is not used in our implementation of Event
  // simply because we do not support the notion of bubbled events
  // (events propagated up through a hierarchy of objects). They are
  // provided to fill-out Event spec compliance.

  inline bool getCancelBubble() const {
    return flags.propagationStopped;
  }
  inline void setCancelBubble(bool stopped) {
    flags.propagationStopped = stopped;
  }
  inline void stopPropagation() {
    flags.propagationStopped = true;
  }
  inline bool getComposed() const {
    return flags.composed;
  }
  inline bool getBubbles() const {
    return flags.bubbles;
  }
  inline bool getCancelable() const {
    return flags.cancelable;
  }
  inline bool getDefaultPrevented() const {
    return getCancelable() && flags.preventedDefault;
  }
  inline bool getReturnValue() const {
    return !getDefaultPrevented();
  }

  // We provide the timeStamp property for spec compliance but we force
  // the value to 0.0 always because we really don't want users to rely
  // on this property for timing details.
  inline double getTimestamp() const {
    return 0.0;
  }

  // Per the spec, an event is trusted iff it was constructed and dispatched by the runtime
  // itself. The Event constructor defaults to untrusted; runtime construction sites (and
  // runtime-only subclass constructors) opt in by passing Trusted::YES explicitly.
  inline bool getIsTrusted() const {
    return flags.trusted;
  }

  // The currentTarget is the EventTarget on which the Event is being
  // dispatched. This will be set every time dispatchEvent() is called
  // successfully and will be null after dispatchEvent returns.
  kj::Maybe<jsg::Ref<EventTarget>> getCurrentTarget();

  // Because we don't support hierarchical EventTargets, this function
  // will always return the same value as getCurrentTarget().
  jsg::Optional<jsg::Ref<EventTarget>> getTarget();

  // For our implementation, since we do not support hierarchical EventTargets,
  // the composedPath is always either an empty array if the Event is currently
  // not being dispatched, or an array containing only the currentTarget if
  // it is being dispatched.
  kj::Array<jsg::Ref<EventTarget>> composedPath();

  JSG_RESOURCE_TYPE(Event, CompatibilityFlags::Reader flags) {
    // Previously, we were setting all properties as instance properties,
    // which broke the ability to subclass the Event object. With the
    // compatibility flag set, we instead attach the properties to the
    // prototype.
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(type, getType);
      JSG_READONLY_PROTOTYPE_PROPERTY(eventPhase, getEventPhase);
      JSG_READONLY_PROTOTYPE_PROPERTY(composed, getComposed);
      JSG_READONLY_PROTOTYPE_PROPERTY(bubbles, getBubbles);
      JSG_READONLY_PROTOTYPE_PROPERTY(cancelable, getCancelable);
      JSG_READONLY_PROTOTYPE_PROPERTY(defaultPrevented, getDefaultPrevented);
      JSG_READONLY_PROTOTYPE_PROPERTY(returnValue, getReturnValue);
      if (flags.getPedanticWpt()) {
        JSG_READONLY_PROTOTYPE_PROPERTY(currentTarget, getCurrentTarget);
      } else {
        // The original implementation had getTarget simply deferring to
        // getCurrentTarget, the new impl moves the original impl into
        // getTarget here so having currentTarget point to getTarget
        // preserves the original behavior.
        JSG_READONLY_PROTOTYPE_PROPERTY(currentTarget, getTarget);
      }
      JSG_READONLY_PROTOTYPE_PROPERTY(target, getTarget);
      JSG_READONLY_PROTOTYPE_PROPERTY(srcElement, getTarget);
      JSG_READONLY_PROTOTYPE_PROPERTY(timeStamp, getTimestamp);
      if (flags.getPedanticWpt()) {
        JSG_READONLY_INSTANCE_PROPERTY(isTrusted, getIsTrusted);
      } else {
        JSG_READONLY_PROTOTYPE_PROPERTY(isTrusted, getIsTrusted);
      }

      JSG_PROTOTYPE_PROPERTY(cancelBubble, getCancelBubble, setCancelBubble);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(type, getType);
      JSG_READONLY_INSTANCE_PROPERTY(eventPhase, getEventPhase);
      JSG_READONLY_INSTANCE_PROPERTY(composed, getComposed);
      JSG_READONLY_INSTANCE_PROPERTY(bubbles, getBubbles);
      JSG_READONLY_INSTANCE_PROPERTY(cancelable, getCancelable);
      JSG_READONLY_INSTANCE_PROPERTY(defaultPrevented, getDefaultPrevented);
      JSG_READONLY_INSTANCE_PROPERTY(returnValue, getReturnValue);
      if (flags.getPedanticWpt()) {
        JSG_READONLY_INSTANCE_PROPERTY(currentTarget, getCurrentTarget);
      } else {
        JSG_READONLY_INSTANCE_PROPERTY(currentTarget, getTarget);
      }
      JSG_READONLY_INSTANCE_PROPERTY(target, getTarget);
      JSG_READONLY_INSTANCE_PROPERTY(srcElement, getCurrentTarget);
      JSG_READONLY_INSTANCE_PROPERTY(timeStamp, getTimestamp);
      JSG_READONLY_INSTANCE_PROPERTY(isTrusted, getIsTrusted);

      JSG_INSTANCE_PROPERTY(cancelBubble, getCancelBubble, setCancelBubble);
    }

    JSG_METHOD(stopImmediatePropagation);
    JSG_METHOD(preventDefault);
    JSG_METHOD(stopPropagation);
    JSG_METHOD(composedPath);

    JSG_STATIC_CONSTANT(NONE);
    JSG_STATIC_CONSTANT(CAPTURING_PHASE);
    JSG_STATIC_CONSTANT(AT_TARGET);
    JSG_STATIC_CONSTANT(BUBBLING_PHASE);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("type", ownType);
    tracker.trackField("target", target);
  }

 private:
  // listing ownType first so type can be initialized with it in constructor
  kj::String ownType;
  kj::StringPtr type;
  kj::Maybe<jsg::Ref<EventTarget>> target;

  struct Flags {
    uint8_t trusted : 1 = 1;
    uint8_t stopped : 1 = 0;
    uint8_t preventedDefault : 1 = 0;
    uint8_t isBeingDispatched : 1 = 0;
    uint8_t propagationStopped : 1 = 0;
    uint8_t composed : 1 = 0;
    uint8_t bubbles : 1 = 0;
    uint8_t cancelable : 1 = 0;
  };
  Flags flags{};

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(target);
  }
};

class ExtendableEvent: public Event {
 public:
  // Runtime-only (the JS constructor is deleted); always trusted.
  explicit ExtendableEvent(kj::StringPtr type): Event(type, {}, Trusted::YES) {}

  // While ExtendableEvent is defined by the spec to be constructable, there's really not a
  // lot of reason currently to do so, especially with the restriction that waitUntil can
  // only be called on trusted events (which have to originate from within the system).
  static jsg::Ref<ExtendableEvent> constructor(kj::String type) = delete;

  void waitUntil(kj::Promise<void> promise);

  jsg::Optional<jsg::Ref<ActorState>> getActorState(jsg::Lock& js);

  JSG_RESOURCE_TYPE(ExtendableEvent) {
    JSG_INHERIT(Event);
    JSG_METHOD(waitUntil);

#if !WORKERD_API_BASICS_TEST
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(actorState, getActorState);
#endif

    JSG_TS_OVERRIDE({ actorState: never });
    // Omit `actorState` from definitions
  }
};

// An implementation of the Web Platform Standard CustomEvent API
class CustomEvent: public Event {
 public:
  struct CustomEventInit final {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    jsg::Optional<jsg::JsRef<jsg::JsValue>> detail;
    JSG_STRUCT(bubbles, cancelable, composed, detail);

    operator Event::Init();
  };

  explicit CustomEvent(kj::String ownType, CustomEventInit init = CustomEventInit());

  static jsg::Ref<CustomEvent> constructor(
      jsg::Lock& js, kj::String type, jsg::Optional<CustomEventInit> init);

  jsg::Optional<jsg::JsValue> getDetail(jsg::Lock& js);

  JSG_RESOURCE_TYPE(CustomEvent) {
    JSG_INHERIT(Event);
    JSG_READONLY_PROTOTYPE_PROPERTY(detail, getDetail);
    JSG_TS_OVERRIDE(<T = any> {
      get detail(): T;
    });
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("detail", detail);
  }

 private:
  jsg::Optional<jsg::JsRef<jsg::JsValue>> detail;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(detail);
  }
};

// An implementation of the Web Platform Standard EventTarget API
class EventTarget: public jsg::Object {
 public:
  size_t getHandlerCount(kj::StringPtr type) const;

  kj::Array<kj::StringPtr> getHandlerNames() const;

  // What to do when a listener throws during dispatch.
  //
  // PROPAGATE is the historical workerd behavior: the first throwing listener ends the
  // dispatch and the exception flows out of dispatchEventImpl(). The runtime's own top-level
  // event delivery (fetch/scheduled/etc.) relies on this for its failure semantics, so it
  // remains the default for internal callers.
  //
  // REPORT is the behavior the spec requires of the JS-observable surfaces ("inner invoke"
  // step 11: report the exception and continue with the next listener): the exception is
  // delivered to the global scope's report-an-exception machinery (the global 'error'
  // event, then console logging) and the dispatch continues. Used by the JS-exposed
  // dispatchEvent(), by AbortSignal aborts (which the spec forbids from throwing), by the
  // global scope's own reportError(), and by the UA-fired dispatches of WebSocket,
  // EventSource, and MessagePort — the latter additionally apply a fail-fast reaction via
  // DispatchResult::firstException.
  enum class DispatchExceptionPolicy { PROPAGATE, REPORT };

  // The result of a dispatchEventImpl() call.
  struct DispatchResult {
    // Per the spec's dispatch algorithm: false iff the event is cancelable and a listener
    // called preventDefault().
    bool result;

    // Under DispatchExceptionPolicy::REPORT, the first listener exception, if any listener
    // threw. It has already been reported; it is surfaced here for native callers that
    // additionally apply a fail-fast reaction on top of the spec's report-and-continue
    // dispatch (e.g. erroring out a WebSocket whose listener threw).
    kj::Maybe<jsg::JsRef<jsg::JsValue>> firstException;
  };

  DispatchResult dispatchEventImpl(jsg::Lock& js,
      jsg::Ref<Event> event,
      DispatchExceptionPolicy exceptionPolicy = DispatchExceptionPolicy::PROPAGATE);

  inline void removeAllHandlers() {
    typeMap.clear();
    // Any activated event handler attribute trampolines were just dropped along with the
    // listener list; clear their identities so a later assignment activates afresh.
    for (auto& entry: eventHandlerAttributes) {
      entry.value.listenerIdentity = kj::none;
    }
  }

  inline void enableWarningOnSpecialEvents() {
    flags.warnOnSpecialEvents = true;
  }

  // ---------------------------------------------------------------------------
  // JS API

  struct EventListenerOptions {
    jsg::Optional<bool> capture;

    JSG_STRUCT(capture);
  };

  struct AddEventListenerOptions {
    jsg::Optional<bool> capture;
    jsg::Optional<bool> passive;
    jsg::Optional<bool> once;
    jsg::Optional<jsg::Ref<AbortSignal>> signal;

    JSG_STRUCT(capture, passive, once, signal);
  };

  using AddEventListenerOpts = kj::OneOf<AddEventListenerOptions, bool>;
  using EventListenerOpts = kj::OneOf<EventListenerOptions, bool>;

  using HandlerFunction = jsg::Function<jsg::Optional<jsg::Value>(jsg::Ref<Event>)>;

  struct HandlerObject {
    HandlerFunction handleEvent;
    jsg::SelfRef self;
    JSG_STRUCT(handleEvent, self);

    // TODO(cleanup): Get rid of this override and parse the type directly in param-extractor.rs
    JSG_STRUCT_TS_OVERRIDE({
      handleEvent: (event: Event) => any | undefined;
    });
  };
  using Handler = kj::OneOf<HandlerFunction, HandlerObject>;

  void addEventListener(jsg::Lock& js,
      kj::String type,
      kj::Maybe<jsg::Identified<Handler>> maybeHandler,
      jsg::Optional<AddEventListenerOpts> maybeOptions,
      const jsg::TypeHandler<jsg::Ref<EventTarget>>& eventTargetHandler);
  void removeEventListener(jsg::Lock& js,
      kj::String type,
      kj::Maybe<jsg::HashableV8Ref<v8::Object>> maybeHandler,
      jsg::Optional<EventListenerOpts> options);
  bool dispatchEvent(jsg::Lock& js, jsg::Ref<Event> event);

  JSG_RESOURCE_TYPE(EventTarget) {
    JSG_METHOD(addEventListener);
    JSG_METHOD(removeEventListener);
    JSG_METHOD(dispatchEvent);

    JSG_TS_DEFINE(
      type EventListener<EventType extends Event = Event> = (event: EventType) => void;
      interface EventListenerObject<EventType extends Event = Event> {
        handleEvent(event: EventType): void;
      }
      type EventListenerOrEventListenerObject<EventType extends Event = Event> = EventListener<EventType> | EventListenerObject<EventType>;
    );
    JSG_TS_OVERRIDE(<EventMap extends Record<string, Event> = Record<string, Event>> {
      addEventListener<Type extends keyof EventMap>(type: Type, handler: EventListenerOrEventListenerObject<EventMap[Type]>, options?: EventTargetAddEventListenerOptions | boolean): void;
      removeEventListener<Type extends keyof EventMap>(type: Type, handler: EventListenerOrEventListenerObject<EventMap[Type]>, options?: EventTargetEventListenerOptions | boolean): void;
      dispatchEvent(event: EventMap[keyof EventMap]): boolean;
    });
  }
  JSG_REFLECTION(onEvents);

  static jsg::Ref<EventTarget> constructor(jsg::Lock& js);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 protected:
  // Invoked whenever the number of registered listeners for `type` changes: on
  // addEventListener() and removeEventListener() — including once-listener removal during
  // dispatch and {signal}-triggered removals — and on managed event handler attribute
  // activation and deactivation, whose trampoline occupies a regular listener slot.
  // removeAllHandlers() does not notify: it is only used for wholesale teardown.
  virtual void listenerCountChanged(jsg::Lock& js, kj::StringPtr type, size_t count) {}

  // The result of a setEventHandlerAttribute() assignment: cleared, or activated with a
  // non-callable object, or activated with a callable handler.
  enum class EventHandlerAssignment { CLEARED, OBJECT, CALLABLE };

  // Implement HTML's event handler IDL attribute semantics for an on<type> attribute (e.g.
  // AbortSignal's onabort): assigning any object activates a trampoline listener that
  // occupies a normal position in the listener list (kept across reassignment; a fresh
  // position after deactivation), and assigning anything else deactivates it. Only callable
  // values are ever invoked: the trampoline invokes whatever callable the attribute holds at
  // dispatch time. Dispatch does not additionally consult the legacy on<type> property
  // reflection for a managed type. The getter returns the exact value assigned.
  kj::Maybe<jsg::JsValue> getEventHandlerAttribute(jsg::Lock& js, kj::StringPtr type);
  EventHandlerAssignment setEventHandlerAttribute(jsg::Lock& js,
      kj::StringPtr type,
      jsg::Optional<kj::OneOf<HandlerFunction, jsg::JsValue>> handler);

  // Registers an internal listener occupying a normal position in the listener list. The
  // identity may later be passed to removeEventListener() to deactivate it.
  void addEventHandlerListener(jsg::Lock& js,
      kj::StringPtr type,
      jsg::HashableV8Ref<v8::Object> identity,
      HandlerFunction callback);

 private:
  struct EventHandler {
    // The listener's identity (the function or object passed to addEventListener, or a
    // synthesized object for internally-registered listeners), used for removeEventListener
    // matching.
    jsg::HashableV8Ref<v8::Object> identity;
    HandlerFunction callback;

    // When once is true, the handler will be removed after it is invoked one time.
    bool once = false;

    // If the event handler is registered with an AbortSignal (the {signal} option), this
    // holds the RAII registration for the signal's abort algorithm that removes this
    // listener, so that if this entry goes away before the signal aborts, the algorithm is
    // unregistered. The handle is opaque: the only thing to do with it is drop it.
    kj::Maybe<kj::Own<void>> abortHandler;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(identity, callback);

      // Note that we intentionally do NOT visit `abortHandler`. It holds no JS references
      // of its own; the algorithm it registers is owned — and GC-visited — by the
      // AbortSignal it was registered with.
    }

    kj::StringPtr jsgGetMemoryName() const {
      return "EventHandler"_kjc;
    }
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };

  struct EventHandlerHashCallbacks {
    const jsg::HashableV8Ref<v8::Object>& keyForRow(const kj::Own<EventHandler>& row) const;
    bool matches(const kj::Own<EventHandler>& a, const jsg::HashableV8Ref<v8::Object>& b) const;
    uint hashCode(const jsg::HashableV8Ref<v8::Object>& obj) const;
  };

  struct EventHandlerSet {
    kj::Table<kj::Own<EventHandler>,
        kj::HashIndex<EventHandlerHashCallbacks>,
        kj::InsertionOrderIndex>
        handlers;

    EventHandlerSet(): handlers(EventHandlerHashCallbacks(), {}) {}

    kj::StringPtr jsgGetMemoryName() const {
      return "EventHandlerSet"_kjc;
    }
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };

  EventHandlerSet& getOrCreate(kj::StringPtr str) KJ_LIFETIMEBOUND;

  jsg::PropertyReflection<kj::OneOf<HandlerFunction, jsg::Value>> onEvents;

  kj::HashMap<kj::String, EventHandlerSet> typeMap;

  // State for one managed on<type> event handler IDL attribute (HTML: an "event handler").
  struct EventHandlerAttribute {
    struct Handler {
      // The exact value assigned, returned by the getter.
      jsg::JsRef<jsg::JsValue> value;
      // The invocable form, present iff the assigned value was callable. A non-callable
      // object is retained as the attribute value but never invoked.
      kj::Maybe<HandlerFunction> fn;
    };
    kj::Maybe<Handler> handler;

    // While activated, the identity of the trampoline listener entry occupying the
    // attribute's position in the listener list.
    kj::Maybe<jsg::HashableV8Ref<v8::Object>> listenerIdentity;
  };

  // Keyed by event type. An entry's presence marks on<type> as managed: dispatch must not
  // additionally consult the legacy on<type> property reflection for that type, which would
  // fire the handler twice.
  kj::HashMap<kj::String, EventHandlerAttribute> eventHandlerAttributes;

  bool managesEventHandlerAttribute(kj::StringPtr type) const {
    return eventHandlerAttributes.find(type) != kj::none;
  }

  // HTML "activate an event handler": registers the trampoline listener if it is not already
  // registered (an already-active handler keeps its position across reassignment).
  void activateEventHandlerAttribute(
      jsg::Lock& js, kj::StringPtr type, EventHandlerAttribute& attribute);

  struct Flags {
    // When using module syntax, the "fetch", "scheduled", "trace", etc.
    // events are handled by exports rather than events. When warnOnSpecialEvents is true,
    // when using module syntax, attempts to register event handlers for these special
    // types of events will result in a warning being emitted.
    uint8_t warnOnSpecialEvents : 1 = 0;
    // Event handlers are not supposed to return values. The first time one does, we'll
    // emit a warning to help users debug things but we'll otherwise ignore it.
    uint8_t warnOnHandlerReturn : 1 = 1;
  };
  Flags flags;

  void visitForGc(jsg::GcVisitor& visitor);
};

// An implementation of the Web Platform Standard AbortSignal API
class AbortTriggerRpcClient;

class AbortSignal final: public EventTarget {
 public:
  enum class Flag { NONE, NEVER_ABORTS, IGNORE_FOR_SUBREQUESTS };

  AbortSignal(kj::Maybe<kj::Exception> exception = kj::none,
      jsg::Optional<jsg::JsRef<jsg::JsValue>> maybeReason = kj::none,
      Flag flag = Flag::NONE);

  using PendingReason = ExternalPusherImpl::PendingAbortReasonBox;

  // The AbortSignal explicitly does not expose a constructor(). It is
  // illegal for user code to create an AbortSignal directly.
  static jsg::Ref<AbortSignal> constructor() = delete;

  bool getAborted(jsg::Lock& js);

  jsg::JsValue getReason(jsg::Lock& js);

  // Will synchronously throw an error if the abort signal has been triggered.
  void throwIfAborted(jsg::Lock& js);

  inline bool getNeverAborts() const {
    return flag == Flag::NEVER_ABORTS;
  }

  // The static abort() function here returns an AbortSignal that
  // has been pre-emptively aborted. It's useful when it might still
  // be desirable to kick off an async process while communicating
  // that it shouldn't continue.
  static jsg::Ref<AbortSignal> abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  // Returns an AbortSignal that is triggered after delay milliseconds.
  static jsg::Ref<AbortSignal> timeout(jsg::Lock& js, double delay);

  void triggerAbort(
      jsg::Lock& js, jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> maybeReason);

  // Implements the spec's "create a dependent abort signal": returns a signal that aborts
  // when any of the given signals abort, carrying the first aborter's reason.
  static jsg::Ref<AbortSignal> any(jsg::Lock& js, kj::Array<jsg::Ref<AbortSignal>> signals);

  // The onabort event handler IDL attribute (see EventTarget::setEventHandlerAttribute).
  // Assigning a callable also subscribes RPC-backed signals to remote abort notifications.
  kj::Maybe<jsg::JsValue> getOnAbort(jsg::Lock& js);
  void setOnAbort(
      jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler);

  void addEventListener(jsg::Lock& js,
      kj::String type,
      jsg::Identified<Handler> handler,
      jsg::Optional<AddEventListenerOpts> maybeOptions,
      const jsg::TypeHandler<jsg::Ref<EventTarget>>& eventTargetHandler);

  JSG_RESOURCE_TYPE(AbortSignal, CompatibilityFlags::Reader flags) {
    JSG_INHERIT(EventTarget);
    JSG_STATIC_METHOD(abort);
    JSG_STATIC_METHOD(timeout);
    JSG_STATIC_METHOD(any);
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(aborted, getAborted);
      JSG_READONLY_PROTOTYPE_PROPERTY(reason, getReason);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(aborted, getAborted);
      JSG_READONLY_INSTANCE_PROPERTY(reason, getReason);
    }
    JSG_PROTOTYPE_PROPERTY(onabort, getOnAbort, setOnAbort);
    JSG_METHOD(throwIfAborted);

    if (flags.getWorkerdExperimental()) {
      JSG_METHOD(skipReleaseForTest);
      JSG_METHOD(getNativeRegistrationCountForTest);
      JSG_TS_OVERRIDE({
        skipReleaseForTest: never;
        getNativeRegistrationCountForTest: never;
      });
    }
  }

  // Allows this AbortSignal to also serve as a kj::Canceler: the returned promise is
  // canceled (rejected with a kj::Exception derived from the abort reason) if this signal
  // is aborted. If the signal is ALREADY aborted, the returned promise is immediately
  // rejected the same way, indistinguishable from an abort arriving right after wrapping.
  // The cancellation runs in the calling IoContext; if the abort is triggered from a
  // different request (or outside any request), it is delivered to the calling context the
  // next time it runs. Requires an active IoContext.
  template <typename T>
  kj::Promise<T> wrap(jsg::Lock& js, kj::Promise<T> promise) {
    if (getNeverAborts()) {
      // This signal can never abort, so there is nothing to hook up.
      return kj::mv(promise);
    }

    // The wrapped promise carries the Cancellation — the (sole-owner) canceler and its
    // registration, whose declaration order guarantees the registration unhooks before the
    // canceler dies.
    auto cancellation = newCanceler(js);
    auto wrapped = cancellation.canceler->wrap(kj::mv(promise));
    return wrapped.attach(kj::mv(cancellation));
  }

  template <typename T>
  static kj::Promise<T> maybeCancelWrap(
      jsg::Lock& js, kj::Maybe<jsg::Ref<AbortSignal>>& signal, kj::Promise<T> promise) {
    KJ_IF_SOME(s, signal) {
      return s->wrap(js, kj::mv(promise));
    } else {
      return kj::mv(promise);
    }
  }

  // A canceler hooked up to this signal, plus the RAII registration keeping the hook alive.
  // Returned by newCanceler() for native consumers that need more than promise wrapping
  // (e.g. ReleasingCanceler::Listener callbacks).
  struct Cancellation {
    // Sole owner of the canceler. The signal's registration reaches it only by reference.
    kj::Own<ReleasingCanceler> canceler;

    // Keeps the canceler hooked to the signal; dropping it (from any thread) unhooks.
    //
    // WARNING: The registration's reference to the canceler is valid only while this handle
    // is registered, so the holder MUST destroy this handle before (or together with, but
    // ordered before) the canceler — i.e. declare it after the canceler member — and must
    // keep both on the creating request's thread, as consumer objects owned by the request
    // naturally are.
    kj::Own<void> registration;
  };

  // Creates a new canceler that is canceled — with a kj::Exception derived from the abort
  // reason — when this signal aborts, following the same ownership and cross-request rules
  // as wrap(). If the signal is already aborted (or can never abort), the returned canceler
  // is pre-canceled (or inert) and no registration is made. Requires an active IoContext.
  Cancellation newCanceler(jsg::Lock& js);

  // Registers a native callback to be invoked with the abort exception if/when this signal
  // aborts. The callback runs under the isolate lock in the IoContext that is current at
  // registration time: synchronously when the abort is triggered within that context,
  // otherwise delivered on that context's next turn, and dropped entirely (never invoked)
  // once that context has been destroyed. If the signal can never abort, the callback is
  // never invoked and no registration is made.
  //
  // Dropping the returned handle (safe from any thread) unregisters the callback: once the
  // handle is destroyed, the callback is guaranteed to never (again) be invoked, so it may
  // capture references whose validity the holder ties to the handle's lifetime (see
  // Cancellation::registration).
  //
  // Requires an active IoContext. The caller is expected to have checked getAborted() first.
  kj::Own<void> addAbortAction(
      jsg::Lock& js, kj::Function<void(jsg::Lock&, const kj::Exception&)> action);

  // Implements the DOM spec's "add an algorithm to signal's abort algorithms": registers a
  // JS-heap callback that runs under the isolate lock, in whichever context triggers the
  // abort, before the 'abort' event is dispatched — exactly once. Unlike addAbortAction(),
  // no IoContext is required or captured, so the algorithm must only touch JS-heap state.
  // Algorithms never run for synthetic dispatchEvent('abort') calls; only a real abort runs
  // them (and then empties the list, per spec).
  //
  // Dropping the returned handle unregisters the algorithm; the handle holds only a weak
  // reference to this signal and must be dropped under the isolate lock (it is expected to
  // be held by JS-heap objects). The caller is expected to have checked getAborted() first:
  // algorithms are never invoked retroactively.
  kj::Own<void> addAbortAlgorithm(jsg::Lock& js, jsg::Function<void()> algorithm);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    EventTarget::visitForMemoryInfo(tracker);
    tracker.trackField("reason", reason);
    tracker.trackFieldWithSize(
        "nativeRegistrations", nativeRegistrations.size() * sizeof(kj::Arc<RegistrationCell>));
  }

  void serialize(jsg::Lock& js, jsg::Serializer& serializer);

  // To test what happens if a capability is dropped before invoking release on the cloned abort
  // signal, this method will tell every rpcClient to skip this step before destruction.
  void skipReleaseForTest();

  // Test-only introspection: the number of native registration cells (live or empty awaiting
  // a sweep) plus RPC registrations currently held by this signal. Lets tests assert that
  // completed registrations are reclaimed rather than accumulating on long-lived signals.
  int getNativeRegistrationCountForTest();

  static jsg::Ref<AbortSignal> deserialize(
      jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer);

  JSG_SERIALIZABLE(rpc::SerializationTag::ABORT_SIGNAL);

  // True if this is a signal on the request of an incoming fetch. When the compat flag
  // `requestSignalPassthrough` is set, this flag has no effect. But to ensure backwards
  // compatibility, when this flag is not set, this signal will not be passed through to
  // subrequests derived from the incoming request.
  bool isIgnoredForSubrequests(jsg::Lock& js) const;

 private:
  Flag flag;

  // Set iff this signal has been aborted; the source of truth for getAborted(). Also the
  // exception native cancellations reject with. Plain data, safe on the JS heap: no
  // IoContext is required to create or read a signal's abort state.
  kj::Maybe<kj::Exception> maybeAbortException;

  kj::Maybe<jsg::JsRef<jsg::JsValue>> reason;

  // One native abort action, shared between this signal and one consumer. The action is
  // invoked at most once, only ever in its owning IoContext (synchronously if the abort is
  // triggered there; otherwise on that context's next turn), always under the isolate lock,
  // and never again after the consumer's RAII handle clears the slot. Because the handle is
  // held by (or attached to) objects the owning request destroys, an IoContext teardown
  // reclaims the action without ever touching the signal; the signal side retains only this
  // trivial shell until swept.
  //
  // The action slot is taken under the mutex and invoked after unlocking. A cross-context
  // abort does not take the slot; it schedules a task in the owning context that re-takes it
  // on arrival — so a consumer that goes away in the meantime reliably turns the delivery
  // into a no-op.
  struct RegistrationCell final: public kj::AtomicRefcounted {
    RegistrationCell(
        IoCrossContextExecutor executor, kj::Function<void(jsg::Lock&, const kj::Exception&)> fn)
        : executor(kj::mv(executor)) {
      *action.lockExclusive() = kj::mv(fn);
    }

    // Routes the action into the owning IoContext and answers "is that context current /
    // still alive?". Immutable, so it is also usable for sweeping after the slot is cleared.
    const IoCrossContextExecutor executor;

    kj::MutexGuarded<kj::Maybe<kj::Function<void(jsg::Lock&, const kj::Exception&)>>> action;
  };

  // Cells are appended on registration and taken wholesale when the signal aborts. Cells
  // whose action has been cleared (consumer done, or its IoContext torn down) or whose
  // owning context is gone are swept on the next registration; this bounds growth for
  // long-lived signals used across many requests. Holds no JS heap references (weak refs at
  // most), so no GC visitation is needed.
  kj::Vector<kj::Arc<RegistrationCell>> nativeRegistrations;

  // Registers an abort action that cancels `canceler` with the abort exception when this
  // signal aborts. The reference remains valid because the returned RAII handle guarantees
  // the action never runs after the handle is destroyed, and the holder destroys the handle
  // before the canceler (see Cancellation::registration).
  kj::Own<void> registerPendingCancellation(jsg::Lock& js, ReleasingCanceler& canceler);

  // The spec's "abort algorithms": insertion-ordered, run and then emptied by triggerAbort()
  // before the 'abort' event is dispatched. Unlike the native registration cells, these hold
  // JS-heap references and are therefore GC-visited.
  struct AbortAlgorithm {
    uint64_t token;
    jsg::Function<void()> fn;
  };
  kj::Vector<AbortAlgorithm> abortAlgorithms;
  uint64_t nextAbortAlgorithmToken = 0;
  void removeAbortAlgorithm(uint64_t token);

  // Spec: "dependent" — true for signals created by AbortSignal.any().
  bool dependent = false;

  // Spec: "dependent signals" — signals created by AbortSignal.any() for which this signal
  // is a source. Strong and GC-visited: a dependent must stay reachable as long as any of
  // its sources could still abort it (V8 collects the cycle once neither side is otherwise
  // reachable). Emptied when this signal aborts; a dependent that aborts first unlinks
  // itself from its remaining sources via severSources().
  kj::Vector<jsg::Ref<AbortSignal>> dependentSignals;

  // Spec: "source signals" — the signals this dependent signal depends on. Weak: used only
  // for AbortSignal.any()'s flattening rule (a dependent passed to any() contributes its
  // sources, never itself, so dependency chains never form) and for severSources().
  kj::Vector<jsg::WeakRef<AbortSignal>> sourceSignals;

  // Records the abort reason and exception (spec "signal abort" step 2, also applied to
  // dependents in steps 3-4 before any abort steps run).
  void setAbortState(jsg::Lock& js, jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> reason);

  // Spec "run the abort steps": abort algorithms, then workerd's native registrations (cells
  // and RPC clones), then the 'abort' event. Requires setAbortState() to have run.
  void runAbortSteps(jsg::Lock& js);

  // Removes this (aborted) dependent signal from any remaining sources so they no longer
  // keep it alive or attempt to re-abort it.
  void severSources(jsg::Lock& js);

  static kj::Exception abortException(
      jsg::Lock& js, const jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>>& reason);

  void visitForGc(jsg::GcVisitor& visitor);

  friend class AbortController;

  // -------------------------------------------------------------
  // RPC client functionality. Used if this signal was serialized.

  // One serialized clone of this signal, to be notified when the signal is triggered. The
  // client is owned by the IoContext in which the signal was serialized — a signal shared
  // across requests may hold registrations from several — and abort delivery is routed into
  // that context like a native registration, re-taking the slot on arrival. There is no
  // consumer-side RAII handle: the slot is reclaimed when the signal aborts, when a sweep
  // finds the owning context destroyed, or when the signal itself is destroyed (either way
  // the client's own destructor tells the peer that no abort is coming).
  struct RpcRegistration final: public kj::AtomicRefcounted {
    RpcRegistration(IoCrossContextExecutor executor, IoOwn<AbortTriggerRpcClient> client)
        : executor(kj::mv(executor)) {
      *this->client.lockExclusive() = kj::mv(client);
    }

    const IoCrossContextExecutor executor;
    kj::MutexGuarded<kj::Maybe<IoOwn<AbortTriggerRpcClient>>> client;
  };
  kj::Vector<kj::Arc<RpcRegistration>> rpcRegistrations;

  // ---------------------------------------------------------------
  // RPC server functionality. Used if this signal was deserialized.

  // Identifies the IoContext that deserialized this signal, which owns rpcAbortPromise
  // below. Only that context can arm the RPC subscription (subscribeToRpcAbort); attempts
  // from other contexts are no-ops.
  kj::Maybe<IoCrossContextExecutor> rpcReceiverContext;
  bool isRpcReceiverContextCurrent();

  // A promise that is fulfilled if an abort() message is received over RPC.
  kj::Maybe<IoOwn<kj::Promise<void>>> rpcAbortPromise;

  // The box through which a serialized abort reason arrives. The abort reason is required in
  // asynchronous event handlers as well as synchronous methods like getReason(), so both sync
  // and async paths check this value. It is written by the receiving request's RPC machinery
  // but readable — under its mutex — from any context, so a deserialized signal that has
  // crossed request boundaries still reports getAborted()/getReason() correctly everywhere.
  kj::Maybe<kj::Arc<PendingReason>> pendingReason;

  // Synchronously check if an abort reason was sent over RPC
  bool hasPendingReason();
  kj::Maybe<jsg::JsValue> deserializePendingReason(jsg::Lock& js);

  // Wait for abort over RPC.
  // We invoke this once at least one event handler is attached to the AbortSignal
  void subscribeToRpcAbort(jsg::Lock& js);
};

// An implementation of the Web Platform Standard AbortController API
class AbortController final: public jsg::Object {
 public:
  explicit AbortController(
      jsg::Lock& js, AbortSignal::Flag abortSignalFlag = AbortSignal::Flag::NONE)
      : signal(js.alloc<AbortSignal>(
            kj::none /* exception */, kj::none /* maybeReason */, abortSignalFlag)) {}

  static jsg::Ref<AbortController> constructor(jsg::Lock& js) {
    return js.alloc<AbortController>(js);
  }

  jsg::Ref<AbortSignal> getSignal() {
    return signal.addRef();
  }

  void abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> reason);

  JSG_RESOURCE_TYPE(AbortController, CompatibilityFlags::Reader flags) {
    if (flags.getJsgPropertyOnPrototypeTemplate()) {
      JSG_READONLY_PROTOTYPE_PROPERTY(signal, getSignal);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(signal, getSignal);
    }
    JSG_METHOD(abort);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("signal", signal);
  }

 private:
  jsg::Ref<AbortSignal> signal;

  void visitForGc(jsg::GcVisitor& visitor) {
    // We have to be careful with GC here. The event listeners added to the AbortSignal
    // could hold a circular reference to the AbortController.
    visitor.visit(signal);
  }
};

// The scheduler class is an emerging web platform standard API that is meant
// to be global and provides task scheduling APIs. We currently only implement
// a subset of the API that is being defined.
class Scheduler final: public jsg::Object {
 public:
  struct WaitOptions {
    jsg::Optional<jsg::Ref<AbortSignal>> signal;
    JSG_STRUCT(signal);
  };

  // Returns a promise that resolves after the `delay` milliseconds.
  // Essentially an awaitable alternative to setTimeout(). The wait
  // can be canceled using an AbortSignal.
  kj::Promise<void> wait(jsg::Lock& js, double delay, jsg::Optional<WaitOptions> maybeOptions);

  JSG_RESOURCE_TYPE(Scheduler) {
    JSG_METHOD(wait);
  }

 private:
};

#define EW_BASICS_ISOLATE_TYPES                                                                    \
  api::Event, api::Event::Init, api::EventTarget, api::EventTarget::EventListenerOptions,          \
      api::EventTarget::AddEventListenerOptions, api::EventTarget::HandlerObject,                  \
      api::AbortController, api::AbortSignal, api::Scheduler, api::Scheduler::WaitOptions,         \
      api::ExtendableEvent, api::CustomEvent, api::CustomEvent::CustomEventInit
// The list of basics.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
