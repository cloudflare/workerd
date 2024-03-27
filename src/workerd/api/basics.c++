// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor-state.h"
#include "basics.h"
#include "global-scope.h"
#include <kj/async.h>
#include <kj/vector.h>
#include <workerd/io/io-context.h>

namespace workerd::api {

namespace {
// "Special" events are the global addEventListener(...) events that the runtime itself
// will emit for various things (e.g. the "fetch" event). When using module syntax, these
// are not emitted as events and instead should be registered as functions on the exported
// handler. To help make that clearer, if user code calls addEventListener() using one of
// these special types (only when using module syntax), a warning will be logged to the
// console.
// It's important to keep this list in sync with any other top level events that are emitted
// when in worker syntax but called as exports in module syntax.
bool isSpecialEventType(kj::StringPtr type) {
  // TODO(someday): How should we cover custom events here? Since it's just for a warning I'm
  //   leaving them out for now.
  return type == "fetch" ||
         type == "scheduled" ||
         type == "tail" ||
         type == "trace" ||
         type == "alarm";
}
}  // namespace

EventTarget::NativeHandler::NativeHandler(
    jsg::Lock& js,
    EventTarget& target,
    kj::String type,
    jsg::Function<Signature> func,
    bool once)
    : type(kj::mv(type)),
      state(State {
        .target = target,
        .func = kj::mv(func),
      }),
      once(once) {
  target.addNativeListener(js, *this);
}

EventTarget::NativeHandler::~NativeHandler() noexcept(false) { detach(); }

void EventTarget::NativeHandler::operator()(jsg::Lock& js, jsg::Ref<Event> event) {
  KJ_IF_SOME(s, state) {
    if (once) {
      auto fn = kj::mv(s.func);
      detach();
      fn(js, kj::mv(event));
      // Note that the function may have detached itself and caused the NativeHandler
      // to be destroyed. Let's be careful not to touch it after this point.
    } else {
      s.func(js, kj::mv(event));
    }
    return;
  }
}

uint EventTarget::NativeHandler::hashCode() const {
  return kj::hashCode(this);
}

void EventTarget::NativeHandler::visitForGc(jsg::GcVisitor& visitor) {
  KJ_IF_SOME(s, state) {
    visitor.visit(s.func);
  }
}

void EventTarget::NativeHandler::detach() {
  KJ_IF_SOME(s, state) {
    s.target.removeNativeListener(*this);
    state = kj::none;
  }
}

kj::Own<void> EventTarget::newNativeHandler(
    jsg::Lock& js,
    kj::String type,
    jsg::Function<void(jsg::Ref<Event>)> func,
    bool once) {
  return kj::heap<EventTarget::NativeHandler>(js, *this, kj::mv(type), kj::mv(func), once);
}

const EventTarget::EventHandler::Handler& EventTarget::EventHandlerHashCallbacks::keyForRow(
    const kj::Own<EventHandler>& row) const {
  // The key for each EventHandler struct is the handler, which is a kj::OneOf
  // of either a JavaScriptHandler or NativeHandler.
  return row->handler;
}

bool EventTarget::EventHandlerHashCallbacks::matches(
    const kj::Own<EventHandler>& a,
    const jsg::HashableV8Ref<v8::Object>& b) const {
  KJ_IF_SOME(jsA, a->handler.tryGet<EventHandler::JavaScriptHandler>()) {
    return jsA.identity == b;
  }
  return false;
}

bool EventTarget::EventHandlerHashCallbacks::matches(
    const kj::Own<EventHandler>& a,
    const NativeHandler& b) const {
  KJ_IF_SOME(ref, a->handler.tryGet<EventHandler::NativeHandlerRef>()) {
    return &ref.handler == &b;
  }
  return false;
}

bool EventTarget::EventHandlerHashCallbacks::matches(
    const kj::Own<EventHandler>& a,
    const EventHandler::NativeHandlerRef& b) const {
  return matches(a, b.handler);
}

bool EventTarget::EventHandlerHashCallbacks::matches(
    const kj::Own<EventHandler>& a,
    const EventHandler::Handler& b) const {
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

uint EventTarget::EventHandlerHashCallbacks::hashCode(
    const jsg::HashableV8Ref<v8::Object>& obj) const {
  return obj.hashCode();
}

uint EventTarget::EventHandlerHashCallbacks::hashCode(const NativeHandler& handler) const {
  return handler.hashCode();
}

uint EventTarget::EventHandlerHashCallbacks::hashCode(
    const EventHandler::NativeHandlerRef& handler) const {
  return hashCode(handler.handler);
}

uint EventTarget::EventHandlerHashCallbacks::hashCode(
    const EventHandler::JavaScriptHandler& handler) const {
  return hashCode(handler.identity);
}

uint EventTarget::EventHandlerHashCallbacks::hashCode(const EventHandler::Handler& handler) const {
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

jsg::Ref<Event> Event::constructor(kj::String type, jsg::Optional<Init> init) {
  static const Init defaultInit;
  return jsg::alloc<Event>(kj::mv(type), init.orDefault(defaultInit), false /* not trusted */);
}

kj::StringPtr Event::getType() { return type; }

jsg::Optional<jsg::Ref<EventTarget>> Event::getCurrentTarget() {
  return target.map([&](jsg::Ref<EventTarget>& t) { return t.addRef(); });
}

kj::Array<jsg::Ref<EventTarget>> Event::composedPath() {
  if (isBeingDispatched) {
    // When isBeingDispatched is true, target should always be non-null.
    // If it's not, there's a bug that we need to know about.
    return kj::arr(KJ_ASSERT_NONNULL(target).addRef());
  }
  return kj::Array<jsg::Ref<EventTarget>>();
}

void Event::beginDispatch(jsg::Ref<EventTarget> target) {
  JSG_REQUIRE(!isBeingDispatched, DOMInvalidStateError, "The event is already being dispatched.");
  isBeingDispatched = true;
  this->target = kj::mv(target);
}

jsg::Ref<EventTarget> EventTarget::constructor() {
  return jsg::alloc<EventTarget>();
}

EventTarget::~EventTarget() noexcept(false) {
  for (auto& entry : typeMap) {
    for (auto& handler : entry.value.handlers) {
      KJ_IF_SOME(native, handler->handler.tryGet<EventHandler::NativeHandlerRef>()) {
        // Note: Can't call `detach()` here because it would loop back and call
        // `removeNativeListener()` on us, invalidating the `typeMap` iterator. We'll directly
        // null out the state.
        native.handler.state = kj::none;
      }
    }
  }
}

size_t EventTarget::getHandlerCount(kj::StringPtr type) const {
  KJ_IF_SOME(handlerSet, typeMap.find(type)) {
    return handlerSet.handlers.size();
  } else {
    return 0;
  }
}

kj::Array<kj::StringPtr> EventTarget::getHandlerNames() const {
  return KJ_MAP(entry, typeMap) { return entry.key.asPtr(); };
}

void EventTarget::addEventListener(jsg::Lock& js, kj::String type,
                                   jsg::Identified<Handler> handler,
                                   jsg::Optional<AddEventListenerOpts> maybeOptions) {
  if (warnOnSpecialEvents && isSpecialEventType(type)) {
    js.logWarning(
        kj::str("When using module syntax, the '", type, "' event handler should be "
                "declared as an exported function on the root module as opposed to using "
                "the global addEventListener()."));
  }

  js.withinHandleScope([&] {

    // Per the spec, the handler can be either a Function, or an object with a
    // handleEvent member function.
    HandlerFunction handlerFn = ([&]() {
      KJ_SWITCH_ONEOF(handler.unwrapped) {
        KJ_CASE_ONEOF(fn, HandlerFunction) {
          return kj::mv(fn);
        }
        KJ_CASE_ONEOF(obj, HandlerObject) {
          return kj::mv(obj.handleEvent);
        }
      }
      KJ_UNREACHABLE;
    })();

    bool once = false;
    kj::Maybe<jsg::Ref<AbortSignal>> maybeSignal;
    KJ_IF_SOME(value, maybeOptions) {
      KJ_SWITCH_ONEOF(value) {
        KJ_CASE_ONEOF(b, bool) {
          JSG_REQUIRE(!b, TypeError, "addEventListener(): useCapture must be false.");
        }
        KJ_CASE_ONEOF(opts, AddEventListenerOptions) {
          JSG_REQUIRE(!opts.capture.orDefault(false), TypeError,
                      "addEventListener(): options.capture must be false.");
          JSG_REQUIRE(!opts.passive.orDefault(false), TypeError,
                      "addEventListener(): options.passive must be false.");
          once = opts.once.orDefault(false);
          maybeSignal = kj::mv(opts.signal);
        }
      }
    }
    KJ_IF_SOME(signal, maybeSignal) {
      // If the AbortSignal has already been triggered, then we need to stop here.
      // Return without adding the event listener.
      if (signal->getAborted()) {
        return;
      }
    }

    auto& set = getOrCreate(type);

    auto maybeAbortHandler = maybeSignal.map([&](jsg::Ref<AbortSignal>& signal) {
      auto func = JSG_VISITABLE_LAMBDA(
          (this, type = kj::mv(type), handler = handler.identity.addRef(js)),
          (handler),
          (jsg::Lock& js, jsg::Ref<Event>) {
        removeEventListener(js, kj::mv(type), kj::mv(handler), kj::none);
      });

      return signal->newNativeHandler(js, kj::str("abort"), kj::mv(func), true);
    });

    auto eventHandler = kj::heap<EventHandler>(
      EventHandler::JavaScriptHandler {
        .identity = kj::mv(handler.identity),
        .callback = kj::mv(handlerFn),
        .abortHandler = kj::mv(maybeAbortHandler),
      }, once);

    set.handlers.upsert(kj::mv(eventHandler), [&](auto&&...) {});
  });
}

void EventTarget::removeEventListener(jsg::Lock& js, kj::String type,
                                      jsg::HashableV8Ref<v8::Object> handler,
                                      jsg::Optional<EventListenerOpts> maybeOptions) {
  KJ_IF_SOME(value, maybeOptions) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(b, bool) {
        JSG_REQUIRE(!b, TypeError, "removeEventListener(): useCapture must be false.");
      }
      KJ_CASE_ONEOF(opts, EventListenerOptions) {
        JSG_REQUIRE(!opts.capture.orDefault(false), TypeError,
                     "removeEventListener(): options.capture must be false.");
      }
    }
  }

  js.withinHandleScope([&] {
    KJ_IF_SOME(handlerSet, typeMap.find(type)) {
      handlerSet.handlers.eraseMatch(handler);
    }
  });
}

void EventTarget::addNativeListener(jsg::Lock& js, NativeHandler& handler) {
  auto& set = getOrCreate(handler.type);

  auto eventHandler = kj::heap<EventHandler>(
    EventHandler::NativeHandlerRef {
      .handler = handler,
    }, handler.once);

  set.handlers.upsert(kj::mv(eventHandler), [&](auto&&...) {});
}

bool EventTarget::removeNativeListener(EventTarget::NativeHandler& handler) {
  KJ_IF_SOME(handlerSet, typeMap.find(handler.type)) {
    return handlerSet.handlers.eraseMatch(handler);
  }
  return false;
}

EventTarget::EventHandlerSet& EventTarget::getOrCreate(kj::StringPtr type) {
  return typeMap.upsert(kj::str(type), EventHandlerSet(), [&](auto&&...) {}).value;
}

bool EventTarget::dispatchEventImpl(jsg::Lock& js, jsg::Ref<Event> event) {
  event->beginDispatch(JSG_THIS);
  KJ_DEFER(event->endDispatch());

  event->clearPreventDefault();

  // First, gather all the function handles that we plan to call. This is important to ensure that
  // the callback can add or remove listeners without affecting the current event's processing.

  return js.withinHandleScope([&] {
    struct Callback {
      EventHandler::Handler handler;
      bool once = false;
      bool oldStyle = false;
    };

    kj::Vector<Callback> callbacks;

    // Check if there is an `on<event>` property on this object. If so, we treat that as an event
    // handler, in addition to the ones registered with addEventListener().
    KJ_IF_SOME(onProp, onEvents.get(js, kj::str("on", event->getType()))) {
      // If the on-event is not a function, we silently ignore it rather than raise an error.
      KJ_IF_SOME(cb, onProp.tryGet<HandlerFunction>()) {
        callbacks.add(Callback {
          .handler = EventHandler::JavaScriptHandler {
            .identity = nullptr,  // won't be used below if oldStyle is true and once is false
            .callback = kj::mv(cb),
          },
          .oldStyle = true,
        });
      }
    }

    auto maybeHandlerSet = typeMap.find(event->getType());
    KJ_IF_SOME(handlerSet, maybeHandlerSet) {
      for (auto& handler: handlerSet.handlers.ordered<kj::InsertionOrderIndex>()) {
        KJ_SWITCH_ONEOF(handler->handler) {
          KJ_CASE_ONEOF(jsh, EventHandler::JavaScriptHandler) {
            callbacks.add(Callback {
              .handler = EventHandler::JavaScriptHandler {
                .identity = jsh.identity.addRef(js),
                .callback = jsh.callback.addRef(js)
              },
              .once = handler->once,
            });
          }
          KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
            callbacks.add(Callback {
              .handler = EventHandler::NativeHandlerRef {
                .handler = native.handler,
              },
              .once = handler->once,
            });
          }
        }
      }
    }

    const auto isRemoved = [&](auto& handler) {
      // This is not the most efficient way to do this but it's what works right now.
      // Instead of capturing direct references to the handler structs, we copy those
      // into the Callbacks vector, which means we need to look up the actual handler
      // again to see if it still exists in the list. The entire way the storage of the
      // handlers is done here can be improved to make this more efficient.
      auto& handlerSet = KJ_ASSERT_NONNULL(maybeHandlerSet);
      KJ_SWITCH_ONEOF(handler) {
        KJ_CASE_ONEOF(js, EventHandler::JavaScriptHandler) {
          return handlerSet.handlers.find(js.identity) == kj::none;
        }
        KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
          return handlerSet.handlers.find(native.handler) == kj::none;
        }
      }
      KJ_UNREACHABLE;
    };

    for (auto& callback: callbacks) {
      if (event->isStopped()) {
        // stopImmediatePropagation() was called; don't call any further listeners
        break;
      }

      // If the handler gets removed by an earlier run handler, then we need to
      // make sure we don't run it. Skip over and continue.
      if (!callback.oldStyle && isRemoved(callback.handler)) {
        continue;
      }

      if (callback.once) {
        KJ_SWITCH_ONEOF(callback.handler) {
          KJ_CASE_ONEOF(jsh, EventHandler::JavaScriptHandler) {
            removeEventListener(js, kj::str(event->getType()), jsh.identity.addRef(js), kj::none);
          }
          KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
            // The native handler will handle detaching itself when invoked
          }
        }
      }

      KJ_SWITCH_ONEOF(callback.handler) {
        KJ_CASE_ONEOF(jsh, EventHandler::JavaScriptHandler) {
          // Per the standard, the event listener is not supposed to return any value, and if it
          // does, that value is ignored. That can be somewhat problematic if the user passes an
          // async function as the event handler. Doing so counts as undefined behavior and can
          // introduce subtle and difficult to diagnose bugs. Here, if the handler does return a
          // value, we're going to emit a warning but otherwise ignore it. The warning will only
          // be emitted at most once per EventEmitter instance.
          auto ret = jsh.callback(js, event.addRef());
          // Note: We used to run each handler in its own v8::TryCatch. However, due to a
          //   misunderstanding of the V8 API, we incorrectly believed that TryCatch mishandled
          //   termination (or maybe it actually did at the time), so we changed things such that
          //   we don't catch exceptions so the first handler to throw an exception terminates the
          //   loop, and the exception flows out of dispatchEvent(). In theory if multiple
          //   handlers were registered then maybe we ought to be running all of them even if one
          //   fails. This isn't entirely clear, though: in the case of 'fetch' handlers, in
          //   fail-closed mode, an exception from any handler should make the whole request fail,
          //   but then who cares if the remaining handlers run? Meanwhile, in fail-open mode, for
          //   consistency, we should probably trigger fallback behavior if any handler throws, so
          //   again it doesn't matter. For other types of handlers, e.g. WebSocket 'message', it's
          //   not clear why one would ever register multiple handlers.
          if (warnOnHandlerReturn) KJ_IF_SOME(r, ret) {
            warnOnHandlerReturn = false;
            // To help make debugging easier, let's tailor the warning a bit if it was a promise.
            auto handle = r.getHandle(js);
            if (handle->IsPromise()) {
              js.logWarning(
                  kj::str("An event handler returned a promise that will be ignored. Event handlers "
                          "should not have a return value and should not be async functions."));
            } else {
              js.logWarning(
                  kj::str("An event handler returned a value of type \"",
                          handle->TypeOf(js.v8Isolate),
                          "\" that will be ignored. Event handlers should not have a return value."));
            }
          }
        }
        KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
          native.handler(js, event.addRef());
        }
      }
    }

    return !event->isPreventDefault();
  });
}

bool EventTarget::dispatchEvent(jsg::Lock& js, jsg::Ref<Event> event) {
  return dispatchEventImpl(js, kj::mv(event));
}

AbortSignal::AbortSignal(kj::Maybe<kj::Exception> exception,
                         jsg::Optional<jsg::JsRef<jsg::JsValue>> maybeReason,
                         Flag flag) :
    canceler(IoContext::current().addObject(
        kj::refcounted<RefcountedCanceler>(kj::cp(exception)))),
    flag(flag),
    reason(kj::mv(maybeReason)) {}

kj::Maybe<jsg::JsValue> AbortSignal::getOnAbort(jsg::Lock& js) {
  return onAbortHandler.map([&](jsg::JsRef<jsg::JsValue>& ref)
      -> jsg::JsValue { return ref.getHandle(js); });
}

void AbortSignal::setOnAbort(jsg::Lock& js, jsg::Optional<jsg::JsValue> handler) {
  // We only want to accept the handler if it's a valid handler... For anything
  // else, set it to null.
  KJ_IF_SOME(h, handler) {
    if (h.isFunction() || h.isObject()) {
      onAbortHandler = jsg::JsRef(js, h);
      return;
    } else {
      // TODO(soon): Per the spec we are supposed o set the handler to null if it is not
      // a function or an object. However, there's an ever so slight change that would
      // be breaking. So let's go ahead and set the value in this case and log a warning.
      // If we do not see any instances of the warning in logs, we can remove this and
      // go with the default behavior.
      LOG_WARNING_PERIODICALLY(
          "NOSENTRY AbortSignal::setOnAbort set to non-function/non-object value");
      onAbortHandler = jsg::JsRef(js, h);
      return;
    }
  }
  onAbortHandler = kj::none;
}

jsg::JsValue AbortSignal::getReason(jsg::Lock& js) {
  KJ_IF_SOME(r, reason) {
    return r.getHandle(js);
  }
  return js.undefined();
}

kj::Exception AbortSignal::abortException(
    jsg::Lock& js,
    jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> maybeReason) {
  KJ_IF_SOME(reason, maybeReason) {
    KJ_SWITCH_ONEOF(reason) {
      KJ_CASE_ONEOF(reason, jsg::JsValue) {
        return js.exceptionToKj(reason);
      }
      KJ_CASE_ONEOF(reason, kj::Exception) {
        return kj::cp(reason);
      }
    }
  }

  return JSG_KJ_EXCEPTION(DISCONNECTED, DOMAbortError, "The operation was aborted");
}

jsg::Ref<AbortSignal> AbortSignal::abort(
    jsg::Lock& js,
    jsg::Optional<jsg::JsValue> maybeReason) {
  auto exception = abortException(js, maybeReason);
  KJ_IF_SOME(reason, maybeReason) {
    return jsg::alloc<AbortSignal>(kj::mv(exception), reason.addRef(js));
  }
  return jsg::alloc<AbortSignal>(kj::cp(exception), js.exceptionToJsValue(kj::mv(exception)));
}

void AbortSignal::throwIfAborted(jsg::Lock& js) {
  if (canceler->isCanceled()) {
    KJ_IF_SOME(r, reason) {
      js.throwException(r.getHandle(js));
    } else {
      js.throwException(abortException(js));
    }
  }
}

jsg::Ref<AbortSignal> AbortSignal::timeout(jsg::Lock& js, double delay) {
  auto signal = jsg::alloc<AbortSignal>();

  auto context = js.v8Context();

  auto& global = jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(
      context, context->Global());

  // It's worth noting that the setTimeout holds a strong pointer to the AbortSignal,
  // keeping it from being garbage collected before the timer fires or until the request
  // completes, whichever comes first.

  global.setTimeoutInternal([signal = signal.addRef()](jsg::Lock& js) mutable {
    signal->triggerAbort(js, JSG_KJ_EXCEPTION(DISCONNECTED,
        DOMTimeoutError, "The operation was aborted due to timeout"));
  }, delay);

  return kj::mv(signal);
}

jsg::Ref<AbortSignal> AbortSignal::any(
    jsg::Lock& js,
    kj::Array<jsg::Ref<AbortSignal>> signals,
    const jsg::TypeHandler<EventTarget::HandlerFunction>& handler) {
  // If nothing was passed in, we can just return a signal that never aborts.
  if (signals.size() == 0) {
    return jsg::alloc<AbortSignal>(kj::none, kj::none, AbortSignal::Flag::NEVER_ABORTS);
  }

  // Let's check to see if any of the signals are already aborted. If it is, we can
  // optimize here by skipping the event handler registration.
  for (auto& sig : signals) {
    if (sig->getAborted()) {
      return AbortSignal::abort(js, sig->getReason(js));
    }
  }

  // Otherwise we need to create a new signal and register event handlers on all
  // of the signals that were passed in.
  auto signal = jsg::alloc<AbortSignal>();
  for (auto& sig : signals) {
    // This is a bit of a hack. We want to call addEventListener, but that requires a
    // jsg::Identified<EventTarget::Handler>, which we can't create directly yet.
    // So we create a jsg::Function, wrap that in a v8::Function, then convert that into
    // the jsg::Identified<EventTarget::Handler>, and voila, we have what we need.
    auto fn = js.wrapSimpleFunction(js.v8Context(),
        [&signal = *signal, &self = *sig](jsg::Lock& js, auto&) {
      // Note that we are not capturing any strong references here to either signal
      // or sig. This is because we are capturing a strong reference to the signal
      // when we add the event below. This ensures that we do not have an unbreakable
      // circular reference. The returned signal will not have any strong references
      // to any of the signals that are passed in, but each of the signals passed in
      // will have a strong reference to the new signal.
      signal.triggerAbort(js, self.getReason(js));
    });
    jsg::Identified<EventTarget::Handler> identified = {
      .identity = { js.v8Isolate, fn },
      .unwrapped = JSG_REQUIRE_NONNULL(handler.tryUnwrap(js, fn.As<v8::Value>()), TypeError,
          "Unable to create AbortSignal.any handler")
    };

    sig->addEventListener(js, kj::str("abort"), kj::mv(identified), AddEventListenerOptions {
      // Once the abort is triggered, this handler should remove itself.
      .once = true,
      // When the signal is triggered, we'll use it to cancel the other registered signals.
      .signal = signal.addRef()
    });
  }
  return signal;
}

void AbortSignal::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(reason, onAbortHandler);
}

RefcountedCanceler& AbortSignal::getCanceler() {
  return *canceler;
}

void AbortSignal::triggerAbort(
    jsg::Lock& js,
    jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> maybeReason) {
  KJ_ASSERT(flag != Flag::NEVER_ABORTS);
  if (canceler->isCanceled()) {
    return;
  }
  auto exception = AbortSignal::abortException(js, maybeReason);
  KJ_IF_SOME(r, maybeReason) {
    KJ_SWITCH_ONEOF(r) {
      KJ_CASE_ONEOF(value, jsg::JsValue) {
        reason = value.addRef(js);
      }
      KJ_CASE_ONEOF(ex, kj::Exception) {
        reason = js.exceptionToJsValue(kj::mv(ex));
      }
    }
  } else {
    reason = js.exceptionToJsValue(kj::mv(exception));
  }
  canceler->cancel(kj::cp(exception));

  // This is questionable only because it goes against the spec but it does help prevent
  // memory leaks. Once the abort signal has been triggered, there's really nothing else
  // the AbortSignal can be used for and no other events make sense. The user code could
  // add more, and could even emit their own events on the signal by using it as an
  // EventTarget directly but that would be rather silly, so stepping outside the lines
  // of the spec here should be just fine.
  KJ_DEFER(removeAllHandlers());

  dispatchEventImpl(js, jsg::alloc<Event>(kj::str("abort")));
}

void AbortController::abort(
    jsg::Lock& js,
    jsg::Optional<jsg::JsValue> maybeReason) {
  signal->triggerAbort(js, maybeReason);
}

void EventTarget::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& entry : typeMap) {
    for (auto& handler : entry.value.handlers) {
      KJ_SWITCH_ONEOF(handler->handler) {
        KJ_CASE_ONEOF(js, EventHandler::JavaScriptHandler) {
          visitor.visit(js);
        }
        KJ_CASE_ONEOF(native, EventHandler::NativeHandlerRef) {
          // Note that even though `native.handler` is a non-owned reference, we still need to
          // visit it. This is because we are the ones that will invoke the handles contained
          // in the native handler if it ever fires. The actual owner of the C++ NativeHandler
          // object doesn't ever access the JS objects it contains; the ownership relationship
          // exists only for RAII reasons, so that the NativeHandler is automatically unregistered
          // if the owner is destroyed.
          //
          // You might say: "Well, it's fine if the owner is responsible for visiting it, because
          // if the owner is no longer reachable then it will be destroyed and it will unregister
          // itself from here!" That doesn't quite work: V8's GC doesn't necessarily destroy
          // objects immediately when they become unreachable. However, it is no longer safe to
          // access an object once it is unreachable. Therefore, if we left it to the
          // NativeHandler's owner to visit the object, it's possible that the object becomes
          // poison some time before it is actually unregistered.
          //
          // Put another way, this is a very weird case where the C++ ownership and the JavaScript
          // ownership are different. We need GC visitation to follow the JavaScript ownership
          // graph.
          visitor.visit(native.handler);
        }
      }
    }
  }
}

kj::Promise<void> Scheduler::wait(
    jsg::Lock& js,
    double delay,
    jsg::Optional<WaitOptions> maybeOptions) {
  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(s, options.signal) {
      if (s->getAborted()) {
        return js.exceptionToKj(s->getReason(js));
      }
    }
  }

  // TODO(cleanup): Use jsg promise and resolver to avoid an unlock/relock. However, we need
  //   the abort signal to support wrapping jsg promises.
  auto paf = kj::newPromiseAndFulfiller<void>();

  auto context = js.v8Context();

  auto& global = jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(
      context, context->Global());
  global.setTimeoutInternal(
      [fulfiller = IoContext::current().addObject(kj::mv(paf.fulfiller))]
      (jsg::Lock& lock) mutable {
        fulfiller->fulfill();
      },
      delay);

  auto promise = kj::mv(paf.promise);

  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(s, options.signal) {
      promise = s->wrap(kj::mv(promise));
    }
  }

  return kj::mv(promise);
}

void ExtendableEvent::waitUntil(kj::Promise<void> promise) {
  JSG_REQUIRE(getIsTrusted(), DOMInvalidStateError,
             "waitUntil() can only be called on trusted event.");
  IoContext::current().addWaitUntil(kj::mv(promise));
}

jsg::Optional<jsg::Ref<ActorState>> ExtendableEvent::getActorState() {
  IoContext& context = IoContext::current();
  return context.getActor().map([&](Worker::Actor& actor) {
    auto& lock = context.getCurrentLock();
    auto persistent = actor.makeStorageForSwSyntax(lock);
    return jsg::alloc<api::ActorState>(
        actor.cloneId(),
        actor.getTransient(lock),
        kj::mv(persistent));
  });
}

CustomEvent::CustomEvent(kj::String ownType, CustomEventInit init)
    : Event(kj::mv(ownType), (Event::Init)init),
      detail(kj::mv(init.detail)) {}

jsg::Ref<CustomEvent> CustomEvent::constructor(jsg::Lock& js, kj::String type,
                                               jsg::Optional<CustomEventInit> init) {
  return jsg::alloc<CustomEvent>(kj::mv(type), kj::mv(init).orDefault({}));
}

jsg::Optional<jsg::JsValue> CustomEvent::getDetail(jsg::Lock& js) {
  return detail.map([&](jsg::JsRef<jsg::JsValue>& val) {
    return val.getHandle(js);
  });
}

CustomEvent::CustomEventInit::operator Event::Init() {
  return {
    .bubbles = this->bubbles.map([](auto& val) { return val; }),
    .cancelable = this->cancelable.map([](auto& val) { return val; }),
    .composed = this->composed.map([](auto& val) { return val; }),
  };
}

size_t EventTarget::EventHandler::JavaScriptHandler::jsgGetMemorySelfSize() const {
  return sizeof(JavaScriptHandler);
}

void EventTarget::EventHandler::JavaScriptHandler::jsgGetMemoryInfo(
    jsg::MemoryTracker& tracker) const {
  tracker.trackField("identity", identity);
  tracker.trackField("callback", callback);
  if (abortHandler != kj::none) {
    tracker.trackFieldWithSize("abortHandler", sizeof(kj::Own<NativeHandler>) +
                                                sizeof(NativeHandler));
  }
}

size_t EventTarget::EventHandler::jsgGetMemorySelfSize() const {
  return sizeof(EventHandler);
}

void EventTarget::EventHandler::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  KJ_SWITCH_ONEOF(handler) {
    KJ_CASE_ONEOF(js, JavaScriptHandler) {
      tracker.trackField("js", js);
    }
    KJ_CASE_ONEOF(native, NativeHandlerRef) {
      tracker.trackFieldWithSize("native", sizeof(NativeHandlerRef));
    }
  }
}

size_t EventTarget::EventHandlerSet::jsgGetMemorySelfSize() const {
  return sizeof(EventHandlerSet);
}

void EventTarget::EventHandlerSet::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& handler : handlers) {
    tracker.trackField("handler", handler);
  }
}

void EventTarget::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("typeMap", typeMap);
}

}  // namespace workerd::api
