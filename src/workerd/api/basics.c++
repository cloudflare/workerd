// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "basics.h"

#include "actor-state.h"
#include "global-scope.h"

#include <workerd/io/features.h>
#include <workerd/io/io-context.h>

#include <capnp/message.h>
#include <kj/async.h>
#include <kj/vector.h>

namespace workerd::api {

namespace {
constexpr kj::StringPtr kAbortEvent = "abort"_kj;
// "Special" events are the global addEventListener(...) events that the runtime itself
// will emit for various things (e.g. the "fetch" event). When using module syntax, these
// are not emitted as events and instead should be registered as functions on the exported
// handler. To help make that clearer, if user code calls addEventListener() using one of
// these special types (only when using module syntax), a warning will be logged to the
// console.
// It's important to keep this list in sync with any other top level events that are emitted
// when in worker syntax but called as exports in module syntax.
constexpr bool isSpecialEventType(kj::StringPtr type) {
  // TODO(someday): How should we cover custom events here? Since it's just for a warning I'm
  //   leaving them out for now.
  return type == "fetch" || type == "scheduled" || type == "tail" || type == "trace" ||
      type == "alarm";
}
}  // namespace

const jsg::HashableV8Ref<v8::Object>& EventTarget::EventHandlerHashCallbacks::keyForRow(
    const kj::Own<EventHandler>& row) const {
  return row->identity;
}

bool EventTarget::EventHandlerHashCallbacks::matches(
    const kj::Own<EventHandler>& a, const jsg::HashableV8Ref<v8::Object>& b) const {
  return a->identity == b;
}

uint EventTarget::EventHandlerHashCallbacks::hashCode(
    const jsg::HashableV8Ref<v8::Object>& obj) const {
  return obj.hashCode();
}

jsg::Ref<Event> Event::constructor(jsg::Lock& js, kj::String type, jsg::Optional<Init> init) {
  static const Init defaultInit;
  return js.alloc<Event>(kj::mv(type), init.orDefault(defaultInit), Trusted::NO);
}

kj::StringPtr Event::getType() {
  return type;
}

kj::Maybe<jsg::Ref<EventTarget>> Event::getCurrentTarget() {
  if (flags.isBeingDispatched) {
    return getTarget();
  }
  return kj::none;
}

jsg::Optional<jsg::Ref<EventTarget>> Event::getTarget() {
  return target.map([&](jsg::Ref<EventTarget>& t) { return t.addRef(); });
}

kj::Array<jsg::Ref<EventTarget>> Event::composedPath() {
  if (flags.isBeingDispatched) {
    // When isBeingDispatched is true, target should always be non-null.
    // If it's not, there's a bug that we need to know about.
    return kj::arr(KJ_ASSERT_NONNULL(target).addRef());
  }
  return kj::Array<jsg::Ref<EventTarget>>();
}

void Event::beginDispatch(jsg::Ref<EventTarget> target) {
  JSG_REQUIRE(
      !flags.isBeingDispatched, DOMInvalidStateError, "The event is already being dispatched.");
  flags.isBeingDispatched = true;
  this->target = kj::mv(target);
}

jsg::Ref<EventTarget> EventTarget::constructor(jsg::Lock& js) {
  return js.alloc<EventTarget>();
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

void EventTarget::addEventListener(jsg::Lock& js,
    kj::String type,
    kj::Maybe<jsg::Identified<Handler>> maybeHandler,
    jsg::Optional<AddEventListenerOpts> maybeOptions,
    const jsg::TypeHandler<jsg::Ref<EventTarget>>& eventTargetHandler) {
  if (flags.warnOnSpecialEvents && isSpecialEventType(type)) {
    js.logWarning(kj::str("When using module syntax, the '", type,
        "' event handler should be "
        "declared as an exported function on the root module as opposed to using "
        "the global addEventListener()."));
  }

  KJ_IF_SOME(handler, maybeHandler) {
    js.withinHandleScope([&] {
      // Per the spec, the handler can be either a Function, or an object with a
      // handleEvent member function.
      HandlerFunction handlerFn = ([&]() {
        KJ_SWITCH_ONEOF(handler.unwrapped) {
          KJ_CASE_ONEOF(fn, HandlerFunction) {
            if (FeatureFlags::get(js).getSetEventTargetThis()) {
              fn.setReceiver(js.v8Ref(eventTargetHandler.wrap(js, JSG_THIS)));
            }
            return kj::mv(fn);
          }
          KJ_CASE_ONEOF(obj, HandlerObject) {
            if (FeatureFlags::get(js).getSetEventTargetThis()) {
              obj.handleEvent.setReceiver(obj.self.asValue(js));
            }
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
        if (signal->getAborted(js)) {
          return;
        }
      }

      auto maybeAbortHandler = maybeSignal.map([&](jsg::Ref<AbortSignal>& signal) {
        // Per the spec's "add an event listener", the {signal} option registers an abort
        // algorithm — not an 'abort' listener — that removes this listener when the signal
        // aborts. The algorithm lives on the signal, so it captures this EventTarget weakly:
        // a strong ref would let a long-lived signal retain every dead target that ever
        // registered a listener with it, and a bare `this` would rely on the registration
        // handle below never outliving this target. If the target is gone by the time the
        // signal aborts, there is nothing left to remove. (The handle is still held by the
        // listener's own entry, so in the common case the algorithm is unregistered as soon
        // as the listener goes away.)
        auto func = JSG_VISITABLE_LAMBDA(
            (self = JSG_THIS_WEAK(js), type = type.clone(), handler = handler.identity.addRef(js)),
            (handler), (jsg::Lock& js) {
              KJ_IF_SOME(target, self.tryGet()) {
              target.removeEventListener(js, kj::mv(type), kj::mv(handler), kj::none);
              } else {
              }
            });

        return signal->addAbortAlgorithm(js, kj::mv(func));
      });

      auto eventHandler = kj::heap<EventHandler>(EventHandler{
        .identity = kj::mv(handler.identity),
        .callback = kj::mv(handlerFn),
        .once = once,
        .abortHandler = kj::mv(maybeAbortHandler),
      });

      auto& handlerSet = getOrCreate(type);
      auto sizeBefore = handlerSet.handlers.size();
      handlerSet.handlers.upsert(kj::mv(eventHandler), [&](auto&&...) {});
      if (handlerSet.handlers.size() != sizeBefore) {
        listenerCountChanged(js, type, handlerSet.handlers.size());
      }
    });
  }
}

void EventTarget::removeEventListener(jsg::Lock& js,
    kj::String type,
    kj::Maybe<jsg::HashableV8Ref<v8::Object>> maybeHandler,
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

  KJ_IF_SOME(handler, maybeHandler) {
    js.withinHandleScope([&] {
      KJ_IF_SOME(handlerSet, typeMap.find(type)) {
        if (handlerSet.handlers.eraseMatch(handler)) {
          listenerCountChanged(js, type, handlerSet.handlers.size());
        }
      }
    });
  }
}

EventTarget::EventHandlerSet& EventTarget::getOrCreate(kj::StringPtr type) {
  return typeMap.upsert(kj::str(type), EventHandlerSet(), [&](auto&&...) {}).value;
}

void EventTarget::addEventHandlerListener(jsg::Lock& js,
    kj::StringPtr type,
    jsg::HashableV8Ref<v8::Object> identity,
    HandlerFunction callback) {
  auto eventHandler = kj::heap<EventHandler>(EventHandler{
    .identity = kj::mv(identity),
    .callback = kj::mv(callback),
  });
  auto& handlerSet = getOrCreate(type);
  auto sizeBefore = handlerSet.handlers.size();
  handlerSet.handlers.upsert(kj::mv(eventHandler), [&](auto&&...) {});
  if (handlerSet.handlers.size() != sizeBefore) {
    listenerCountChanged(js, type, handlerSet.handlers.size());
  }
}

kj::Maybe<jsg::JsValue> EventTarget::getEventHandlerAttribute(jsg::Lock& js, kj::StringPtr type) {
  KJ_IF_SOME(attribute, eventHandlerAttributes.find(type)) {
    KJ_IF_SOME(handler, attribute.handler) {
      return handler.value.getHandle(js);
    }
  }
  return kj::none;
}

EventTarget::EventHandlerAssignment EventTarget::setEventHandlerAttribute(jsg::Lock& js,
    kj::StringPtr type,
    jsg::Optional<kj::OneOf<HandlerFunction, jsg::JsValue>> handler) {
  const auto getOrCreateAttribute = [&]() -> EventHandlerAttribute& {
    return eventHandlerAttributes.findOrCreate(
        type, [&] { return decltype(eventHandlerAttributes)::Entry{kj::str(type), {}}; });
  };

  // Per HTML's event handler semantics: callables (unwrapped as HandlerFunction) become the
  // active handler; non-callable objects are retained as the attribute value but are never
  // invoked; anything else deactivates the handler (treated as null).
  KJ_IF_SOME(h, handler) {
    KJ_SWITCH_ONEOF(h) {
      KJ_CASE_ONEOF(fn, HandlerFunction) {
        auto value = jsg::JsValue(
            KJ_ASSERT_NONNULL(fn.tryGetHandle(js.v8Isolate), "handler function has no wrapper"));
        auto& attribute = getOrCreateAttribute();
        attribute.handler = EventHandlerAttribute::Handler{
          .value = jsg::JsRef(js, value),
          .fn = kj::mv(fn),
        };
        activateEventHandlerAttribute(js, type, attribute);
        return EventHandlerAssignment::CALLABLE;
      }
      KJ_CASE_ONEOF(value, jsg::JsValue) {
        if (value.isObject()) {
          auto& attribute = getOrCreateAttribute();
          attribute.handler = EventHandlerAttribute::Handler{
            .value = jsg::JsRef(js, value),
            .fn = kj::none,
          };
          activateEventHandlerAttribute(js, type, attribute);
          return EventHandlerAssignment::OBJECT;
        }
      }
    }
  }

  // Deactivate: clear the value and remove the trampoline listener, so a later reassignment
  // takes a fresh position in the listener list. The map entry is kept: its presence is what
  // marks the type as managed.
  KJ_IF_SOME(attribute, eventHandlerAttributes.find(type)) {
    attribute.handler = kj::none;
    KJ_IF_SOME(identity, attribute.listenerIdentity) {
      removeEventListener(js, kj::str(type), identity.addRef(js), kj::none);
    }
    attribute.listenerIdentity = kj::none;
  }
  return EventHandlerAssignment::CLEARED;
}

void EventTarget::activateEventHandlerAttribute(
    jsg::Lock& js, kj::StringPtr type, EventHandlerAttribute& attribute) {
  // HTML "activate an event handler": if the trampoline listener already exists, the handler
  // keeps its current position in the listener list.
  if (attribute.listenerIdentity != kj::none) {
    return;
  }

  auto identity = jsg::HashableV8Ref<v8::Object>(js.v8Isolate, v8::Object::New(js.v8Isolate));
  attribute.listenerIdentity = identity.addRef(js);

  // The trampoline is deliberately not the handler itself: it invokes whatever value the
  // attribute holds at dispatch time, so reassignment need not (and must not) move it.
  auto trampoline = JSG_VISITABLE_LAMBDA((self = JSG_THIS_WEAK(js), type = kj::str(type)), (),
      (jsg::Lock & js, jsg::Ref<Event> event)->jsg::Optional<jsg::Value> {
        KJ_IF_SOME(target, self.tryGet()) {
        KJ_IF_SOME(attribute, target.eventHandlerAttributes.find(type)) {
        KJ_IF_SOME(handler, attribute.handler) {
        KJ_IF_SOME(fn, handler.fn) {
        return fn(js, kj::mv(event));
        } else {
        }  // Empty elses to squash compiler warnings
        } else {
        }
        } else {
        }
        } else {
        }
        return kj::none;
      });

  addEventHandlerListener(js, type, kj::mv(identity), kj::mv(trampoline));
}

namespace {

// Implements the reporting half of the spec's "inner invoke" step 11 for listener exceptions
// under DispatchExceptionPolicy::REPORT: deliver the exception to the global scope's
// report-an-exception machinery (which fires the cancelable 'error' event, then falls back
// to the console). Outside of a request (e.g. unit-test contexts without a
// ServiceWorkerGlobalScope), fall back to plain console/inspector reporting.
void reportListenerError(jsg::Lock& js, const jsg::JsValue& exception) {
  if (IoContext::hasCurrent()) {
    IoContext::current().getCurrentLock().getGlobalScope().reportError(js, exception);
  } else {
    js.reportError(exception);
  }
}

}  // namespace

EventTarget::DispatchResult EventTarget::dispatchEventImpl(
    jsg::Lock& js, jsg::Ref<Event> event, DispatchExceptionPolicy exceptionPolicy) {
  event->beginDispatch(JSG_THIS);
  KJ_DEFER(event->endDispatch());

  event->clearPreventDefault();

  kj::Maybe<jsg::JsRef<jsg::JsValue>> firstException;

  // First, gather all the function handles that we plan to call. This is important to ensure that
  // the callback can add or remove listeners without affecting the current event's processing.

  bool result = js.withinHandleScope([&] {
    struct Callback {
      // The listener's identity, used to check whether it was removed by an earlier handler
      // and to remove it when `once` is set. Old-style on<event> handlers (found via
      // property reflection rather than the listener list) have none.
      kj::Maybe<jsg::HashableV8Ref<v8::Object>> identity;
      HandlerFunction callback;
      bool once = false;
    };

    kj::Vector<Callback> callbacks;

    // Check if there is an `on<event>` property on this object. If so, we treat that as an event
    // handler, in addition to the ones registered with addEventListener(). This is skipped
    // for event types managed as event handler IDL attributes (see
    // setEventHandlerAttribute(), e.g. AbortSignal's onabort), whose handlers occupy a
    // positioned trampoline listener instead and would otherwise fire twice.
    if (!managesEventHandlerAttribute(event->getType())) {
      KJ_IF_SOME(onProp, onEvents.get(js, kj::str("on", event->getType()))) {
        // If the on-event is not a function, we silently ignore it rather than raise an error.
        KJ_IF_SOME(cb, onProp.tryGet<HandlerFunction>()) {
          callbacks.add(Callback{
            .identity = kj::none,
            .callback = kj::mv(cb),
          });
        }
      }
    }

    KJ_IF_SOME(handlerSet, typeMap.find(event->getType())) {
      callbacks.reserve(handlerSet.handlers.size());
      for (auto& handler: handlerSet.handlers.ordered<kj::InsertionOrderIndex>()) {
        callbacks.add(Callback{
          .identity = handler->identity.addRef(js),
          .callback = handler->callback.addRef(js),
          .once = handler->once,
        });
      }
    }

    const auto isRemoved = [&](jsg::HashableV8Ref<v8::Object>& identity) {
      // This is not the most efficient way to do this but it's what works right now.
      // Instead of capturing direct references to the handler structs, we copy those
      // into the Callbacks vector, which means we need to look up the actual handler
      // again to see if it still exists in the list. The entire way the storage of the
      // handlers is done here can be improved to make this more efficient.
      KJ_IF_SOME(handlerSet, typeMap.find(event->getType())) {
        return handlerSet.handlers.find(identity) == kj::none;
      }
      return true;
    };

    for (auto& callback: callbacks) {
      if (event->isStopped()) {
        // stopImmediatePropagation() was called; don't call any further listeners
        break;
      }

      KJ_IF_SOME(identity, callback.identity) {
        // If the handler was removed by an earlier-run handler, then we need to
        // make sure we don't run it. Skip over and continue.
        if (isRemoved(identity)) {
          continue;
        }

        // Per spec ("inner invoke" step 5), once-listeners are removed before invocation.
        if (callback.once) {
          removeEventListener(js, kj::str(event->getType()), identity.addRef(js), kj::none);
        }
      }

      const auto invoke = [&]() {
        // Per the standard, the event listener is not supposed to return any value, and
        // if it does, that value is ignored. That can be somewhat problematic if the user
        // passes an async function as the event handler. Doing so counts as undefined
        // behavior and can introduce subtle and difficult to diagnose bugs. Here, if the
        // handler does return a value, we're going to emit a warning but otherwise ignore
        // it. The warning will only be emitted at most once per EventTarget instance.
        auto ret = callback.callback(js, event.addRef());
        KJ_IF_SOME(r, ret) {
          auto handle = r.getHandle(js);
          // Returning true is the same as calling preventDefault() on the event.
          if (handle->IsTrue()) {
            event->preventDefault();
          }
          if (flags.warnOnHandlerReturn && !handle->IsBoolean()) {
            flags.warnOnHandlerReturn = false;
            // To help make debugging easier, let's tailor the warning a bit if it was a
            // promise.
            if (handle->IsPromise()) {
              js.logWarning(kj::str(
                  "An event handler returned a promise that will be ignored. Event handlers "
                  "should not have a return value and should not be async functions."));
            } else {
              js.logWarning(kj::str("An event handler returned a value of type \"",
                  handle->TypeOf(js.v8Isolate),
                  "\" that will be ignored. Event handlers should not have a return value."));
            }
          }
        }
      };

      switch (exceptionPolicy) {
        case DispatchExceptionPolicy::PROPAGATE:
          // The first handler to throw ends the dispatch and the exception flows out of
          // dispatchEventImpl(). The runtime's top-level event delivery depends on this:
          // for example, a throwing 'fetch' handler must fail the request (fail-closed)
          // or trigger fallback (fail-open) rather than let other handlers respond.
          invoke();
          break;
        case DispatchExceptionPolicy::REPORT:
          // Spec "inner invoke" step 11: report the exception and continue with the next
          // listener.
          JSG_TRY(js) {
            invoke();
          }
          JSG_CATCH(exception) {
            auto handle = jsg::JsValue(exception.getHandle(js));
            if (firstException == kj::none) {
              firstException = jsg::JsRef(js, handle);
            }
            reportListenerError(js, handle);
          }
          break;
      }
    }

    return !event->isPreventDefault();
  });

  return DispatchResult{.result = result, .firstException = kj::mv(firstException)};
}

EventTarget::DispatchExceptionPolicy EventTarget::effectiveExceptionPolicy(
    jsg::Lock& js, DispatchExceptionPolicy desired) {
  if (desired == DispatchExceptionPolicy::REPORT) {
    // Fall back to PROPAGATE when the compat flag is not set or when there is no active
    // Worker context (e.g. in C++ unit tests that use the JSG test harness directly).
    KJ_IF_SOME(flags, FeatureFlags::tryGet(js)) {
      if (!flags.getSpecCompliantDispatchExceptions()) {
        return DispatchExceptionPolicy::PROPAGATE;
      }
    } else {
      return DispatchExceptionPolicy::PROPAGATE;
    }
  }
  return desired;
}

bool EventTarget::dispatchEvent(jsg::Lock& js, jsg::Ref<Event> event) {
  // The JS-exposed dispatchEvent() is a spec surface: listener exceptions are reported and
  // do not interrupt the dispatch (nor propagate to the dispatchEvent() caller).
  return dispatchEventImpl(
      js, kj::mv(event), effectiveExceptionPolicy(js, DispatchExceptionPolicy::REPORT))
      .result;
}

// A wrapper for the AbortTrigger jsrpc client, that automatically sends a release() message once
// the client is destroyed, informing the server that an abort will not be triggered in the future.
class AbortTriggerRpcClient final {
 public:
  AbortTriggerRpcClient(rpc::AbortTrigger::Client&& client): client(kj::mv(client)) {}

  kj::Promise<void> abort(kj::ArrayPtr<kj::byte> reason) {
    auto req = client.abortRequest(capnp::MessageSize{reason.size() / sizeof(capnp::word) + 8, 0});
    auto field = req.initReason();
    field.setV8Serialized(reason);
    return req.sendIgnoringResult();
  }

  ~AbortTriggerRpcClient() noexcept(false) {
    if (skipReleaseForTest) {
      return;
    }

    auto req = client.releaseRequest(capnp::MessageSize{4, 0});
    // We call detach() on the resulting promise so that we can perform RPC in a destructor
    req.sendIgnoringResult().detach([](kj::Exception exc) {
      if (exc.getType() == kj::Exception::Type::DISCONNECTED) {
        // It's possible we can't send the release message because we're already disconnected.
        return;
      };

      // Other exceptions could be more interesting
      LOG_EXCEPTION("abortTriggerReleaseRpc", exc);
    });
  }

  bool skipReleaseForTest = false;

 private:
  rpc::AbortTrigger::Client client;
};

namespace {

kj::Promise<void> abortRpcClientTask(
    IoContext& ioContext, AbortTriggerRpcClient& client, kj::Array<kj::byte> reason) {
  KJ_IF_SOME(outputLocks, ioContext.waitForOutputLocksIfNecessary()) {
    co_await outputLocks;
  }
  co_await client.abort(reason);
}

// Sends the serialized abort reason to one RPC clone as a task on the current IoContext,
// which must be the context that owns the client.
void sendAbortToRpc(IoOwn<AbortTriggerRpcClient> client, kj::Array<kj::byte> reason) {
  auto& ioContext = IoContext::current();
  // Dereference the IoOwn here, while the owning context is known to be current; the task
  // keeps the client alive by holding the IoOwn as an attachment.
  auto& clientRef = *client;
  ioContext.addTask(
      abortRpcClientTask(ioContext, clientRef, kj::mv(reason)).attach(kj::mv(client)));
}

}  // namespace

AbortSignal::AbortSignal(kj::Maybe<kj::Exception> exception,
    jsg::Optional<jsg::JsRef<jsg::JsValue>> maybeReason,
    Flag flag)
    : flag(flag),
      maybeAbortException(kj::mv(exception)),
      reason(kj::mv(maybeReason)) {}

kj::Maybe<jsg::JsValue> AbortSignal::getOnAbort(jsg::Lock& js) {
  return getEventHandlerAttribute(js, kAbortEvent);
}

void AbortSignal::setOnAbort(
    jsg::Lock& js, jsg::Optional<kj::OneOf<EventTarget::HandlerFunction, jsg::JsValue>> handler) {
  // The trampoline's activation registers a regular 'abort' listener, which arms the RPC
  // abort subscription through listenerCountChanged().
  setEventHandlerAttribute(js, kAbortEvent, kj::mv(handler));
}

void AbortSignal::listenerCountChanged(jsg::Lock& js, kj::StringPtr type, size_t count) {
  // Only 'abort' listeners can observe an abort; registrations for other event types must
  // not arm the RPC subscription (whose pending awaitIo blocks actor hibernation). A
  // notification that leaves 'abort' listeners registered arms too — arming is idempotent,
  // and any such notification means an abort could still be observed.
  if (type == kAbortEvent && count > 0) {
    subscribeToRpcAbort(js);
  }
}

bool AbortSignal::getAborted(jsg::Lock& js) {
  return maybeAbortException != kj::none || hasPendingReason();
}

jsg::JsValue AbortSignal::getReason(jsg::Lock& js) {
  KJ_IF_SOME(r, reason) {
    return r.getHandle(js);
  }

  KJ_IF_SOME(r, deserializePendingReason(js)) {
    return r;
  }

  return js.undefined();
}

kj::Exception AbortSignal::abortException(
    jsg::Lock& js, const jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>>& maybeReason) {
  KJ_IF_SOME(reason, maybeReason) {
    KJ_SWITCH_ONEOF(reason) {
      KJ_CASE_ONEOF(reason, jsg::JsValue) {
        return js.exceptionToKj(reason);
      }
      KJ_CASE_ONEOF(reason, kj::Exception) {
        return reason.clone();
      }
    }
  }

  return JSG_KJ_EXCEPTION(DISCONNECTED, DOMAbortError, "The operation was aborted");
}

jsg::Ref<AbortSignal> AbortSignal::abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  auto exception = abortException(js, maybeReason);
  KJ_IF_SOME(reason, maybeReason) {
    return js.alloc<AbortSignal>(kj::mv(exception), reason.addRef(js));
  }
  return js.alloc<AbortSignal>(exception.clone(), js.exceptionToJsValue(kj::mv(exception)));
}

void AbortSignal::throwIfAborted(jsg::Lock& js) {
  if (maybeAbortException != kj::none) {
    KJ_IF_SOME(r, reason) {
      js.throwException(r.getHandle(js));
    } else {
      js.throwException(abortException(js, kj::none));
    }
  }

  KJ_IF_SOME(r, deserializePendingReason(js)) {
    js.throwException(r);
  }
}

jsg::Ref<AbortSignal> AbortSignal::timeout(jsg::Lock& js, double delay) {
  auto signal = js.alloc<AbortSignal>();

  auto context = js.v8Context();

  auto& global =
      jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(context, context->Global());

  // It's worth noting that the setTimeout holds a strong pointer to the AbortSignal,
  // keeping it from being garbage collected before the timer fires or until the request
  // completes, whichever comes first.

  global.setTimeoutInternal([signal = signal.addRef()](jsg::Lock& js) mutable {
    signal->triggerAbort(js,
        JSG_KJ_EXCEPTION(
            DISCONNECTED, DOMTimeoutError, "The operation was aborted due to timeout"));
  }, delay);

  return kj::mv(signal);
}

jsg::Ref<AbortSignal> AbortSignal::any(jsg::Lock& js, kj::Array<jsg::Ref<AbortSignal>> signals) {
  // Implements the spec's "create a dependent abort signal".

  // If nothing was passed in, we can just return a signal that never aborts.
  if (signals.size() == 0) {
    return js.alloc<AbortSignal>(kj::none, kj::none, AbortSignal::Flag::NEVER_ABORTS);
  }

  // Spec step 2: if any of the signals is already aborted, return an already-aborted signal
  // carrying its reason; nothing gets linked. The spec sets the result's abort reason to the
  // source's, so the result adopts the source's settled state rather than deriving a fresh one
  // from the reason object. Deriving would run that object's getters again — from what is
  // otherwise a pure query — and would reclassify the exception, leaving the result aborting
  // differently from both its source and a dependent that had been linked to that source
  // before it aborted.
  for (auto& sig: signals) {
    if (sig->getAborted(js)) {
      auto abortReason = sig->getReason(js);
      KJ_IF_SOME(exception, sig->maybeAbortException) {
        return js.alloc<AbortSignal>(exception.clone(), abortReason.addRef(js));
      }
      // The signal is aborted only by way of a reason that arrived over RPC and has not been
      // applied yet, so there is no settled exception to adopt; derive one from the reason.
      return AbortSignal::abort(js, abortReason);
    }
  }

  auto resultSignal = js.alloc<AbortSignal>();
  resultSignal->dependent = true;

  // Links resultSignal as a dependent of the given (never itself dependent) source.
  // Duplicate links are harmless: a dependent is only aborted once, so extra entries are
  // skipped at trigger time — matching the spec's set semantics observably.
  const auto linkToSource = [&](AbortSignal& source) {
    source.dependentSignals.add(resultSignal.addRef());
    resultSignal->sourceSignals.add(source.getWeakRefToThis<AbortSignal>(js));

    // A dependent must observe aborts that arrive for the source over RPC, just like any
    // other abort observer.
    source.subscribeToRpcAbort(js);
  };

  // Spec step 4, including the flattening rule: a source that is itself dependent
  // contributes its own sources rather than itself, so dependency chains never form. (A
  // dead flattened source can never abort and so contributes nothing.)
  for (auto& sig: signals) {
    if (!sig->dependent) {
      linkToSource(*sig);
    } else {
      for (auto& weakSource: sig->sourceSignals) {
        KJ_IF_SOME(source, weakSource.tryGet()) {
          linkToSource(source);
        }
      }
    }
  }

  return resultSignal;
}

void AbortSignal::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(reason);
  for (auto& algorithm: abortAlgorithms) {
    visitor.visit(algorithm.fn);
  }
  for (auto& dep: dependentSignals) {
    visitor.visit(dep);
  }
}

namespace {

// Takes a cell's slot content, leaving the slot empty. Callers must use the taken value
// only after this returns, i.e. after the cell's mutex has been released.
template <typename T>
kj::Maybe<T> take(const kj::MutexGuarded<kj::Maybe<T>>& slot) {
  auto lock = slot.lockExclusive();
  auto value = kj::mv(*lock);
  *lock = kj::none;
  return value;
}

// Reclaims registration cells whose consumer is gone (slot already cleared) or whose owning
// IoContext has been destroyed (the slot content could never be used anyway; clearing it
// here is safe from any thread because dropping an IoOwn on a defunct context is a no-op and
// abort actions capture nothing needing the owner). Called on registration paths so that
// growth on a long-lived signal is bounded by its live registrations. `slotOf` maps a cell
// to its guarded slot.
template <typename T, typename SlotOf>
void sweepCells(kj::Vector<kj::Arc<T>>& cells, SlotOf slotOf) {
  size_t dst = 0;
  for (size_t i = 0; i < cells.size(); i++) {
    bool dead = [&]() {
      auto lock = slotOf(*cells[i]).lockExclusive();
      if (*lock == kj::none) {
        return true;
      }
      if (cells[i]->executor.isTargetDestroyed()) {
        *lock = kj::none;
        return true;
      }
      return false;
    }();
    if (!dead) {
      if (dst != i) {
        cells[dst] = kj::mv(cells[i]);
      }
      ++dst;
    }
  }
  cells.truncate(dst);
}

}  // namespace

kj::Own<void> AbortSignal::addAbortAction(
    jsg::Lock& js, kj::Function<void(jsg::Lock&, const kj::Exception&)> action) {
  // The RAII registration handle. Clearing the cell's slot is the only thing it does, which
  // makes it safe to drop from any thread; the emptied cell itself is removed from the
  // signal by a later sweep under the isolate lock.
  class Registration final {
   public:
    Registration(kj::Arc<RegistrationCell> cell): cell(kj::mv(cell)) {}
    ~Registration() noexcept(false) {
      *cell->action.lockExclusive() = kj::none;
    }
    KJ_DISALLOW_COPY_AND_MOVE(Registration);

   private:
    kj::Arc<RegistrationCell> cell;
  };

  if (getNeverAborts()) {
    return kj::Own<void>();
  }

  // Abort actions observe aborts, including ones arriving over RPC for a deserialized
  // signal; this is the single arming point for every native registration path (wrap(),
  // newCanceler(), and direct callers alike).
  subscribeToRpcAbort(js);

  auto& ioContext = IoContext::current();

  sweepCells(nativeRegistrations, [](auto& cell) -> auto& { return cell.action; });

  auto cell = kj::arc<RegistrationCell>(ioContext.getCrossContextExecutor(), kj::mv(action));
  nativeRegistrations.add(cell.addRef());
  return kj::heap<Registration>(kj::mv(cell));
}

kj::Own<void> AbortSignal::addAbortAlgorithm(jsg::Lock& js, jsg::Function<void()> algorithm) {
  // The RAII registration handle. It holds only a weak reference: if the signal is already
  // gone (or aborted, which empties the algorithm list), unregistration is a no-op.
  class Registration final {
   public:
    Registration(jsg::WeakRef<AbortSignal> signal, uint64_t token)
        : signal(kj::mv(signal)),
          token(token) {}
    ~Registration() noexcept(false) {
      KJ_IF_SOME(s, signal.tryGet()) {
        s.removeAbortAlgorithm(token);
      }
    }
    KJ_DISALLOW_COPY_AND_MOVE(Registration);

   private:
    jsg::WeakRef<AbortSignal> signal;
    uint64_t token;
  };

  if (getNeverAborts()) {
    return kj::Own<void>();
  }

  subscribeToRpcAbort(js);

  auto token = nextAbortAlgorithmToken++;
  abortAlgorithms.add(AbortAlgorithm{.token = token, .fn = kj::mv(algorithm)});
  return kj::heap<Registration>(JSG_THIS_WEAK(js), token);
}

void AbortSignal::removeAbortAlgorithm(uint64_t token) {
  // Linear scan-and-shift: the list is short and the remaining entries' order must be
  // preserved (algorithms run in registration order).
  for (size_t i = 0; i < abortAlgorithms.size(); i++) {
    if (abortAlgorithms[i].token == token) {
      for (size_t j = i; j + 1 < abortAlgorithms.size(); j++) {
        abortAlgorithms[j] = kj::mv(abortAlgorithms[j + 1]);
      }
      abortAlgorithms.removeLast();
      return;
    }
  }
}

kj::Own<void> AbortSignal::registerPendingCancellation(jsg::Lock& js, ReleasingCanceler& canceler) {
  // Capturing by reference is safe: the returned handle guarantees the action never runs
  // once the handle has been destroyed, and holders destroy the handle before the canceler.
  return addAbortAction(js,
      [&canceler](jsg::Lock& js, const kj::Exception& exception) { canceler.cancel(exception); });
}

AbortSignal::Cancellation AbortSignal::newCanceler(jsg::Lock& js) {
  if (getNeverAborts()) {
    return {
      .canceler = kj::heap<ReleasingCanceler>(),
      .registration = kj::Own<void>(),
    };
  }

  if (getAborted(js)) {
    // Already aborted: hand back a pre-canceled canceler; there is no future abort to hook.
    auto exception = [&]() -> kj::Exception {
      KJ_IF_SOME(e, maybeAbortException) {
        return e.clone();
      }
      KJ_IF_SOME(r, deserializePendingReason(js)) {
        return js.exceptionToKj(r);
      }
      KJ_UNREACHABLE;
    }();
    return {
      .canceler = kj::heap<ReleasingCanceler>(kj::mv(exception)),
      .registration = kj::Own<void>(),
    };
  }

  auto canceler = kj::heap<ReleasingCanceler>();
  auto registration = registerPendingCancellation(js, *canceler);
  return {
    .canceler = kj::mv(canceler),
    .registration = kj::mv(registration),
  };
}

void AbortSignal::setAbortState(
    jsg::Lock& js, jsg::JsRef<jsg::JsValue> newReason, kj::Exception exception) {
  reason = kj::mv(newReason);
  maybeAbortException = kj::mv(exception);
}

void AbortSignal::severSources(jsg::Lock& js) {
  auto sources = kj::mv(sourceSignals);
  for (auto& weakSource: sources) {
    KJ_IF_SOME(source, weakSource.tryGet()) {
      auto& deps = source.dependentSignals;
      for (size_t i = 0; i < deps.size(); i++) {
        if (deps[i].get() == this) {
          for (size_t j = i; j + 1 < deps.size(); j++) {
            deps[j] = kj::mv(deps[j + 1]);
          }
          deps.removeLast();
          break;
        }
      }
    }
  }
}

void AbortSignal::triggerAbort(
    jsg::Lock& js, jsg::Optional<kj::OneOf<kj::Exception, jsg::JsValue>> maybeReason) {
  KJ_ASSERT(flag != Flag::NEVER_ABORTS);
  if (maybeAbortException != kj::none || aborting) {
    return;
  }

  // Spec "signal abort" steps 1-2: settle the abort state.
  //
  // Deriving the native form of a JS reason reads properties off it, so a getter or a Proxy
  // trap can run arbitrary JS here — including JS that calls abort() again. The latch is
  // armed before the derivation, not after, so that such a call finds an abort already under
  // way and returns, leaving this one to decide the outcome. If the derivation throws,
  // nothing has been recorded, so the latch is released and the signal stays abortable.
  aborting = true;
  KJ_ON_SCOPE_FAILURE(aborting = false);

  auto exception = abortException(js, maybeReason);
  auto reasonRef = [&]() -> jsg::JsRef<jsg::JsValue> {
    KJ_IF_SOME(r, maybeReason) {
      KJ_SWITCH_ONEOF(r) {
        KJ_CASE_ONEOF(value, jsg::JsValue) {
          return value.addRef(js);
        }
        KJ_CASE_ONEOF(ex, kj::Exception) {
          return js.exceptionToJsValue(kj::mv(ex));
        }
      }
    }
    return js.exceptionToJsValue(exception.clone());
  }();
  setAbortState(js, kj::mv(reasonRef), kj::mv(exception));

  // Spec steps 3-4: record the same state on every not-yet-aborted dependent NOW — before any
  // abort steps or events run anywhere; their own abort steps run only after ours complete
  // (step 6). Handing each dependent the state settled above keeps this loop free of JS, so
  // nothing can abort a dependent or re-enter severSources() while it is walked, and every
  // dependent reports the reason and rejects with the exception this signal did. A signal
  // linked more than once (e.g. any([s, s])) fails the not-yet-aborted check on the second
  // encounter and so is collected, and aborted, only once. Collect with fresh strong
  // addRef()s rather than by moving the stored refs: the stored refs are GC-traced, and a ref
  // moved out of its visited home would leave the dependent's wrapper collectable by any GC
  // that runs during the abort steps below. Clearing the member also severs the links: an
  // aborted signal has no further use for its dependents, and any() never links to an aborted
  // source, so nothing new can arrive.
  kj::Vector<jsg::Ref<AbortSignal>> dependentsToAbort;
  auto reasonHandle = KJ_ASSERT_NONNULL(reason).getHandle(js);
  auto& settled = KJ_ASSERT_NONNULL(maybeAbortException);
  for (auto& dep: dependentSignals) {
    if (dep->maybeAbortException == kj::none) {
      dep->setAbortState(js, reasonHandle.addRef(js), settled.clone());
      dependentsToAbort.add(dep.addRef());
    }
  }
  dependentSignals.clear();

  // Spec step 5: run our own abort steps.
  runAbortSteps(js);

  // Spec step 6: run each collected dependent's abort steps, unlinking each from any other
  // sources it still has (those can no longer abort it, nor need they keep it alive).
  for (auto& dep: dependentsToAbort) {
    dep->severSources(js);
    dep->runAbortSteps(js);
  }
}

void AbortSignal::runAbortSteps(jsg::Lock& js) {
  auto& exception = KJ_ASSERT_NONNULL(maybeAbortException);

  // 1. Abort algorithms (spec "run the abort steps", steps 1-2): run each algorithm in
  //    registration order, then empty the list. The functions are copied out with fresh
  //    strong addRef()s (same pattern as dispatchEventImpl): the stored ones are GC-traced,
  //    and an algorithm may itself run JS — and therefore GC — while later entries await
  //    their turn. Emptying the list first also makes the loop safe against re-entrant
  //    mutation (e.g. an algorithm's effects dropping another algorithm's registration).
  if (!abortAlgorithms.empty()) {
    kj::Vector<jsg::Function<void()>> algorithms;
    algorithms.reserve(abortAlgorithms.size());
    for (auto& algorithm: abortAlgorithms) {
      algorithms.add(algorithm.fn.addRef(js));
    }
    abortAlgorithms.clear();
    for (auto& algorithm: algorithms) {
      algorithm(js);
    }
  }

  // 2. Native cancellations (canceler-wrapped promises and abort actions). Each registration
  //    runs in the IoContext that created it: synchronously if that context is the current
  //    one, otherwise delivered on that context's next turn — or dropped, if that context is
  //    already gone (in which case everything it wanted to cancel died with it). Taking the
  //    vector up front makes this safe against re-entrant registration.
  //
  //    A deferred delivery does not take the action with it: it re-takes the cell's slot on
  //    arrival in the owning context, so that a consumer (and the references its action
  //    captures) that went away in the meantime reliably turns the delivery into a no-op.
  auto cells = kj::mv(nativeRegistrations);
  for (auto& cell: cells) {
    if (cell->executor.isCurrent()) {
      KJ_IF_SOME(action, take(cell->action)) {
        action(js, exception);
      }
    } else {
      cell->executor.tryExecute(
          [cell = cell.addRef(), ex = exception.clone()](jsg::Lock& js) mutable {
        KJ_IF_SOME(action, take(cell->action)) {
          action(js, ex);
        }
      });
    }
  }

  // 3. Dispatch to RPC clients, with the same per-registration routing and re-take.
  if (!rpcRegistrations.empty()) {
    auto regs = kj::mv(rpcRegistrations);

    // Serialize the reason once; each clone gets its own copy of the bytes.
    jsg::Serializer ser(js);
    KJ_IF_SOME(r, reason) {
      ser.write(js, r.getHandle(js));
    }
    auto released = ser.release();

    for (auto& reg: regs) {
      auto bytes = kj::heapArray<kj::byte>(released.data);
      if (reg->executor.isCurrent()) {
        KJ_IF_SOME(client, take(reg->client)) {
          sendAbortToRpc(kj::mv(client), kj::mv(bytes));
        }
      } else {
        reg->executor.tryExecute(
            [reg = reg.addRef(), bytes = kj::mv(bytes)](jsg::Lock& js) mutable {
          KJ_IF_SOME(client, take(reg->client)) {
            sendAbortToRpc(kj::mv(client), kj::mv(bytes));
          }
        });
      }
    }
  }

  // 4. Dispatch to local listeners

  // This is questionable only because it goes against the spec but it does help prevent
  // memory leaks. Once the abort signal has been triggered, there's really nothing else
  // the AbortSignal can be used for and no other events make sense. The user code could
  // add more, and could even emit their own events on the signal by using it as an
  // EventTarget directly but that would be rather silly, so stepping outside the lines
  // of the spec here should be just fine.
  KJ_DEFER(removeAllHandlers());

  // Per spec, "signal abort" cannot throw: listener exceptions are reported, and the
  // remaining listeners (and, for a source signal, the dependents' abort steps) still run.
  dispatchEventImpl(js, js.alloc<Event>(kAbortEvent, Event::Init{}, Trusted::YES),
      effectiveExceptionPolicy(js, DispatchExceptionPolicy::REPORT));
}

void AbortSignal::serialize(jsg::Lock& js, jsg::Serializer& serializer) {
  JSG_REQUIRE(FeatureFlags::get(js).getAbortSignalRpc(), DOMDataCloneError,
      "AbortSignal serialization is not enabled.");

  auto& handler = JSG_REQUIRE_NONNULL(serializer.getExternalHandler(), DOMDataCloneError,
      "AbortSignal can only be serialized for RPC.");

  auto externalHandler = dynamic_cast<RpcSerializerExternalHandler*>(&handler);
  JSG_REQUIRE(
      externalHandler != nullptr, DOMDataCloneError, "AbortSignal can only be serialized for RPC.");

  serializer.writeRawUint32(static_cast<uint>(getAborted(js)));
  serializer.writeRawUint32(static_cast<uint>(flag));
  // getReason() falls back to a pending (RPC-received but not yet triggered) reason, so a
  // deserialized signal re-serialized before its receiving request processed the abort still
  // carries the reason along; it returns undefined when there is none.
  serializer.write(js, getReason(js));

  if (getAborted(js) || getNeverAborts()) {
    // This AbortSignal cannot be triggered in the future. No stream is needed.
    return;
  }

  auto triggerCap = [&]() -> rpc::AbortTrigger::Client {
    auto pipeline = externalHandler->getExternalPusher()
                        .pushAbortSignalRequest(capnp::MessageSize{2, 0})
                        .sendForPipeline();

    externalHandler->write(
        [signal = pipeline.getSignal()](rpc::JsValue::External::Builder builder) mutable {
      builder.setAbortSignal(kj::mv(signal));
    });

    return pipeline.getTrigger();
  }();

  auto& ioContext = IoContext::current();

  sweepCells(rpcRegistrations, [](auto& cell) -> auto& { return cell.client; });

  // Keep track of every AbortSignal cloned from this one.
  // If this->triggerAbort(...) is called, each clone will be informed.
  rpcRegistrations.add(kj::arc<RpcRegistration>(ioContext.getCrossContextExecutor(),
      ioContext.createObject<AbortTriggerRpcClient>(kj::mv(triggerCap))));
}

jsg::Ref<AbortSignal> AbortSignal::deserialize(
    jsg::Lock& js, rpc::SerializationTag tag, jsg::Deserializer& deserializer) {
  auto& handler = KJ_REQUIRE_NONNULL(
      deserializer.getExternalHandler(), "got AbortSignal on non-RPC serialized object?");
  auto externalHandler = dynamic_cast<RpcDeserializerExternalHandler*>(&handler);
  KJ_REQUIRE(externalHandler != nullptr, "got AbortSignal on non-RPC serialized object?");

  auto isCanceled = static_cast<bool>(deserializer.readRawUint32());
  auto flag = static_cast<Flag>(deserializer.readRawUint32());
  auto reason = deserializer.readValue(js);

  if (isCanceled) {
    // The signal is already aborted and cannot be triggered again. We don't need to set up RPC.
    return abort(js, reason);
  }

  if (flag == Flag::NEVER_ABORTS) {
    // The signal can't be aborted. We don't need to setup RPC
    return js.alloc<AbortSignal>(/* exception */ kj::none, /* maybeReason */ kj::none, flag);
  }

  // The AbortSignalImpl will receive any remote triggerAbort requests and fulfill the promise with the reason for abort

  auto signal = js.alloc<AbortSignal>(/* exception */ kj::none, /* maybeReason */ kj::none, flag);

  auto& ioctx = IoContext::current();

  auto reader = externalHandler->read();
  KJ_REQUIRE(reader.isAbortSignal(), "external table slot type does't match serialization tag");

  auto resolvedSignal = ioctx.getExternalPusher()->unwrapAbortSignal(reader.getAbortSignal());

  signal->rpcReceiverContext = ioctx.getCrossContextExecutor();
  signal->rpcAbortPromise = ioctx.addObject(kj::heap(kj::mv(resolvedSignal.signal)));
  signal->pendingReason = kj::mv(resolvedSignal.reason);

  return signal;
}

int AbortSignal::getNativeRegistrationCountForTest() {
  return static_cast<int>(nativeRegistrations.size() + rpcRegistrations.size());
}

void AbortSignal::skipReleaseForTest() {
  for (auto& reg: rpcRegistrations) {
    KJ_IF_SOME(client, take(reg->client)) {
      client->skipReleaseForTest = true;
    }
  }

  rpcRegistrations.clear();
}

bool AbortSignal::isRpcReceiverContextCurrent() {
  KJ_IF_SOME(executor, rpcReceiverContext) {
    return executor.isCurrent();
  }
  return false;
}

bool AbortSignal::hasPendingReason() {
  KJ_IF_SOME(pr, pendingReason) {
    return *pr->value.lockShared() != nullptr;
  }

  return false;
}

kj::Maybe<jsg::JsValue> AbortSignal::deserializePendingReason(jsg::Lock& js) {
  KJ_IF_SOME(pr, pendingReason) {
    // Copy the pending state out under the mutex; the JS work below then runs without
    // holding it. (The box is written at most once, by the receiving request; it may be read
    // from any context.)
    auto pending = [&]() -> kj::Maybe<ExternalPusherImpl::PendingAbortReason> {
      auto lock = pr->value.lockShared();
      if (*lock == nullptr) {
        // pendingReason not initialized. This means abort wasn't yet triggered.
        return kj::none;
      }
      KJ_SWITCH_ONEOF(*lock) {
        KJ_CASE_ONEOF(v8Serialized, kj::Array<kj::byte>) {
          return ExternalPusherImpl::PendingAbortReason(kj::heapArray<kj::byte>(v8Serialized));
        }
        KJ_CASE_ONEOF(exception, kj::Exception) {
          return ExternalPusherImpl::PendingAbortReason(exception.clone());
        }
      }
      KJ_UNREACHABLE;
    }();

    KJ_IF_SOME(p, pending) {
      KJ_SWITCH_ONEOF(p) {
        KJ_CASE_ONEOF(v8Serialized, kj::Array<kj::byte>) {
          jsg::Deserializer des(js, v8Serialized);
          return kj::some(des.readValue(js));
        }
        KJ_CASE_ONEOF(exception, kj::Exception) {
          return kj::some(js.exceptionToJsValue(kj::mv(exception)).getHandle(js));
        }
      }
      KJ_UNREACHABLE;
    }
  }

  return kj::none;
}

void AbortSignal::subscribeToRpcAbort(jsg::Lock& js) {
  // For an AbortSignal received over RPC, the first time someone registers an event on the signal,
  // we want to arrange to awaitIo() for the underlying RPC signal. If no one is actually listening,
  // though, we don't want to awaitIo() since it blocks hibernation in actors.

  if (rpcAbortPromise == kj::none) {
    // Not an RPC-received signal, or the subscription is already armed.
    return;
  }

  if (!isRpcReceiverContextCurrent()) {
    // Only the request that deserialized this signal can arm the subscription — it owns the
    // underlying promise — but a signal retained across requests may see registrations from
    // other contexts. Ask the receiving context to arm on its next turn. The queued action
    // captures only a WeakRef, which is safe to destroy from any thread should that context
    // be torn down without draining its queue. If the context is already gone, the request
    // is dropped: no delivery is possible anymore, though the abort itself stays observable
    // through the pending-reason box (see docs/reference/detail/abort-signal.md).
    KJ_IF_SOME(executor, rpcReceiverContext) {
      executor.tryExecute([weakSelf = JSG_THIS_WEAK(js)](jsg::Lock& js) {
        KJ_IF_SOME(self, weakSelf.tryGet()) {
          self.subscribeToRpcAbort(js);
        }
      });
    }
    return;
  }

  KJ_IF_SOME(promise, rpcAbortPromise) {
    IoContext::current().awaitIo(js, kj::mv(*promise), [self = JSG_THIS](jsg::Lock& js) mutable {
      KJ_IF_SOME(r, self->deserializePendingReason(js)) {
        self->triggerAbort(js, r);
      }
    });

    rpcAbortPromise = kj::none;
  }
}

bool AbortSignal::isIgnoredForSubrequests(jsg::Lock& js) const {
  // True if this is a signal on the request of an incoming fetch. When the compat flag
  // `requestSignalPassthrough` is set, this flag has no effect. But to ensure backwards
  // compatibility, when this flag is not set, this signal will not be passed through to
  // subrequests derived from the incoming request.

  return !FeatureFlags::get(js).getRequestSignalPassthrough() &&
      flag == Flag::IGNORE_FOR_SUBREQUESTS;
}

void AbortController::abort(jsg::Lock& js, jsg::Optional<jsg::JsValue> maybeReason) {
  signal->triggerAbort(js, maybeReason);
}

void EventTarget::visitForGc(jsg::GcVisitor& visitor) {
  for (auto& entry: typeMap) {
    for (auto& handler: entry.value.handlers) {
      visitor.visit(*handler);
    }
  }
  for (auto& entry: eventHandlerAttributes) {
    KJ_IF_SOME(handler, entry.value.handler) {
      visitor.visit(handler.value);
      KJ_IF_SOME(fn, handler.fn) {
        visitor.visit(fn);
      }
    }
    KJ_IF_SOME(identity, entry.value.listenerIdentity) {
      visitor.visit(identity);
    }
  }
}

kj::Promise<void> Scheduler::wait(
    jsg::Lock& js, double delay, jsg::Optional<WaitOptions> maybeOptions) {
  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(s, options.signal) {
      if (s->getAborted(js)) {
        return js.exceptionToKj(s->getReason(js));
      }
    }
  }

  // TODO(cleanup): Use jsg promise and resolver to avoid an unlock/relock. However, we need
  //   the abort signal to support wrapping jsg promises.
  auto paf = kj::newPromiseAndFulfiller<void>();

  auto context = js.v8Context();

  auto& global =
      jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(context, context->Global());
  auto timeoutId = global.setTimeoutInternal(
      [fulfiller = IoContext::current().addObject(kj::mv(paf.fulfiller))](jsg::Lock& lock) mutable {
    fulfiller->fulfill();
  }, delay);

  auto promise = kj::mv(paf.promise);

  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(s, options.signal) {
      promise = s->wrap(js, kj::mv(promise));
      // When the signal aborts and the canceler drops the promise, clear the underlying
      // timeout to free the quota slot. clearTimeoutImpl is a no-op if the timeout has
      // already fired, so this is safe on the normal completion path as well.
      promise = promise.attach(kj::defer([timeoutId, &ioContext = IoContext::current()]() {
        ioContext.clearTimeoutImpl(TimeoutId::fromNumber(timeoutId));
      }));
    }
  }

  return kj::mv(promise);
}

void ExtendableEvent::waitUntil(kj::Promise<void> promise) {
  JSG_REQUIRE(
      getIsTrusted(), DOMInvalidStateError, "waitUntil() can only be called on trusted event.");
  IoContext::current().addWaitUntil(kj::mv(promise));
}

jsg::Optional<jsg::Ref<ActorState>> ExtendableEvent::getActorState(jsg::Lock& js) {
  IoContext& context = IoContext::current();
  return context.getActor().map([&](Worker::Actor& actor) {
    auto& lock = context.getCurrentLock();
    auto persistent = actor.makeStorageForSwSyntax(lock);
    return js.alloc<api::ActorState>(actor.cloneId(), actor.getTransient(lock), kj::mv(persistent));
  });
}

CustomEvent::CustomEvent(kj::String ownType, CustomEventInit init)
    : Event(kj::mv(ownType), Event::Init(init)),
      detail(kj::mv(init.detail)) {}

jsg::Ref<CustomEvent> CustomEvent::constructor(
    jsg::Lock& js, kj::String type, jsg::Optional<CustomEventInit> init) {
  return js.alloc<CustomEvent>(kj::mv(type), kj::mv(init).orDefault({}));
}

jsg::Optional<jsg::JsValue> CustomEvent::getDetail(jsg::Lock& js) {
  return detail.map([&](jsg::JsRef<jsg::JsValue>& val) { return val.getHandle(js); });
}

CustomEvent::CustomEventInit::operator Event::Init() {
  return {
    .bubbles = this->bubbles.map([](auto& val) { return val; }),
    .cancelable = this->cancelable.map([](auto& val) { return val; }),
    .composed = this->composed.map([](auto& val) { return val; }),
  };
}

size_t EventTarget::EventHandler::jsgGetMemorySelfSize() const {
  return sizeof(EventHandler);
}

void EventTarget::EventHandler::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("identity", identity);
  tracker.trackField("callback", callback);
  if (abortHandler != kj::none) {
    tracker.trackFieldWithSize("abortHandler", sizeof(kj::Own<void>));
  }
}

size_t EventTarget::EventHandlerSet::jsgGetMemorySelfSize() const {
  return sizeof(EventHandlerSet);
}

void EventTarget::EventHandlerSet::jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const {
  for (const auto& handler: handlers) {
    tracker.trackField("handler", handler);
  }
}

void EventTarget::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("typeMap", typeMap);
  tracker.trackFieldWithSize("eventHandlerAttributes",
      eventHandlerAttributes.size() * sizeof(decltype(eventHandlerAttributes)::Entry));
}

}  // namespace workerd::api
