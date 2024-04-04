// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once
// This file defines Event- and EventTarget-related APIs.
//
// TODO(cleanp): Rename to events.h?

#include <workerd/jsg/jsg.h>
#include <workerd/io/io-context.h>
#include <workerd/util/canceler.h>
#include <kj/function.h>
#include <kj/map.h>
#include <workerd/io/compatibility-date.capnp.h>

namespace workerd::api {

class EventTarget;
class AbortSignal;
class AbortController;
class ActorState;

// An implementation of the Web Platform Standard Event API
class Event: public jsg::Object {
public:
  struct Init final {
    jsg::Optional<bool> bubbles;
    jsg::Optional<bool> cancelable;
    jsg::Optional<bool> composed;
    JSG_STRUCT(bubbles, cancelable, composed);
  };

  inline explicit Event(kj::String ownType, Init init = Init(), bool trusted = true)
      : type(ownType),
        ownType(kj::mv(ownType)),
        init(init),
        trusted(trusted) {}

  inline explicit Event(kj::StringPtr type, Init init = Init(), bool trusted = true)
      : type(type),
        init(init),
        trusted(trusted) {}

  inline bool isPreventDefault() const { return preventedDefault; }
  inline void clearPreventDefault() { preventedDefault = false; }

  void beginDispatch(jsg::Ref<EventTarget> target);
  inline void endDispatch() { isBeingDispatched = false; }

  inline bool isStopped() const { return stopped; }

  static jsg::Ref<Event> constructor(kj::String type, jsg::Optional<Init> init);
  kj::StringPtr getType();

  inline void stopImmediatePropagation() { stopped = true; }
  inline void preventDefault() { preventedDefault = true; }

  // The only phases we actually use are NONE and AT_TARGET but we provide
  // all of them to meet spec compliance.
  enum Phase {
    NONE,
    CAPTURING_PHASE,
    AT_TARGET,
    BUBBLING_PHASE,
  };

  inline int getEventPhase() const { return isBeingDispatched ? AT_TARGET : NONE; }

  // Much of the following is not used in our implementation of Event
  // simply because we do not support the notion of bubbled events
  // (events propagated up through a hierarchy of objects). They are
  // provided to fill-out Event spec compliance.

  inline bool getCancelBubble() const { return propagationStopped; }
  inline void setCancelBubble(bool stopped) { propagationStopped = stopped; }
  inline void stopPropagation() { propagationStopped = true; }
  inline bool getComposed() const { return init.composed.orDefault(false); }
  inline bool getBubbles() const { return init.bubbles.orDefault(false); }
  inline bool getCancelable() const { return init.cancelable.orDefault(false); }
  inline bool getDefaultPrevented() const { return getCancelable() && preventedDefault; }
  inline bool getReturnValue() const { return !getDefaultPrevented(); }

  // We provide the timeStamp property for spec compliance but we force
  // the value to 0.0 always because we really don't want users to rely
  // on this property for timing details.
  inline double getTimestamp() const { return 0.0; }

  // What makes an Event trusted? It's pretty simple... any Event created
  // by EW internally is Trusted, any Event created using new Event() in JS
  // is not trusted.
  inline bool getIsTrusted() const { return trusted; }

  // The currentTarget is the EventTarget on which the Event is being
  // dispatched. This will be set every time dispatchEvent() is called
  // successfully and will remain set after dispatching is completed.
  jsg::Optional<jsg::Ref<EventTarget>> getCurrentTarget();

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
      JSG_READONLY_PROTOTYPE_PROPERTY(currentTarget, getCurrentTarget);
      JSG_READONLY_PROTOTYPE_PROPERTY(srcElement, getCurrentTarget);
      JSG_READONLY_PROTOTYPE_PROPERTY(timeStamp, getTimestamp);
      JSG_READONLY_PROTOTYPE_PROPERTY(isTrusted, getIsTrusted);

      JSG_PROTOTYPE_PROPERTY(cancelBubble, getCancelBubble, setCancelBubble);
    } else {
      JSG_READONLY_INSTANCE_PROPERTY(type, getType);
      JSG_READONLY_INSTANCE_PROPERTY(eventPhase, getEventPhase);
      JSG_READONLY_INSTANCE_PROPERTY(composed, getComposed);
      JSG_READONLY_INSTANCE_PROPERTY(bubbles, getBubbles);
      JSG_READONLY_INSTANCE_PROPERTY(cancelable, getCancelable);
      JSG_READONLY_INSTANCE_PROPERTY(defaultPrevented, getDefaultPrevented);
      JSG_READONLY_INSTANCE_PROPERTY(returnValue, getReturnValue);
      JSG_READONLY_INSTANCE_PROPERTY(currentTarget, getCurrentTarget);
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
  kj::StringPtr type;
  kj::String ownType;
  Init init;
  bool trusted = true;
  bool stopped = false;
  bool preventedDefault = false;
  bool isBeingDispatched = false;
  bool propagationStopped = false;
  kj::Maybe<jsg::Ref<EventTarget>> target;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(target);
  }
};

class ExtendableEvent: public Event {
public:
  using Event::Event;

  // While ExtendableEvent is defined by the spec to be constructable, there's really not a
  // lot of reason currently to do so, especially with the restriction that waitUntil can
  // only be called on trusted events (which have to originate from within the system).
  static jsg::Ref<ExtendableEvent> constructor(kj::String type) = delete;

  void waitUntil(kj::Promise<void> promise);

  jsg::Optional<jsg::Ref<ActorState>> getActorState();

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

  static jsg::Ref<CustomEvent> constructor(jsg::Lock& js, kj::String type,
                                           jsg::Optional<CustomEventInit> init);

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
};

// An implementation of the Web Platform Standard EventTarget API
class EventTarget: public jsg::Object {
public:
  ~EventTarget() noexcept(false);

  size_t getHandlerCount(kj::StringPtr type) const;

  kj::Array<kj::StringPtr> getHandlerNames() const;

  bool dispatchEventImpl(jsg::Lock& js, jsg::Ref<Event> event);

  inline void removeAllHandlers() { typeMap.clear(); }

  inline void enableWarningOnSpecialEvents() { warnOnSpecialEvents = true; }

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

  typedef jsg::Function<jsg::Optional<jsg::Value>(jsg::Ref<Event>)> HandlerFunction;
  struct HandlerObject {
    HandlerFunction handleEvent;
    JSG_STRUCT(handleEvent);

    // TODO(cleanp): Get rid of this override and parse the type directly in param-extractor.rs
    JSG_STRUCT_TS_OVERRIDE({
      handleEvent: (event: Event) => any | undefined;
    });
  };
  typedef kj::OneOf<HandlerFunction, HandlerObject> Handler;

  void addEventListener(jsg::Lock& js, kj::String type, jsg::Identified<Handler> handler,
                        jsg::Optional<AddEventListenerOpts> maybeOptions);
  void removeEventListener(jsg::Lock& js, kj::String type, jsg::HashableV8Ref<v8::Object> handler,
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

  static jsg::Ref<EventTarget> constructor();

  // Registers a lambda that will be called when the given event type is emitted.
  // The handler will be registered for as long as the returned kj::Own<void>
  // handle is held. If the EventTarget is destroyed while the native handler handle
  // is held, it will be automatically detached.
  //
  // The caller must not do anything with the returned Own<void> except drop it. This is why it
  // is Own<void> and not Own<NativeHandler>.
  kj::Own<void> newNativeHandler(jsg::Lock& js,
      kj::String type,
      jsg::Function<void(jsg::Ref<Event>)> func,
      bool once = false);

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  // RAII-style listener that can be attached to an EventTarget.
  class NativeHandler {
  public:
    using Signature = void(jsg::Ref<Event>);
    NativeHandler(jsg::Lock& js, EventTarget& target, kj::String type,
        jsg::Function<Signature> func, bool once = false);
    ~NativeHandler() noexcept(false);
    KJ_DISALLOW_COPY_AND_MOVE(NativeHandler);

    void operator()(jsg::Lock&js, jsg::Ref<Event> event);

    uint hashCode() const;

    void visitForGc(jsg::GcVisitor& visitor);
  private:
    void detach();

    kj::String type;
    struct State {
      // target's destructor will null out `state`, so this is OK to be a bare reference.
      EventTarget& target;

      jsg::Function<Signature> func;
    };

    kj::Maybe<State> state;
    bool once;

    friend class EventTarget;
  };

  void addNativeListener(jsg::Lock& js, NativeHandler& handler);
  bool removeNativeListener(NativeHandler& handler);

  struct EventHandler {
    struct JavaScriptHandler {
      jsg::HashableV8Ref<v8::Object> identity;
      HandlerFunction callback;

      // If the event handler is registered with an AbortSignal, then the abortHandler points
      // at the NativeHandler representing that registration, so that if this object is GC'd before
      // the AbortSignal is signaleled, we unregister ourselves from listening on it. Note that
      // this is Own<void> for the same reason newNativeHandler() returns Own<void>: We are not
      // supposed to do anything with this except drop it.
      kj::Maybe<kj::Own<void>> abortHandler;

      void visitForGc(jsg::GcVisitor& visitor) {
        visitor.visit(identity, callback);

        // Note that we intentionally do NOT visit `abortHandler`. This is because the JS handles
        // held by `abortHandler` are not ever accessed by this path. Instead, they are accessed
        // by the AbortSignal, if and when it fires. So it is the AbortSignal's responsibility to
        // visit the NativeHandler's content.
      }

      kj::StringPtr jsgGetMemoryName() const { return "JavaScriptHandler"_kjc; }
      size_t jsgGetMemorySelfSize() const;
      void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
    };

    struct NativeHandlerRef {
      NativeHandler& handler;
    };

    // An EventHandler can be backed by either a JavaScript Handler (which is either a
    // function or an object) or a native handler. The insertion order matters here so
    // we maintain a single table.
    using Handler = kj::OneOf<JavaScriptHandler, NativeHandlerRef>;

    Handler handler;

    // When once is true, the handler will be removed after it is invoked one time.
    bool once = false;

    EventHandler(Handler handler, bool once)
        : handler(kj::mv(handler)), once(once) {}
    KJ_DISALLOW_COPY_AND_MOVE(EventHandler);

    kj::StringPtr jsgGetMemoryName() const { return "EventHandler"_kjc; }
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };

  struct EventHandlerHashCallbacks {
    const EventHandler::Handler& keyForRow(const kj::Own<EventHandler>& row) const;
    bool matches(const kj::Own<EventHandler>& a, const jsg::HashableV8Ref<v8::Object>& b) const;
    bool matches(const kj::Own<EventHandler>& a, const NativeHandler& b) const;
    bool matches(const kj::Own<EventHandler>& a, const EventHandler::NativeHandlerRef& b) const;
    bool matches(const kj::Own<EventHandler>& a, const EventHandler::Handler& b) const;
    uint hashCode(const jsg::HashableV8Ref<v8::Object>& obj) const;
    uint hashCode(const NativeHandler& handler) const;
    uint hashCode(const EventHandler::NativeHandlerRef& handler) const;
    uint hashCode(const EventHandler::JavaScriptHandler& handler) const;
    uint hashCode(const EventHandler::Handler& handler) const;
  };

  struct EventHandlerSet {
    kj::Table<kj::Own<EventHandler>,
              kj::HashIndex<EventHandlerHashCallbacks>,
              kj::InsertionOrderIndex> handlers;

    EventHandlerSet()
        : handlers(EventHandlerHashCallbacks(), {}) {}

    kj::StringPtr jsgGetMemoryName() const { return "EventHandlerSet"_kjc; }
    size_t jsgGetMemorySelfSize() const;
    void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const;
  };

  EventHandlerSet& getOrCreate(kj::StringPtr str) KJ_LIFETIMEBOUND;

  jsg::PropertyReflection<kj::OneOf<HandlerFunction, jsg::Value>> onEvents;

  kj::HashMap<kj::String, EventHandlerSet> typeMap;

  // When using module syntax, the "fetch", "scheduled", "trace", etc.
  // events are handled by exports rather than events. When warnOnSpecialEvents is true,
  // when using module syntax, attempts to register event handlers for these special
  // types of events will result in a warning being emitted.
  bool warnOnSpecialEvents = false;

  // Event handlers are not supposed to return values. The first time one does, we'll
  // emit a warning to help users debug things but we'll otherwise ignore it.
  bool warnOnHandlerReturn = true;

  void visitForGc(jsg::GcVisitor& visitor);

  friend class NativeHandler;
};

// An implementation of the Web Platform Standard AbortSignal API
class AbortSignal final: public EventTarget {
public:
  enum class Flag { NONE, NEVER_ABORTS };

  AbortSignal(kj::Maybe<kj::Exception> exception = kj::none,
              jsg::Optional<jsg::JsRef<jsg::JsValue>> maybeReason = kj::none,
              Flag flag = Flag::NONE);

  // The AbortSignal explicitly does not expose a constructor(). It is
  // illegal for user code to create an AbortSignal directly.
  static jsg::Ref<AbortSignal> constructor() = delete;

  bool getAborted() { return canceler->isCanceled(); }

  jsg::JsValue getReason(jsg::Lock& js);

  // Will synchronously throw an error if the abort signal has been triggered.
  void throwIfAborted(jsg::Lock& js);

  inline bool getNeverAborts() const { return flag == Flag::NEVER_ABORTS; }

  // The static abort() function here returns an AbortSignal that
  // has been pre-emptively aborted. It's useful when it might still
  // be desirable to kick off an async process while communicating
  // that it shouldn't continue.
  static jsg::Ref<AbortSignal> abort(
      jsg::Lock& js,
      jsg::Optional<jsg::JsValue> reason);

  // Returns an AbortSignal that is triggered after delay milliseconds.
  static jsg::Ref<AbortSignal> timeout(jsg::Lock& js, double delay);

  void triggerAbort(jsg::Lock& js,
                    jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> maybeReason);

  static jsg::Ref<AbortSignal> any(
      jsg::Lock& js,
      kj::Array<jsg::Ref<AbortSignal>> signals,
      const jsg::TypeHandler<EventTarget::HandlerFunction>& handler);

  // While AbortSignal extends EventTarget, and our EventTarget implementation will
  // automatically support onabort being set as an own property, the spec defines
  // onabort as a prototype property on the AbortSignal prototype. Therefore, we
  // need to explicitly set it as a prototype property here.
  kj::Maybe<jsg::JsValue> getOnAbort(jsg::Lock& js);
  void setOnAbort(jsg::Lock& js, jsg::Optional<jsg::JsValue> handler);

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
  }

  // Allows this AbortSignal to also serve as a kj::Canceler
  template <typename T>
  kj::Promise<T> wrap(kj::Promise<T> promise) {
    JSG_REQUIRE(!canceler->isCanceled(), TypeError, "The AbortSignal has already been triggered");
    return canceler->wrap(kj::mv(promise));
  }

  template <typename T>
  static kj::Promise<T> maybeCancelWrap(
      kj::Maybe<jsg::Ref<AbortSignal>>& signal,
      kj::Promise<T> promise) {
    KJ_IF_SOME(s, signal) {
      return s->wrap(kj::mv(promise));
    } else {
      return kj::mv(promise);
    }
  }

  static kj::Exception abortException(
      jsg::Lock& js,
      jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> reason = kj::none);

  RefcountedCanceler& getCanceler();

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    EventTarget::visitForMemoryInfo(tracker);
    tracker.trackInlineFieldWithSize("IoOwn<RefcountedCanceler>",
        sizeof(IoOwn<RefcountedCanceler>));
    tracker.trackField("reason", reason);
  }

private:
  IoOwn<RefcountedCanceler> canceler;
  Flag flag;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> reason;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> onAbortHandler;

  void visitForGc(jsg::GcVisitor& visitor);

  friend class AbortController;
};

// An implementation of the Web Platform Standard AbortController API
class AbortController final: public jsg::Object {
public:
  explicit AbortController()
      : signal(jsg::alloc<AbortSignal>()) {}

  static jsg::Ref<AbortController> constructor() {
    return jsg::alloc<AbortController>();
  }

  jsg::Ref<AbortSignal> getSignal() { return signal.addRef(); }

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
    // We have to be careful with gc here. The event listeners added to the AbortSignal
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
  kj::Promise<void> wait(
      jsg::Lock& js,
      double delay,
      jsg::Optional<WaitOptions> maybeOptions);

  JSG_RESOURCE_TYPE(Scheduler) {
    JSG_METHOD(wait);
  }

private:
};

#define EW_BASICS_ISOLATE_TYPES                \
    api::Event,                                \
    api::Event::Init,                          \
    api::EventTarget,                          \
    api::EventTarget::EventListenerOptions,    \
    api::EventTarget::AddEventListenerOptions, \
    api::EventTarget::HandlerObject,           \
    api::AbortController,                      \
    api::AbortSignal,                          \
    api::Scheduler,                            \
    api::Scheduler::WaitOptions,               \
    api::ExtendableEvent,                      \
    api::CustomEvent,                          \
    api::CustomEvent::CustomEventInit
// The list of basics.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
