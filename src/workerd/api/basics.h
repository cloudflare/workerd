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

  explicit Event(kj::String ownType, Init init = Init(), bool trusted = true)
      : type(ownType),
        ownType(kj::mv(ownType)),
        init(init),
        trusted(trusted) {}

  explicit Event(kj::StringPtr type, Init init = Init(), bool trusted = true)
      : type(type),
        init(init),
        trusted(trusted) {}

  bool isPreventDefault() { return preventedDefault; }
  void clearPreventDefault() { preventedDefault = false; }

  void beginDispatch(jsg::Ref<EventTarget> target);
  void endDispatch() { isBeingDispatched = false; }

  bool isStopped() { return stopped; }

  static jsg::Ref<Event> constructor(kj::String type, jsg::Optional<Init> init);
  kj::StringPtr getType();

  void stopImmediatePropagation() { stopped = true; }
  void preventDefault() { preventedDefault = true; }

  // The only phases we actually use are NONE and AT_TARGET but we provide
  // all of them to meet spec compliance.
  enum Phase {
    NONE,
    CAPTURING_PHASE,
    AT_TARGET,
    BUBBLING_PHASE,
  };

  int getEventPhase() const { return isBeingDispatched ? AT_TARGET : NONE; }

  // Much of the following is not used in our implementation of Event
  // simply because we do not support the notion of bubbled events
  // (events propagated up through a hierarchy of objects). They are
  // provided to fill-out Event spec compliance.

  bool getCancelBubble() { return propagationStopped; }
  void setCancelBubble(bool stopped) { propagationStopped = stopped; }
  void stopPropagation() { propagationStopped = true; }
  bool getComposed() { return init.composed.orDefault(false); }
  bool getBubbles() { return init.bubbles.orDefault(false); }
  bool getCancelable() { return init.cancelable.orDefault(false); }
  bool getDefaultPrevented() { return getCancelable() && preventedDefault; }
  bool getReturnValue() { return !getDefaultPrevented(); }

  // We provide the timeStamp property for spec compliance but we force
  // the value to 0.0 always because we really don't want users to rely
  // on this property for timing details.
  double getTimestamp() { return 0.0; }

  // What makes an Event trusted? It's pretty simple... any Event created
  // by EW internally is Trusted, any Event created using new Event() in JS
  // is not trusted.
  bool getIsTrusted() { return trusted; }

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

// An implementation of the Web Platform Standard EventTarget API
class EventTarget: public jsg::Object {
public:

  // RAII-style listener that can be attached to an EventTarget.
  class NativeHandler {
  public:
    using Signature = void(jsg::Ref<Event>);

    inline explicit NativeHandler(
        jsg::Lock& js,
        EventTarget& target,
        kj::String type,
        jsg::Function<Signature> func,
        bool once = false)
        : type(kj::mv(type)),
          target(target),
          func(kj::mv(func)),
          once(once) {
      KJ_ASSERT_NONNULL(this->target).addNativeListener(js, *this);
    }

    inline ~NativeHandler() noexcept(false) { detach(); }

    NativeHandler(NativeHandler&&) = default;
    NativeHandler& operator=(NativeHandler&&) = default;

    inline void detach(bool deferClearData = false) {
      KJ_IF_SOME(t, target) {
        t.removeNativeListener(*this);
        target = kj::none;
        // If deferClearData is true, we're going to wait to clear
        // the maybeData field until after the func is invoked. This
        // is because detach will be called immediately before the
        // operator() using the maybeData is called.
        if (!deferClearData) {
          auto drop KJ_UNUSED = kj::mv(func);
        }
      }
    }

    inline void operator()(jsg::Lock& js, jsg::Ref<Event> event) {
      if (once && !isAttached()) {
        // Arrange to drop the func after running it. Note that the function itself is allowed to
        // delete the NativeHandler, so we have to pull it off in advance, rather than after the
        // call.
        auto funcToDrop = kj::mv(func);
        funcToDrop(js, kj::mv(event));
      } else {
        func(js, kj::mv(event));
      }
    }

    inline bool isAttached() { return target != kj::none; }

    inline uint hashCode() const {
      return kj::hashCode(this);
    }

    // The visitForGc here must be called from EventTarget's visitForGc implementation.
    inline void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(func);
    }

  private:
    kj::String type;
    kj::Maybe<EventTarget&> target;
    jsg::Function<Signature> func;
    bool once;

    friend class EventTarget;
  };

  ~EventTarget() noexcept(false);

  size_t getHandlerCount(kj::StringPtr type) const;

  kj::Array<kj::StringPtr> getHandlerNames() const;

  bool dispatchEventImpl(jsg::Lock& js, jsg::Ref<Event> event);

  void removeAllHandlers() { typeMap.clear(); }

  void enableWarningOnSpecialEvents() { warnOnSpecialEvents = true; }

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

private:
  void addNativeListener(jsg::Lock& js, NativeHandler& handler);
  bool removeNativeListener(NativeHandler& handler);

  struct EventHandler {
    struct JavaScriptHandler {
      jsg::HashableV8Ref<v8::Object> identity;
      HandlerFunction callback;
      // If the event handler is registered with an AbortSignal, then the abortHandler
      // is set and will ensure that the handler is removed correctly.
      kj::Maybe<kj::Own<NativeHandler>> abortHandler;

      void visitForGc(jsg::GcVisitor& visitor) {
        visitor.visit(identity, callback);
      }
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
  };

  struct EventHandlerHashCallbacks {
    auto& keyForRow(const EventHandler& row) const {
      // The key for each EventHandler struct is the handler, which is a kj::OneOf
      // of either a JavaScriptHandler or NativeHandler.
      return row.handler;
    }

    inline bool matches(const EventHandler& a, const jsg::HashableV8Ref<v8::Object>& b) const {
      KJ_IF_SOME(jsA, a.handler.tryGet<EventHandler::JavaScriptHandler>()) {
        return jsA.identity == b;
      }
      return false;
    }

    inline bool matches(const EventHandler& a, const NativeHandler& b) const {
      KJ_IF_SOME(ref, a.handler.tryGet<EventHandler::NativeHandlerRef>()) {
        return &ref.handler == &b;
      }
      return false;
    }

    inline bool matches(const EventHandler& a, const EventHandler::NativeHandlerRef& b) const {
      return matches(a, b.handler);
    }

    inline bool matches(const EventHandler& a, const EventHandler::Handler& b) const {
      KJ_SWITCH_ONEOF(b) {
        KJ_CASE_ONEOF(jsB, EventHandler::JavaScriptHandler) {
          return matches(a, jsB.identity);
        }
        KJ_CASE_ONEOF(nativeB, EventHandler::NativeHandlerRef) {
          return matches(a, nativeB);
        }
      }
      KJ_UNREACHABLE;
    }

    inline uint hashCode(const jsg::HashableV8Ref<v8::Object>& obj) const {
      return obj.hashCode();
    }

    inline uint hashCode(const NativeHandler& handler) const {
      return handler.hashCode();
    }

    inline uint hashCode(const EventHandler::NativeHandlerRef& handler) const {
      return hashCode(handler.handler);
    }

    inline uint hashCode(const EventHandler::JavaScriptHandler& handler) const {
      return hashCode(handler.identity);
    }

    inline uint hashCode(const EventHandler::Handler& handler) const {
      KJ_SWITCH_ONEOF(handler) {
        KJ_CASE_ONEOF(js, EventHandler::JavaScriptHandler) {
          return hashCode(js);
        }
        KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
          return hashCode(native);
        }
      }
      KJ_UNREACHABLE;
    }
  };

  struct EventHandlerSet {
    kj::Table<EventHandler,
              kj::HashIndex<EventHandlerHashCallbacks>,
              kj::InsertionOrderIndex> handlers;

    EventHandlerSet()
        : handlers(EventHandlerHashCallbacks(), {}) {}
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
              Flag flag = Flag::NONE) :
      canceler(IoContext::current().addObject(
          kj::refcounted<RefcountedCanceler>(kj::cp(exception)))),
      flag(flag),
      reason(kj::mv(maybeReason)) {}

  // The AbortSignal explicitly does not expose a constructor(). It is
  // illegal for user code to create an AbortSignal directly.
  static jsg::Ref<AbortSignal> constructor() = delete;

  bool getAborted() { return canceler->isCanceled(); }

  jsg::JsValue getReason(jsg::Lock& js) {
    KJ_IF_SOME(r, reason) {
      return r.getHandle(js);
    }
    return js.undefined();
  }

  // Will synchronously throw an error if the abort signal has been triggered.
  void throwIfAborted(jsg::Lock& js);

  bool getNeverAborts() const { return flag == Flag::NEVER_ABORTS; }

  // The static abort() function here returns an AbortSignal that
  // has been pre-emptively aborted. It's useful when it might still
  // be desirable to kick off an async process while communicating
  // that it shouldn't continue.
  static jsg::Ref<AbortSignal> abort(
      jsg::Lock& js,
      jsg::Optional<jsg::JsValue> reason);

  // Returns an AbortSignal that is triggered after delay milliseconds.
  static jsg::Ref<AbortSignal> timeout(jsg::Lock& js, double delay);

  void triggerAbort(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason);

  static jsg::Ref<AbortSignal> any(
      jsg::Lock& js,
      kj::Array<jsg::Ref<AbortSignal>> signals,
      const jsg::TypeHandler<EventTarget::HandlerFunction>& handler);

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
      jsg::Optional<jsg::JsValue> reason = kj::none);

  RefcountedCanceler& getCanceler();

private:
  IoOwn<RefcountedCanceler> canceler;
  Flag flag;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> reason;

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
    api::ExtendableEvent
// The list of basics.h types that are added to worker.c++'s JSG_DECLARE_ISOLATE_TYPE

}  // namespace workerd::api
