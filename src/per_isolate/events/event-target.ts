'use strict';

// EventTarget (DOM §2.7-§2.10).
//
// Listeners are kept per type, in lists created by addEventListener() and
// deleted once they empty. A target has no parent, so the dispatch path is
// the target alone and both of dispatch's passes run at it: the capturing
// pass runs its capture listeners, then the bubbling pass runs the rest.
//
// Listener exceptions are reported through the global reportError(), which
// fires the global 'error' event and logs the exception unless a listener
// cancels it. Nothing reaches the dispatchEvent() caller.

import type { CommonHelpers } from './common';
import type { Event, EventInternals } from './event';

const {
  ArrayPrototypePush,
  FunctionPrototypeBind,
  FunctionPrototypeCall,
  ObjectDefineProperties,
  SafeMap,
  SymbolToStringTag,
  TypeError,
  WeakRef,
  WeakRefPrototypeDeref,
} = primordials;

const {
  isObject,
  kEnumerable,
  notEnoughArguments,
  requireAbortSignal,
  toStringTagProperty,
} = require('events/common') as CommonHelpers;

const {
  isEvent,
  type: getEventType,
  beginDispatch,
  enterInvoke,
  setInPassiveListener,
  isImmediatePropagationStopped,
  endDispatch,
} = (require('events/event') as { eventInternals: EventInternals })
  .eventInternals;

// Captured before user code can replace it.
const reportError = FunctionPrototypeBind(
  globalThis.reportError,
  globalThis
) as (error: unknown) => void;

// The spec's event listener struct, less its type, which keys the list that
// holds it. Its identity is (callback, capture); duplicates are never added.
interface ListenerRecord {
  readonly callback: object;
  readonly capture: boolean;
  readonly passive: boolean;
  readonly once: boolean;
  removed: boolean;
  // The target whose list holds the record, for the {signal} option's abort
  // algorithm, which holds the record only weakly.
  readonly target: EventTarget;
  // The abort algorithm's registration with the {signal} option's signal.
  registration: AbortAlgorithmHandle | undefined;
}

interface ListenerOptionsDict {
  capture?: unknown;
  once?: unknown;
  passive?: unknown;
  signal?: unknown;
}

// WebIDL conversion of an EventListener? argument: undefined and null mean
// no listener, and any other object, callable or not, is the listener.
function convertListener(callback: unknown, context: string): object | null {
  if (callback == null) return null;
  if (!isObject(callback)) {
    throw new TypeError(`${context}: parameter 2 is not of type 'Object'.`);
  }
  return callback;
}

// Both scans run over a live list, which never holds a removed record.
function findListener(
  list: ListenerRecord[],
  callback: object,
  capture: boolean
): ListenerRecord | undefined {
  for (let i = 0; i < list.length; i++) {
    const record = list[i] as ListenerRecord;
    if (record.callback === callback && record.capture === capture) {
      return record;
    }
  }
  return undefined;
}

function indexOfRecord(list: ListenerRecord[], record: ListenerRecord): number {
  for (let i = 0; i < list.length; i++) {
    if (list[i] === record) return i;
  }
  return -1;
}

// The records of `list` that run in one dispatch pass, in order: the capture
// listeners for the capturing pass, the rest for the bubbling pass. This is
// the spec's clone of the list, less the records the pass would skip anyway.
// undefined if there are none.
function listenersForPass(
  list: ListenerRecord[],
  capture: boolean
): ListenerRecord[] | undefined {
  let snapshot: ListenerRecord[] | undefined;
  for (let i = 0; i < list.length; i++) {
    const record = list[i] as ListenerRecord;
    if (record.capture !== capture) continue;
    if (snapshot === undefined) snapshot = [record];
    else ArrayPrototypePush(snapshot, record);
  }
  return snapshot;
}

// WebIDL's "call a user object's operation" for an EventListener: a callable
// listener is called with the target as `this`; any other listener's
// handleEvent is read at every call, and called with the listener as `this`.
// Return values are ignored.
function callListener(
  callback: object,
  target: EventTarget,
  event: Event
): void {
  if (typeof callback === 'function') {
    FunctionPrototypeCall(callback, target, event);
    return;
  }
  const handleEvent: unknown = (callback as { handleEvent?: unknown })
    .handleEvent;
  if (typeof handleEvent !== 'function') {
    throw new TypeError("The listener's handleEvent is not a function.");
  }
  FunctionPrototypeCall(handleEvent, callback, event);
}

// Inner invoke's "report the exception". The global reportError() throws
// only when an 'error' listener throws under the legacy dispatch policy
// (spec_compliant_dispatch_exceptions off), and that exception is dropped.
function reportException(exception: unknown): void {
  try {
    reportError(exception);
  } catch {
    // Dropped (see above).
  }
}

let assertIsEventTarget: (self: unknown) => void;
// The spec's "remove an event listener". Idempotent.
let removeListener: (
  target: EventTarget,
  type: string,
  record: ListenerRecord
) => void;

// The {signal} option's abort steps: remove the listener. They hold the
// record only weakly, and the record holds its target, so a signal that
// outlives a target does not keep the target or its listeners alive. A
// collected record takes its registration with it, which unregisters the
// algorithm.
function newAbortAlgorithm(type: string, record: ListenerRecord): () => void {
  const recordRef = new WeakRef(record);
  return () => {
    const live = WeakRefPrototypeDeref(recordRef) as ListenerRecord | undefined;
    if (live !== undefined) removeListener(live.target, type, live);
  };
}

class EventTarget {
  // A SafeMap from type to listener list, created by the first
  // addEventListener().
  #listeners: Map<string, ListenerRecord[]> | undefined = undefined;

  static {
    assertIsEventTarget = (self: unknown) => {
      if (!isObject(self) || !(#listeners in self)) {
        throw new TypeError('Illegal invocation');
      }
    };

    removeListener = (target, type, record) => {
      if (record.removed) return;
      record.removed = true;
      const listeners = target.#listeners;
      const list = listeners?.get(type);
      if (list !== undefined) {
        const index = indexOfRecord(list, record);
        if (index !== -1) {
          const last = list.length - 1;
          for (let i = index; i < last; i++) list[i] = list[i + 1]!;
          list.length = last;
          if (last === 0) listeners!.delete(type);
        }
      }
      const registration = record.registration;
      if (registration !== undefined) {
        record.registration = undefined;
        registration.remove();
      }
    };
  }

  addEventListener(
    type: string,
    callback: EventListenerOrEventListenerObject | null,
    options: boolean | AddEventListenerOptions | undefined = undefined
  ): void {
    assertIsEventTarget(this);
    if (arguments.length < 2) {
      throw notEnoughArguments(
        "Failed to execute 'addEventListener' on 'EventTarget'",
        2,
        arguments.length
      );
    }
    // WebIDL converts every argument, the options' members included, before
    // anything else happens.
    const typeString = `${type}`;
    const listener = convertListener(
      callback,
      "Failed to execute 'addEventListener' on 'EventTarget'"
    );
    let capture: boolean;
    let once = false;
    let passive = false;
    let signal: unknown;
    let aborted = false;
    if (isObject(options)) {
      const dict = options as ListenerOptionsDict;
      capture = !!dict.capture;
      once = !!dict.once;
      passive = !!dict.passive;
      signal = dict.signal;
      if (signal !== undefined) {
        aborted = requireAbortSignal(
          signal,
          "Failed to execute 'addEventListener' on 'EventTarget': " +
            "Failed to read the 'signal' property from " +
            "'AddEventListenerOptions': The provided value is not of type " +
            "'AbortSignal'."
        );
      }
    } else {
      capture = !!options;
    }

    // The spec's "add an event listener".
    if (aborted || listener === null) return;
    let listeners = this.#listeners;
    const list = listeners?.get(typeString);
    if (list !== undefined && findListener(list, listener, capture)) {
      return;
    }
    const record = {
      __proto__: null,
      callback: listener,
      capture,
      passive,
      once,
      removed: false,
      target: this,
      registration: undefined,
    } as ListenerRecord;
    if (signal !== undefined) {
      record.registration = utils.addAbortAlgorithm(
        signal as AbortSignal,
        newAbortAlgorithm(typeString, record)
      );
    }
    if (list !== undefined) {
      ArrayPrototypePush(list, record);
      return;
    }
    if (listeners === undefined) {
      listeners = new SafeMap() as Map<string, ListenerRecord[]>;
      this.#listeners = listeners;
    }
    listeners.set(typeString, [record]);
  }

  removeEventListener(
    type: string,
    callback: EventListenerOrEventListenerObject | null,
    options: boolean | EventListenerOptions | undefined = undefined
  ): void {
    assertIsEventTarget(this);
    if (arguments.length < 2) {
      throw notEnoughArguments(
        "Failed to execute 'removeEventListener' on 'EventTarget'",
        2,
        arguments.length
      );
    }
    const typeString = `${type}`;
    const listener = convertListener(
      callback,
      "Failed to execute 'removeEventListener' on 'EventTarget'"
    );
    const capture = isObject(options)
      ? !!(options as ListenerOptionsDict).capture
      : !!options;
    if (listener === null) return;
    const list = this.#listeners?.get(typeString);
    if (list === undefined) return;
    const record = findListener(list, listener, capture);
    if (record !== undefined) removeListener(this, typeString, record);
  }

  dispatchEvent(event: Event): boolean {
    assertIsEventTarget(this);
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to execute 'dispatchEvent' on 'EventTarget'",
        1,
        arguments.length
      );
    }
    if (!isEvent(event)) {
      throw new TypeError(
        "Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'."
      );
    }
    beginDispatch(event, this);
    // The type cannot change during dispatch: initEvent() does nothing then.
    const type = getEventType(event);
    let notCanceled: boolean;
    try {
      this.#invoke(event, type, true);
      this.#invoke(event, type, false);
    } finally {
      notCanceled = endDispatch(event);
    }
    return notCanceled;
  }

  // The spec's invoke and inner invoke at this target, for one pass. The pass
  // runs over a snapshot of the list taken as it starts, so a listener added
  // during the pass does not run in it, and a listener removed during the
  // pass is skipped because removal marks it.
  #invoke(event: Event, type: string, capture: boolean): void {
    const list = this.#listeners?.get(type);
    if (list === undefined || !enterInvoke(event, this)) return;
    const snapshot = listenersForPass(list, capture);
    if (snapshot === undefined) return;
    for (let i = 0; i < snapshot.length; i++) {
      const record = snapshot[i] as ListenerRecord;
      if (record.removed) continue;
      if (record.once) removeListener(this, type, record);
      if (record.passive) setInPassiveListener(event, true);
      try {
        callListener(record.callback, this, event);
      } catch (exception) {
        reportException(exception);
      }
      if (record.passive) setInPassiveListener(event, false);
      if (isImmediatePropagationStopped(event)) break;
    }
  }
}

ObjectDefineProperties(EventTarget.prototype, {
  __proto__: null,
  addEventListener: kEnumerable,
  removeEventListener: kEnumerable,
  dispatchEvent: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('EventTarget'),
});

module.exports = {
  EventTarget,
};
