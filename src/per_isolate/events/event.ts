'use strict';

// Event and CustomEvent (DOM §2.2, §2.4).
//
// An event's state is its private fields, with the spec's flags packed into
// one word (#flags). There is no initialized flag: without createEvent(),
// every event is initialized.
//
// Dispatch lives in event-target.ts, which drives an event through
// eventInternals, one operation per spec step it performs. The namespace is
// built in Event's static block, so it reaches the private fields, and it is
// never exported to users.

import type { CommonHelpers } from './common';

const {
  DOMException,
  DateNow,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  SymbolToStringTag,
  TypeError,
} = primordials;

const {
  isObject,
  kEnumerable,
  notEnoughArguments,
  toDictionary,
  toStringTagProperty,
} = require('events/common') as CommonHelpers;

// The members of EventInit and CustomEventInit, as read from the user's
// dictionary.
interface EventInitDict {
  bubbles?: unknown;
  cancelable?: unknown;
  composed?: unknown;
}

interface CustomEventInitDict extends EventInitDict {
  detail?: unknown;
}

// #flags. kBubbles, kCancelable and kComposed hold the attributes of those
// names and kTrusted holds isTrusted; the rest are the spec's flags.
const kBubbles = 1 << 0;
const kCancelable = 1 << 1;
const kComposed = 1 << 2;
const kTrusted = 1 << 3;
const kStopPropagation = 1 << 4;
const kStopImmediatePropagation = 1 << 5;
const kCanceled = 1 << 6;
const kInPassiveListener = 1 << 7;
const kDispatch = 1 << 8;

// The eventPhase values. Without a tree, an event is only ever at its target.
const NONE = 0;
const CAPTURING_PHASE = 1;
const AT_TARGET = 2;
const BUBBLING_PHASE = 3;

// WebIDL conversion of an EventInit dictionary to the flags it sets. The
// members are read once each, in order, and converted with ToBoolean.
function convertEventInit(dict: EventInitDict): number {
  return (
    (dict.bubbles ? kBubbles : 0) |
    (dict.cancelable ? kCancelable : 0) |
    (dict.composed ? kComposed : 0)
  );
}

// The operations event-target.ts performs on an event, named for the spec
// steps they implement.
export interface EventInternals {
  // WebIDL's conversion of an Event argument.
  isEvent(value: unknown): value is Event;
  type(event: Event): string;
  // dispatchEvent() steps 1-2 and dispatch step 1: throw if the event is
  // being dispatched; otherwise set the dispatch flag, clear isTrusted, and
  // set the target.
  beginDispatch(event: Event, target: object): void;
  // Invoke steps 4-5: false if the stop propagation flag is set; otherwise
  // set currentTarget.
  enterInvoke(event: Event, target: object): boolean;
  // Inner invoke steps 9 and 12.
  setInPassiveListener(event: Event, value: boolean): void;
  // Inner invoke step 14.
  isImmediatePropagationStopped(event: Event): boolean;
  // Dispatch steps 7-10 and 13: clear currentTarget, the dispatch flag and
  // the stop flags, and return false if the event was canceled. It clears
  // the in passive listener flag too, should an exception have escaped a
  // passive listener's invocation.
  endDispatch(event: Event): boolean;
}

let assertIsEvent: (self: unknown) => void;
// The spec's "initialize" for initEvent() and initCustomEvent(), after
// their dispatch flag check: returns false, having done nothing, if the
// event is being dispatched.
let initializeEvent: (
  event: Event,
  type: string,
  bubbles: boolean,
  cancelable: boolean
) => boolean;
// The [LegacyUnforgeable] isTrusted: an own accessor of every instance,
// with one getter shared by all of them.
let kIsTrustedProperty: PropertyDescriptor;
let eventInternals: EventInternals;

class Event {
  #type: string;
  #flags: number;
  #target: object | null = null;
  #currentTarget: object | null = null;
  #timeStamp: number;

  static {
    assertIsEvent = (self: unknown) => {
      if (!isObject(self) || !(#flags in self)) {
        throw new TypeError('Illegal invocation');
      }
    };

    initializeEvent = (event, type, bubbles, cancelable) => {
      const flags = event.#flags;
      if (flags & kDispatch) return false;
      // Unsets the stop propagation, stop immediate propagation and
      // canceled flags and isTrusted. composed is kept.
      event.#flags =
        (flags & kComposed) |
        (bubbles ? kBubbles : 0) |
        (cancelable ? kCancelable : 0);
      event.#target = null;
      event.#type = type;
      return true;
    };

    // An object literal's getter is named 'get isTrusted', as WebIDL
    // requires.
    const accessor = {
      get isTrusted(): boolean {
        const self: unknown = this;
        assertIsEvent(self);
        return ((self as Event).#flags & kTrusted) !== 0;
      },
    };
    kIsTrustedProperty = ObjectFreeze({
      __proto__: null,
      get: ObjectGetOwnPropertyDescriptor(accessor, 'isTrusted').get,
      enumerable: true,
      configurable: false,
    });

    eventInternals = ObjectFreeze({
      __proto__: null,
      isEvent: (value: unknown): value is Event =>
        isObject(value) && #flags in value,
      type: (event: Event): string => event.#type,
      beginDispatch(event: Event, target: object): void {
        const flags = event.#flags;
        if (flags & kDispatch) {
          throw new DOMException(
            'The event is already being dispatched.',
            'InvalidStateError'
          );
        }
        event.#flags = (flags | kDispatch) & ~kTrusted;
        event.#target = target;
      },
      enterInvoke(event: Event, target: object): boolean {
        if (event.#flags & kStopPropagation) return false;
        event.#currentTarget = target;
        return true;
      },
      setInPassiveListener(event: Event, value: boolean): void {
        event.#flags = value
          ? event.#flags | kInPassiveListener
          : event.#flags & ~kInPassiveListener;
      },
      isImmediatePropagationStopped: (event: Event): boolean =>
        (event.#flags & kStopImmediatePropagation) !== 0,
      endDispatch(event: Event): boolean {
        event.#currentTarget = null;
        const flags =
          event.#flags &
          ~(
            kDispatch |
            kStopPropagation |
            kStopImmediatePropagation |
            kInPassiveListener
          );
        event.#flags = flags;
        return (flags & kCanceled) === 0;
      },
    }) as EventInternals;
  }

  constructor(
    type: string,
    eventInitDict: EventInitDict | undefined = undefined
  ) {
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to construct 'Event'",
        1,
        arguments.length
      );
    }
    this.#type = `${type}`;
    this.#flags = convertEventInit(
      toDictionary(
        eventInitDict,
        "Failed to construct 'Event': The provided value is not of type 'EventInit'."
      )
    );
    this.#timeStamp = DateNow();
    ObjectDefineProperty(this, 'isTrusted', kIsTrustedProperty);
  }

  get type(): string {
    assertIsEvent(this);
    return this.#type;
  }

  get target(): object | null {
    assertIsEvent(this);
    return this.#target;
  }

  get srcElement(): object | null {
    assertIsEvent(this);
    return this.#target;
  }

  get currentTarget(): object | null {
    assertIsEvent(this);
    return this.#currentTarget;
  }

  // The path is the target alone, and currentTarget is set exactly while it
  // is non-empty and user code can observe it.
  composedPath(): object[] {
    assertIsEvent(this);
    const currentTarget = this.#currentTarget;
    return currentTarget === null ? [] : [currentTarget];
  }

  get eventPhase(): number {
    assertIsEvent(this);
    return this.#currentTarget === null ? NONE : AT_TARGET;
  }

  stopPropagation(): void {
    assertIsEvent(this);
    this.#flags |= kStopPropagation;
  }

  get cancelBubble(): boolean {
    assertIsEvent(this);
    return (this.#flags & kStopPropagation) !== 0;
  }

  set cancelBubble(value: boolean) {
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to set the 'cancelBubble' property on 'Event'",
        1,
        0
      );
    }
    assertIsEvent(this);
    if (value) this.#flags |= kStopPropagation;
  }

  stopImmediatePropagation(): void {
    assertIsEvent(this);
    this.#flags |= kStopPropagation | kStopImmediatePropagation;
  }

  get bubbles(): boolean {
    assertIsEvent(this);
    return (this.#flags & kBubbles) !== 0;
  }

  get cancelable(): boolean {
    assertIsEvent(this);
    return (this.#flags & kCancelable) !== 0;
  }

  get returnValue(): boolean {
    assertIsEvent(this);
    return (this.#flags & kCanceled) === 0;
  }

  set returnValue(value: boolean) {
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to set the 'returnValue' property on 'Event'",
        1,
        0
      );
    }
    assertIsEvent(this);
    if (!value) this.#setCanceledFlag();
  }

  preventDefault(): void {
    assertIsEvent(this);
    this.#setCanceledFlag();
  }

  get defaultPrevented(): boolean {
    assertIsEvent(this);
    return (this.#flags & kCanceled) !== 0;
  }

  get composed(): boolean {
    assertIsEvent(this);
    return (this.#flags & kComposed) !== 0;
  }

  get timeStamp(): number {
    assertIsEvent(this);
    return this.#timeStamp;
  }

  initEvent(
    type: string,
    bubbles: boolean = false,
    cancelable: boolean = false
  ): void {
    assertIsEvent(this);
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to execute 'initEvent' on 'Event'",
        1,
        arguments.length
      );
    }
    initializeEvent(this, `${type}`, !!bubbles, !!cancelable);
  }

  // The spec's "set the canceled flag".
  #setCanceledFlag(): void {
    const flags = this.#flags;
    if ((flags & (kCancelable | kInPassiveListener)) === kCancelable) {
      this.#flags = flags | kCanceled;
    }
  }
}

let assertIsCustomEvent: (self: unknown) => void;

class CustomEvent extends Event {
  #detail: unknown = null;

  static {
    assertIsCustomEvent = (self: unknown) => {
      if (!isObject(self) || !(#detail in self)) {
        throw new TypeError('Illegal invocation');
      }
    };
  }

  constructor(
    type: string,
    eventInitDict: CustomEventInitDict | undefined = undefined
  ) {
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to construct 'CustomEvent'",
        1,
        arguments.length
      );
    }
    // Converted here, in WebIDL's order, so that Event's constructor gets a
    // string and a dictionary, whose inherited members it reads before
    // detail is read below.
    const typeString = `${type}`;
    const dict = toDictionary(
      eventInitDict,
      "Failed to construct 'CustomEvent': The provided value is not of type 'CustomEventInit'."
    ) as CustomEventInitDict;
    super(typeString, dict);
    const detail = dict.detail;
    if (detail !== undefined) this.#detail = detail;
  }

  get detail(): unknown {
    assertIsCustomEvent(this);
    return this.#detail;
  }

  initCustomEvent(
    type: string,
    bubbles: boolean = false,
    cancelable: boolean = false,
    detail: unknown = null
  ): void {
    assertIsCustomEvent(this);
    if (arguments.length < 1) {
      throw notEnoughArguments(
        "Failed to execute 'initCustomEvent' on 'CustomEvent'",
        1,
        arguments.length
      );
    }
    if (initializeEvent(this, `${type}`, !!bubbles, !!cancelable)) {
      this.#detail = detail;
    }
  }
}

// WebIDL constants: on the interface object and the prototype, read-only
// and non-configurable.
function constant(value: number): PropertyDescriptor {
  return {
    __proto__: null,
    value,
    writable: false,
    enumerable: true,
    configurable: false,
  } as PropertyDescriptor;
}

const kPhaseConstants = {
  __proto__: null,
  NONE: constant(NONE),
  CAPTURING_PHASE: constant(CAPTURING_PHASE),
  AT_TARGET: constant(AT_TARGET),
  BUBBLING_PHASE: constant(BUBBLING_PHASE),
};

ObjectDefineProperties(Event, kPhaseConstants);

ObjectDefineProperties(Event.prototype, {
  __proto__: null,
  type: kEnumerable,
  target: kEnumerable,
  srcElement: kEnumerable,
  currentTarget: kEnumerable,
  composedPath: kEnumerable,
  eventPhase: kEnumerable,
  stopPropagation: kEnumerable,
  cancelBubble: kEnumerable,
  stopImmediatePropagation: kEnumerable,
  bubbles: kEnumerable,
  cancelable: kEnumerable,
  returnValue: kEnumerable,
  preventDefault: kEnumerable,
  defaultPrevented: kEnumerable,
  composed: kEnumerable,
  timeStamp: kEnumerable,
  initEvent: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('Event'),
});
ObjectDefineProperties(Event.prototype, kPhaseConstants);

ObjectDefineProperties(CustomEvent.prototype, {
  __proto__: null,
  detail: kEnumerable,
  initCustomEvent: kEnumerable,
  [SymbolToStringTag]: toStringTagProperty('CustomEvent'),
});

export type { Event, CustomEvent };

module.exports = {
  Event,
  CustomEvent,
  // For event-target.ts only; never re-exported to users.
  eventInternals,
};
