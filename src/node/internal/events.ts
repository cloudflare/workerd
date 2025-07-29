// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Adapted from Deno and Node.js:
// Copyright 2018-2022 the Deno authors. All rights reserved. MIT license.
//
// Adapted from Node.js. Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
/* eslint-disable */

import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
  ERR_INVALID_THIS,
  ERR_OUT_OF_RANGE,
  ERR_UNHANDLED_ERROR,
} from 'node-internal:internal_errors';
import type {
  EventEmitterAsyncResource as _EventEmitterAsyncResource,
  EventEmitter as _EventEmitter,
} from 'node:events';
import {
  validateAbortSignal,
  validateBoolean,
  validateFunction,
  validateObject,
} from 'node-internal:validators';
import { spliceOne } from 'node-internal:internal_utils';
import { nextTick, emitWarning } from 'node-internal:internal_process';
import { default as async_hooks } from 'node-internal:async_hooks';
const { AsyncResource } = async_hooks;

import { inspect } from 'node-internal:internal_inspect';

const enableNodejsProcessV2 =
  !!Cloudflare.compatibilityFlags['enable_nodejs_process_v2'];

const kRejection = Symbol.for('nodejs.rejection');
const kCapture = Symbol('kCapture');
const kErrorMonitor = Symbol('events.errorMonitor');
const kMaxEventTargetListeners = Symbol('events.maxEventTargetListeners');
const kMaxEventTargetListenersWarned = Symbol(
  'events.maxEventTargetListenersWarned'
);

export interface EventEmitterOptions {
  captureRejections?: boolean;
}

export type EventName = string | symbol | number;
export type EventCallback = ((...args: any[]) => unknown) & {
  listener?: EventCallback;
};
// @ts-expect-error TS2417 Unrelated error.
export declare class EventEmitter extends _EventEmitter {
  constructor(opts?: EventEmitterOptions);
  listenerCount(eventName: EventName): number;
  emit(eventName: EventName, ...args: any[]): boolean;
  on(eventName: EventName, listener: EventCallback): this;
  once(eventName: EventName, listener: EventCallback): this;
  addEventListener(eventName: EventName, listener: EventCallback): this;
  removeEventListener(eventName: EventName, listener: EventCallback): this;

  [kRejection](err: unknown, eventName: EventName, ...args: unknown[]): void;
  [kCapture]: boolean;

  _events: undefined | Record<EventName, EventCallback[]>;
  _eventsCount: number;
  _maxListeners: undefined | number;
}

type AsyncResource = typeof AsyncResource;

declare var EventTarget: Function;

export function EventEmitter(
  this: EventEmitter,
  opts?: EventEmitterOptions
): EventEmitter {
  EventEmitter.init.call(this, opts);
  return this;
}

class EventEmitterReferencingAsyncResource extends AsyncResource {
  #eventEmitter: EventEmitter;
  constructor(emitter: EventEmitter) {
    super('');
    this.#eventEmitter = emitter;
  }

  get eventEmitter() {
    if (this.#eventEmitter === undefined)
      throw new ERR_INVALID_THIS('EventEmitterReferencingAsyncResource');
    return this.#eventEmitter;
  }
}

export class EventEmitterAsyncResource
  extends EventEmitter
  implements _EventEmitterAsyncResource
{
  #asyncResource: EventEmitterReferencingAsyncResource;

  constructor(options?: EventEmitterOptions) {
    super(options);
    this.#asyncResource = new EventEmitterReferencingAsyncResource(this);
  }

  // @ts-expect-error TS2416 Not assignable to base type
  get asyncResource(): AsyncResource {
    if (this.#asyncResource === undefined)
      throw new ERR_INVALID_THIS('EventEmitterAsyncResource');
    // @ts-expect-error TS2741 Prototype is missing from type.
    return this.#asyncResource;
  }

  override emit(event: string | symbol, ...args: any[]): boolean {
    if (this.#asyncResource === undefined)
      throw new ERR_INVALID_THIS('EventEmitterAsyncResource');
    args.unshift(super.emit, this, event);
    Reflect.apply(
      this.#asyncResource.runInAsyncScope,
      this.#asyncResource,
      args
    );
    return true;
  }
}

export function addAbortListener(signal: AbortSignal, listener: any) {
  if (signal === undefined) {
    throw new ERR_INVALID_ARG_TYPE('signal', 'AbortSignal', signal);
  }
  validateAbortSignal(signal, 'signal');
  validateFunction(listener, 'listener');

  let removeEventListener: Function;
  if (signal.aborted) {
    queueMicrotask(() => listener());
  } else {
    signal.addEventListener('abort', listener, { once: true });
    removeEventListener = () => {
      signal.removeEventListener('abort', listener);
    };
  }
  return {
    __proto__: null,
    [Symbol.dispose]() {
      removeEventListener?.();
    },
  };
}

export default EventEmitter;

EventEmitter.on = on;
EventEmitter.once = once;
EventEmitter.getEventListeners = getEventListeners;
EventEmitter.getMaxListeners = _getMaxListeners;
EventEmitter.setMaxListeners = setMaxListeners;
EventEmitter.listenerCount = listenerCount;
EventEmitter.EventEmitter = EventEmitter;
EventEmitter.usingDomains = false;
EventEmitter.captureRejectionSymbol = kRejection;
EventEmitter.errorMonitor = kErrorMonitor;
EventEmitter.EventEmitterAsyncResource = EventEmitterAsyncResource;

export const captureRejectionSymbol = EventEmitter.captureRejectionSymbol;
export const errorMonitor = EventEmitter.errorMonitor;
export const getMaxListeners = _getMaxListeners;
export const usingDomains = EventEmitter.usingDomains;
export let defaultMaxListeners = 10;

Object.defineProperties(EventEmitter, {
  captureRejections: {
    get(this: EventEmitter) {
      return EventEmitter.prototype[kCapture];
    },
    set(this: EventEmitter, value: unknown): void {
      validateBoolean(value, 'EventEmitter.captureRejections');

      EventEmitter.prototype[kCapture] = value;
    },
    enumerable: true,
  },
  defaultMaxListeners: {
    enumerable: true,
    get: function (this: EventEmitter): number {
      return defaultMaxListeners;
    },
    set: function (this: EventEmitter, arg: unknown): void {
      if (typeof arg !== 'number' || arg < 0 || Number.isNaN(arg)) {
        throw new ERR_OUT_OF_RANGE(
          'defaultMaxListeners',
          'a non-negative number',
          arg
        );
      }
      defaultMaxListeners = arg;
    },
  },
  kMaxEventTargetListeners: {
    value: kMaxEventTargetListeners,
    enumerable: false,
    configurable: false,
    writable: false,
  },
  kMaxEventTargetListenersWarned: {
    value: kMaxEventTargetListenersWarned,
    enumerable: false,
    configurable: false,
    writable: false,
  },
});

// The default for captureRejections is false
Object.defineProperty(EventEmitter.prototype, kCapture, {
  value: false,
  writable: true,
  enumerable: false,
});

EventEmitter.init = function (this: EventEmitter, opts?: EventEmitterOptions) {
  if (
    this._events === undefined ||
    this._events === Object.getPrototypeOf(this)._events
  ) {
    this._events = Object.create(null);
    this._eventsCount = 0;
  }

  (this as any)._maxListeners ??= undefined;

  if (opts?.captureRejections) {
    validateBoolean(opts.captureRejections, 'options.captureRejections');
    (this as any)[kCapture] = Boolean(opts.captureRejections);
  } else {
    // Assigning the kCapture property directly saves an expensive
    // prototype lookup in a very sensitive hot path.
    (this as any)[kCapture] = EventEmitter.prototype[kCapture];
  }
};

export function setMaxListeners(
  n = defaultMaxListeners,
  ...eventTargets: any[]
) {
  if (typeof n !== 'number' || n < 0 || Number.isNaN(n)) {
    throw new ERR_OUT_OF_RANGE('n', 'a non-negative number', n);
  }
  if (eventTargets.length === 0) {
    defaultMaxListeners = n;
  } else {
    for (let i = 0; i < eventTargets.length; i++) {
      const target = eventTargets[i];
      if (target instanceof EventTarget) {
        (target as any)[kMaxEventTargetListeners] = n;
        (target as any)[kMaxEventTargetListenersWarned] = false;
      } else if (typeof target.setMaxListeners === 'function') {
        target.setMaxListeners(n);
      } else {
        throw new ERR_INVALID_ARG_TYPE(
          'eventTargets',
          ['EventEmitter', 'EventTarget'],
          target
        );
      }
    }
  }
}

EventEmitter.prototype._events = undefined;
EventEmitter.prototype._eventsCount = 0;
EventEmitter.prototype._maxListeners = undefined;
EventEmitter.addAbortListener = addAbortListener;

function addCatch(
  that: any,
  promise: Promise<unknown>,
  type: string | symbol,
  args: any[]
) {
  if (!that[kCapture]) {
    return;
  }

  // Handle Promises/A+ spec, then could be a getter
  // that throws on second use.
  try {
    const then = promise.then;

    if (typeof then === 'function') {
      then.call(promise, undefined, function (err) {
        // The callback is called with nextTick to avoid a follow-up
        // rejection from this promise.
        nextTick(emitUnhandledRejectionOrErr, that, err, type, args);
      });
    }
  } catch (err) {
    that.emit('error', err);
  }
}

function emitUnhandledRejectionOrErr(
  ee: any,
  err: any,
  type: string | symbol,
  args: any[]
) {
  if (typeof ee[kRejection] === 'function') {
    ee[kRejection](err, type, ...args);
  } else {
    // We have to disable the capture rejections mechanism, otherwise
    // we might end up in an infinite loop.
    const prev = ee[kCapture];

    // If the error handler throws, it is not catchable and it
    // will end up in 'uncaughtException'. We restore the previous
    // value of kCapture in case the uncaughtException is present
    // and the exception is handled.
    try {
      ee[kCapture] = false;
      ee.emit('error', err);
    } finally {
      ee[kCapture] = prev;
    }
  }
}

EventEmitter.prototype.setMaxListeners = function setMaxListeners(n: number) {
  if (typeof n !== 'number' || n < 0 || Number.isNaN(n)) {
    throw new ERR_OUT_OF_RANGE('n', 'a non-negative number', n);
  }
  this._maxListeners = n;
  return this;
};

function _getMaxListeners(that: any) {
  if (that._maxListeners === undefined) {
    return (EventEmitter as any).defaultMaxListeners;
  }
  return that._maxListeners;
}

EventEmitter.prototype.getMaxListeners = function getMaxListeners(
  this: EventEmitter
): number {
  return _getMaxListeners(this);
};

EventEmitter.prototype.emit = function emit(
  this: EventEmitter,
  type: string | symbol,
  ...args: any[]
): boolean {
  let doError = type === 'error';

  const events = this._events;
  if (events !== undefined) {
    if (doError && events[kErrorMonitor] !== undefined) {
      this.emit(kErrorMonitor, ...args);
    }
    doError = doError && events.error === undefined;
  } else if (!doError) {
    return false;
  }

  // If there is no 'error' event listener then throw.
  if (doError) {
    let er;
    if (args.length > 0) {
      er = args[0];
    }
    if (er instanceof Error) {
      try {
        const capture = {};
        (Error as any).captureStackTrace(capture, EventEmitter.prototype.emit);
      } catch {
        // pass
      }

      // Note: The comments on the `throw` lines are intentional, they show
      // up in Node's output if this results in an unhandled exception.
      throw er; // Unhandled 'error' event
    }

    let stringifiedEr;
    try {
      stringifiedEr = inspect(er);
    } catch {
      stringifiedEr = er;
    }

    // At least give some kind of context to the user
    const err = new ERR_UNHANDLED_ERROR(stringifiedEr);
    (err as any).context = er;
    throw err; // Unhandled 'error' event
  }

  const handler = events?.[type];

  if (handler === undefined) {
    return false;
  }

  if (typeof handler === 'function') {
    const result = (handler as Function).apply(this, args);

    // We check if result is undefined first because that
    // is the most common case so we do not pay any perf
    // penalty
    if (result !== undefined && result !== null) {
      addCatch(this, result, type, args);
    }
  } else {
    const len = handler.length;
    const listeners = arrayClone(handler);
    for (let i = 0; i < len; ++i) {
      const result = listeners[i].apply(this, args);

      // We check if result is undefined first because that
      // is the most common case so we do not pay any perf
      // penalty.
      // This code is duplicated because extracting it away
      // would make it non-inlineable.
      if (result !== undefined && result !== null) {
        addCatch(this, result, type, args);
      }
    }
  }

  return true;
};

function _addListener(
  target: any,
  type: string | symbol,
  listener: unknown,
  prepend: boolean
) {
  let m;
  let events;
  let existing;

  validateFunction(listener, 'listener');

  events = target._events;
  if (events === undefined) {
    events = target._events = Object.create(null);
    target._eventsCount = 0;
  } else {
    // To avoid recursion in the case that type === "newListener"! Before
    // adding it to the listeners, first emit "newListener".
    if (events.newListener !== undefined) {
      target.emit('newListener', type, (listener as any).listener ?? listener);

      // Re-assign `events` because a newListener handler could have caused the
      // this._events to be assigned to a new object
      events = target._events;
    }
    existing = events[type];
  }

  if (existing === undefined) {
    // Optimize the case of one listener. Don't need the extra array object.
    events[type] = listener;
    ++target._eventsCount;
  } else {
    if (typeof existing === 'function') {
      // Adding the second element, need to change to array.
      existing = events[type] = prepend
        ? [listener, existing]
        : [existing, listener];
      // If we've already got an array, just append.
    } else if (prepend) {
      existing.unshift(listener);
    } else {
      existing.push(listener);
    }

    // Check for listener leak
    m = _getMaxListeners(target);
    if (m > 0 && existing.length > m && !existing.warned) {
      existing.warned = true;
      console.log(
        'Possible EventEmitter memory leak detected. ' +
          `${existing.length} ${String(type)} listeners ` +
          `added to an EventEmitter. Use ` +
          'emitter.setMaxListeners() to increase limit'
      );
      const w: EventEmitterError = Object.assign(
        new Error(
          'Possible EventEmitter memory leak detected. ' +
            `${existing.length} ${String(type)} listeners ` +
            `added to ${inspect(target, { depth: -1 })}. Use ` +
            'emitter.setMaxListeners() to increase limit'
        ),
        {
          name: 'MaxListenersExceededWarning',
          emitter: target,
          type: type,
          count: existing.length,
        }
      );
      // Only the newer process version compat adds process.emitWarning support.
      if (enableNodejsProcessV2) emitWarning(w);
    }
  }

  return target;
}

interface EventEmitterError extends Error {
  name: string;
  emitter: unknown;
  type: string | symbol;
  count: number;
}

EventEmitter.prototype.addListener = function addListener(
  this: EventEmitter,
  type: string | symbol,
  listener: unknown
) {
  return _addListener(this, type, listener, false);
};

EventEmitter.prototype.on = EventEmitter.prototype.addListener;

EventEmitter.prototype.prependListener = function prependListener(
  type: string | symbol,
  listener: unknown
) {
  return _addListener(this, type, listener, true);
};

function onceWrapper(this: any) {
  if (!this.fired) {
    this.target.removeListener(this.type, this.wrapFn);
    this.fired = true;
    if (arguments.length === 0) {
      return this.listener.call(this.target);
    }
    return this.listener.apply(this.target, arguments);
  }
}

function _onceWrap(target: any, type: string | symbol, listener: unknown) {
  const state = { fired: false, wrapFn: undefined, target, type, listener };
  const wrapped = onceWrapper.bind(state);
  (wrapped as any).listener = listener;
  (state as any).wrapFn = wrapped;
  return wrapped;
}

EventEmitter.prototype.once = function once(
  type: string | symbol,
  listener: unknown
) {
  validateFunction(listener, 'listener');

  this.on(type, _onceWrap(this, type, listener));
  return this;
};

EventEmitter.prototype.prependOnceListener = function prependOnceListener(
  type: string | symbol,
  listener: unknown
) {
  validateFunction(listener, 'listener');

  this.prependListener(type, _onceWrap(this, type, listener));
  return this;
};

EventEmitter.prototype.removeListener = function removeListener(
  type: string | symbol,
  listener: unknown
) {
  validateFunction(listener, 'listener');

  const events = this._events;
  if (events === undefined) {
    return this;
  }

  const list = events[type] as EventCallback | EventCallback[] | undefined;
  if (list === undefined) {
    return this;
  }

  if (list === listener || ('listener' in list && list.listener === listener)) {
    if (--this._eventsCount === 0) {
      this._events = Object.create(null);
    } else {
      delete events[type];
      if (events.removeListener) {
        this.emit('removeListener', type, list.listener || listener);
      }
    }
  } else if (typeof list !== 'function') {
    let position = -1;

    for (let i = list.length - 1; i >= 0; i--) {
      if (
        list[i] === listener ||
        (list[i] as EventCallback).listener === listener
      ) {
        position = i;
        break;
      }
    }

    if (position < 0) {
      return this;
    }

    if (position === 0) {
      list.shift();
    } else {
      spliceOne(list, position);
    }

    if (list.length === 1) {
      events[type] = list.at(0) as unknown as EventCallback[];
    }

    if (events.removeListener !== undefined) {
      this.emit('removeListener', type, listener);
    }
  }

  return this;
};

EventEmitter.prototype.off = EventEmitter.prototype.removeListener;

EventEmitter.prototype.removeAllListeners = function removeAllListeners(
  type: string | symbol
) {
  const events = this._events;
  if (events === undefined) {
    return this;
  }

  // Not listening for removeListener, no need to emit
  if (events.removeListener === undefined) {
    if (arguments.length === 0) {
      this._events = Object.create(null);
      this._eventsCount = 0;
    } else if (events[type] !== undefined) {
      if (--this._eventsCount === 0) {
        this._events = Object.create(null);
      } else {
        delete events[type];
      }
    }
    return this;
  }

  // Emit removeListener for all listeners on all events
  if (arguments.length === 0) {
    for (const key of Reflect.ownKeys(events)) {
      if (key === 'removeListener') continue;
      this.removeAllListeners(key);
    }
    this.removeAllListeners('removeListener');
    this._events = Object.create(null);
    this._eventsCount = 0;
    return this;
  }

  const listeners = events[type];

  if (typeof listeners === 'function') {
    this.removeListener(type, listeners);
  } else if (listeners !== undefined) {
    // LIFO order
    for (let i = listeners.length - 1; i >= 0; i--) {
      this.removeListener(type, listeners[i] as EventListener);
    }
  }

  return this;
};

function _listeners(target: any, type: string | symbol, unwrap: boolean) {
  const events = target._events;

  if (events === undefined) {
    return [];
  }

  const evlistener = events[type];
  if (evlistener === undefined) {
    return [];
  }

  if (typeof evlistener === 'function') {
    return unwrap ? [evlistener.listener || evlistener] : [evlistener];
  }

  return unwrap ? unwrapListeners(evlistener) : arrayClone(evlistener);
}

EventEmitter.prototype.listeners = function listeners(type: string | symbol) {
  return _listeners(this, type, true);
};

EventEmitter.prototype.rawListeners = function rawListeners(
  type: string | symbol
) {
  return _listeners(this, type, false);
};

const _listenerCount = function listenerCount(
  this: any,
  type: string | symbol
) {
  const events = this._events;

  if (events !== undefined) {
    const evlistener = events[type];

    if (typeof evlistener === 'function') {
      return 1;
    } else if (evlistener !== undefined) {
      return evlistener.length;
    }
  }

  return 0;
};

EventEmitter.prototype.listenerCount = _listenerCount;

export function listenerCount(emitter: any, type: string | symbol) {
  if (typeof emitter.listenerCount === 'function') {
    return emitter.listenerCount(type);
  }
  return _listenerCount.call(emitter, type);
}

EventEmitter.prototype.eventNames = function eventNames() {
  return this._eventsCount > 0 ? Reflect.ownKeys(this._events || {}) : [];
};

function arrayClone(arr: any[]) {
  // At least since V8 8.3, this implementation is faster than the previous
  // which always used a simple for-loop
  switch (arr.length) {
    case 2:
      return [arr[0], arr[1]];
    case 3:
      return [arr[0], arr[1], arr[2]];
    case 4:
      return [arr[0], arr[1], arr[2], arr[3]];
    case 5:
      return [arr[0], arr[1], arr[2], arr[3], arr[4]];
    case 6:
      return [arr[0], arr[1], arr[2], arr[3], arr[4], arr[5]];
  }
  return arr.slice();
}

function unwrapListeners(arr: any[]) {
  const ret = arrayClone(arr);
  for (let i = 0; i < ret.length; ++i) {
    const orig = ret[i].listener;
    if (typeof orig === 'function') {
      ret[i] = orig;
    }
  }
  return ret;
}

export function getEventListeners(emitterOrTarget: any, type: string | symbol) {
  // First check if EventEmitter
  if (typeof emitterOrTarget.listeners === 'function') {
    return emitterOrTarget.listeners(type);
  }
  if (emitterOrTarget instanceof EventTarget) {
    // Workers does not implement the ability to get the event listeners on an
    // EventTarget the way that Node.js does. We simply return empty here.
    return [];
  }
  throw new ERR_INVALID_ARG_TYPE(
    'emitter',
    ['EventEmitter', 'EventTarget'],
    emitterOrTarget
  );
}

export interface OnceOptions {
  signal?: AbortSignal;
}

export async function once(
  emitter: any,
  name: string | symbol,
  options: OnceOptions = {}
) {
  validateObject(options, 'options');
  const { signal } = options;
  validateAbortSignal(signal, 'options.signal');
  if (signal?.aborted) {
    throw new AbortError(undefined, { cause: signal.reason });
  }
  return new Promise((resolve, reject) => {
    const errorListener = (err: any) => {
      emitter.removeListener(name, resolver);
      if (signal != null) {
        eventTargetAgnosticRemoveListener(signal, 'abort', abortListener);
      }
      reject(err);
    };
    const resolver = (...args: any[]) => {
      if (typeof emitter.removeListener === 'function') {
        emitter.removeListener('error', errorListener);
      }
      if (signal != null) {
        eventTargetAgnosticRemoveListener(signal, 'abort', abortListener);
      }
      resolve(args);
    };
    eventTargetAgnosticAddListener(emitter, name, resolver, { once: true });
    if (name !== 'error' && typeof emitter.once === 'function') {
      // EventTarget does not have `error` event semantics like Node
      // EventEmitters, we listen to `error` events only on EventEmitters.
      emitter.once('error', errorListener);
    }
    function abortListener() {
      eventTargetAgnosticRemoveListener(emitter, name, resolver);
      eventTargetAgnosticRemoveListener(emitter, 'error', errorListener);
      reject(new AbortError());
    }
    if (signal != null) {
      eventTargetAgnosticAddListener(signal, 'abort', abortListener, {
        once: true,
      });
    }
  });
}

const AsyncIteratorPrototype = Object.getPrototypeOf(
  Object.getPrototypeOf(async function* () {}).prototype
);

function createIterResult(value: any, done: boolean) {
  return { value, done };
}

function eventTargetAgnosticRemoveListener(
  emitter: any,
  name: string | symbol,
  listener: unknown,
  flags: unknown = undefined
) {
  if (typeof emitter.removeListener === 'function') {
    emitter.removeListener(name, listener);
  } else if (typeof emitter.removeEventListener === 'function') {
    emitter.removeEventListener(name, listener, flags);
  } else {
    throw new ERR_INVALID_ARG_TYPE('emitter', 'EventEmitter', emitter);
  }
}

interface AddListenerFlags {
  once?: boolean;
}

function eventTargetAgnosticAddListener(
  emitter: any,
  name: string | symbol,
  listener: unknown,
  flags: AddListenerFlags = {}
) {
  if (typeof emitter.on === 'function') {
    if (flags?.once) {
      emitter.once(name, listener);
    } else {
      emitter.on(name, listener);
    }
  } else if (typeof emitter.addEventListener === 'function') {
    // EventTarget does not have `error` event semantics like Node
    // EventEmitters, we do not listen to `error` events here.
    emitter.addEventListener(
      name,
      (arg: unknown) => {
        (listener as any)(arg);
      },
      flags
    );
  } else {
    throw new ERR_INVALID_ARG_TYPE('emitter', 'EventEmitter', emitter);
  }
}

interface OnOptions {
  signal?: AbortSignal;
}

export function on(
  emitter: any,
  event: string | symbol,
  options: OnOptions = {}
) {
  const signal = options?.signal;
  validateAbortSignal(signal, 'options.signal');
  if (signal?.aborted) {
    throw new AbortError();
  }

  const unconsumedEvents: any[] = [];
  const unconsumedPromises: any[] = [];
  let error: any = null;
  let finished = false;

  const iterator = Object.setPrototypeOf(
    {
      next() {
        // First, we consume all unread events
        const value = unconsumedEvents.shift();
        if (value) {
          return Promise.resolve(createIterResult(value, false));
        }

        // Then we error, if an error happened
        // This happens one time if at all, because after 'error'
        // we stop listening
        if (error) {
          const p = Promise.reject(error);
          // Only the first element errors
          error = null;
          return p;
        }

        // If the iterator is finished, resolve to done
        if (finished) {
          return Promise.resolve(createIterResult(undefined, true));
        }

        // Wait until an event happens
        return new Promise(function (resolve, reject) {
          unconsumedPromises.push({ resolve, reject });
        });
      },

      return() {
        eventTargetAgnosticRemoveListener(emitter, event, eventHandler);
        eventTargetAgnosticRemoveListener(emitter, 'error', errorHandler);

        if (signal) {
          eventTargetAgnosticRemoveListener(signal, 'abort', abortListener, {
            once: true,
          });
        }

        finished = true;

        for (const promise of unconsumedPromises) {
          promise.resolve(createIterResult(undefined, true));
        }

        return Promise.resolve(createIterResult(undefined, true));
      },

      throw(err: any) {
        if (!err || !(err instanceof Error)) {
          throw new ERR_INVALID_ARG_TYPE(
            'EventEmitter.AsyncIterator',
            'Error',
            err
          );
        }
        error = err;
        eventTargetAgnosticRemoveListener(emitter, event, eventHandler);
        eventTargetAgnosticRemoveListener(emitter, 'error', errorHandler);
      },

      [Symbol.asyncIterator]() {
        return this;
      },
    },
    AsyncIteratorPrototype
  );

  eventTargetAgnosticAddListener(emitter, event, eventHandler);
  if (event !== 'error' && typeof emitter.on === 'function') {
    emitter.on('error', errorHandler);
  }

  if (signal) {
    eventTargetAgnosticAddListener(signal, 'abort', abortListener, {
      once: true,
    });
  }

  return iterator;

  function abortListener() {
    errorHandler(new AbortError());
  }

  function eventHandler(...args: any[]) {
    const promise = unconsumedPromises.shift();
    if (promise) {
      promise.resolve(createIterResult(args, false));
    } else {
      unconsumedEvents.push(args);
    }
  }

  function errorHandler(err: any) {
    finished = true;

    const toError = unconsumedPromises.shift();

    if (toError) {
      toError.reject(err);
    } else {
      // The next time we call next()
      error = err;
    }

    iterator.return();
  }
}
