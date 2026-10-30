'use strict';

// The seam between EventTarget subclasses implemented in the bootstrap and the
// EventTarget they extend. A subclass uses nothing of EventTarget's beyond its
// public JavaScript surface except through this module: not utils, and not the
// EventTarget primordial.
//
// EventTarget is still implemented in C++ (api/basics.h), and the functions
// here reach the parts of it that C++ subclasses use but JavaScript cannot see,
// through api/event-target-bootstrap.h. When EventTarget moves to the
// bootstrap, this module is reimplemented on top of it and the subclasses do
// not change. The contract below is what that implementation must provide.
//
// THE CONTRACT
//
// EventTarget
//   The class to extend.
//
// setListenerCountHook(target, type, hook)
//   Calls hook(type, count) whenever the number of listeners registered on
//   `target` for `type` changes. Every registration counts: addEventListener()
//   and removeEventListener(), a once-listener's removal as it is dispatched,
//   a removal by the listener's AbortSignal, and the activation and
//   deactivation of the on<type> event handler attribute, whose trampoline
//   occupies a listener slot of its own. Wholesale teardown does not notify.
//   One hook per target and type; registering another replaces it. The hook is
//   called synchronously from inside those operations, possibly mid-dispatch,
//   so it must neither run user code nor throw.
//
// getEventHandlerAttribute(target, type)
// setEventHandlerAttribute(target, type, value)
//   HTML's on<type> event handler IDL attribute. Assigning any object
//   activates a trampoline listener at the end of the listener list, which
//   keeps its position across reassignment; assigning anything else
//   deactivates it, so the next assignment takes a fresh position. Only a
//   callable value is ever invoked, with the global object as its receiver;
//   returning true cancels the event. The getter returns the exact value
//   assigned, or null. Once set, on<type> is no longer read as a plain
//   property during dispatch, which would fire the handler twice.
//
// effectiveExceptionPolicy(desired)
//   What a dispatch that wants `desired` must use: 'report' falls back to
//   'propagate' unless spec_compliant_dispatch_exceptions is enabled.
//
// dispatchEvent(target, event, policy)
//   Dispatches `event` at `target` with the policy given, not the effective
//   one. Under 'report' every listener runs, each listener exception is
//   reported as the spec's "inner invoke" requires, and the result is
//   undefined if none threw, else a null-prototype record whose `exception`
//   is the first. Under 'propagate' the first listener exception ends the
//   dispatch and is thrown. Unlike the public dispatchEvent(), the event may
//   be a trusted one.
//
// createTrustedEvent(type)
//   An Event as the runtime fires one: isTrusted, neither bubbling nor
//   cancelable.
//
// createTrustedMessageEvent(type, data, source)
//   A MessageEvent as the runtime fires one: isTrusted, `data` as is (not
//   cloned), an empty lastEventId, a null origin, no ports, and `source`, a
//   MessagePort or null.

const { EventTarget } = primordials;

const {
  setListenerCountHook,
  getEventHandlerAttribute,
  setEventHandlerAttribute,
  dispatchEvent: utilsDispatchEvent,
  createTrustedEvent,
  createTrustedMessageEvent,
} = utils;

type ExceptionPolicy = 'report' | 'propagate';

const kSpecCompliantDispatchExceptions =
  compatFlags['spec_compliant_dispatch_exceptions'] === true;

function effectiveExceptionPolicy(desired: ExceptionPolicy): ExceptionPolicy {
  return desired === 'report' && !kSpecCompliantDispatchExceptions
    ? 'propagate'
    : desired;
}

function dispatchEvent(
  target: EventTarget,
  event: Event,
  policy: ExceptionPolicy
): { readonly exception: unknown } | undefined {
  return utilsDispatchEvent(target, event, policy === 'report');
}

export type { ExceptionPolicy };

export type EventTargetInternals = {
  EventTarget: typeof globalThis.EventTarget;
  setListenerCountHook: typeof setListenerCountHook;
  getEventHandlerAttribute: typeof getEventHandlerAttribute;
  setEventHandlerAttribute: typeof setEventHandlerAttribute;
  effectiveExceptionPolicy: typeof effectiveExceptionPolicy;
  dispatchEvent: typeof dispatchEvent;
  createTrustedEvent: typeof createTrustedEvent;
  createTrustedMessageEvent: typeof createTrustedMessageEvent;
};

module.exports = {
  EventTarget,
  setListenerCountHook,
  getEventHandlerAttribute,
  setEventHandlerAttribute,
  effectiveExceptionPolicy,
  dispatchEvent,
  createTrustedEvent,
  createTrustedMessageEvent,
};
