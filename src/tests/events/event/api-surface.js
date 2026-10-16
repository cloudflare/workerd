// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The WebIDL shape of Event and CustomEvent: where each member lives, its
// property attributes and function metadata, and the brand checks.
//
// Divergences (ledger in AGENTS.md):
// - #1: isTrusted is a prototype getter in C++ (an own data property under
//   pedantic_wpt); TS defines it on each instance, as [LegacyUnforgeable]
//   requires.
// - #11: C++ functions report length 0 and its accessors have empty names.
// - #12: C++ constants are configurable.
// - #13: C++ Symbol.toStringTag is writable.
// - #14: C++ CustomEvent's interface object does not inherit from Event's.
// - #15: C++ prototypes carry the cloudflare:internal-class marker.
// - #16: C++ interface objects' prototype property is writable.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

// Event's attributes, less isTrusted, which #1 places differently.
const kAttributes = [
  'type',
  'target',
  'srcElement',
  'currentTarget',
  'eventPhase',
  'cancelBubble',
  'bubbles',
  'cancelable',
  'returnValue',
  'defaultPrevented',
  'composed',
  'timeStamp',
];

// The operations both implementations have (initEvent() is init-event.js's).
const kOperations = [
  'composedPath',
  'stopPropagation',
  'stopImmediatePropagation',
  'preventDefault',
];

const kConstants = [
  ['NONE', 0],
  ['CAPTURING_PHASE', 1],
  ['AT_TARGET', 2],
  ['BUBBLING_PHASE', 3],
];

// #13.
export const toStringTagBranding = {
  test() {
    for (const [ctor, tag] of [
      [Event, 'Event'],
      [CustomEvent, 'CustomEvent'],
    ]) {
      deepStrictEqual(
        Object.getOwnPropertyDescriptor(ctor.prototype, Symbol.toStringTag),
        {
          value: tag,
          writable: !usingTsImpl,
          enumerable: false,
          configurable: true,
        }
      );
    }
    strictEqual(
      Object.prototype.toString.call(new Event('x')),
      '[object Event]'
    );
    strictEqual(
      Object.prototype.toString.call(new CustomEvent('x')),
      '[object CustomEvent]'
    );
  },
};

export const attributesArePrototypeAccessors = {
  test() {
    for (const name of kAttributes) {
      const { get, set, enumerable, configurable } =
        Object.getOwnPropertyDescriptor(Event.prototype, name);
      strictEqual(typeof get, 'function', name);
      strictEqual(enumerable, true, name);
      strictEqual(configurable, true, name);
      // C++ has no returnValue setter (#5).
      const settable =
        name === 'cancelBubble' || (name === 'returnValue' && usingTsImpl);
      strictEqual(typeof set, settable ? 'function' : 'undefined', name);
    }
    const detail = Object.getOwnPropertyDescriptor(
      CustomEvent.prototype,
      'detail'
    );
    strictEqual(typeof detail.get, 'function');
    strictEqual(detail.set, undefined);
    strictEqual(detail.enumerable, true);
    strictEqual(detail.configurable, true);
    const event = new CustomEvent('x', { detail: 1 });
    for (const name of [...kAttributes, 'detail']) {
      strictEqual(Object.hasOwn(event, name), false, name);
    }
  },
};

export const operationsArePrototypeMethods = {
  test() {
    for (const name of kOperations) {
      const { value, writable, enumerable, configurable } =
        Object.getOwnPropertyDescriptor(Event.prototype, name);
      strictEqual(typeof value, 'function', name);
      strictEqual(value.name, name);
      strictEqual(writable, true, name);
      strictEqual(enumerable, true, name);
      strictEqual(configurable, true, name);
    }
  },
};

// #1.
export const isTrustedPlacement = {
  test() {
    const event = new Event('x');
    strictEqual(event.isTrusted, false);
    if (usingTsImpl) {
      strictEqual(Object.hasOwn(Event.prototype, 'isTrusted'), false);
      const own = Object.getOwnPropertyDescriptor(event, 'isTrusted');
      strictEqual(own.get.name, 'get isTrusted');
      strictEqual(own.set, undefined);
      strictEqual(own.enumerable, true);
      strictEqual(own.configurable, false);
      // One getter, shared by every instance, CustomEvents included.
      strictEqual(
        Object.getOwnPropertyDescriptor(new CustomEvent('y'), 'isTrusted').get,
        own.get
      );
    } else if (pedanticWpt) {
      strictEqual(Object.hasOwn(Event.prototype, 'isTrusted'), false);
      deepStrictEqual(Object.getOwnPropertyDescriptor(event, 'isTrusted'), {
        value: false,
        writable: false,
        enumerable: true,
        configurable: true,
      });
    } else {
      strictEqual(Object.hasOwn(event, 'isTrusted'), false);
      const { get, set, enumerable, configurable } =
        Object.getOwnPropertyDescriptor(Event.prototype, 'isTrusted');
      strictEqual(typeof get, 'function');
      strictEqual(set, undefined);
      strictEqual(enumerable, true);
      strictEqual(configurable, true);
    }
    // It is an instance's only own property, wherever it lives.
    const ownKeys = usingTsImpl || pedanticWpt ? ['isTrusted'] : [];
    deepStrictEqual(Reflect.ownKeys(event), ownKeys);
    deepStrictEqual(
      Reflect.ownKeys(new CustomEvent('x', { detail: 1 })),
      ownKeys
    );
  },
};

// #12.
export const phaseConstants = {
  test() {
    for (const [name, value] of kConstants) {
      for (const holder of [Event, Event.prototype]) {
        deepStrictEqual(
          Object.getOwnPropertyDescriptor(holder, name),
          {
            value,
            writable: false,
            enumerable: true,
            configurable: !usingTsImpl,
          },
          name
        );
      }
      strictEqual(Object.hasOwn(CustomEvent.prototype, name), false, name);
      strictEqual(new CustomEvent('x')[name], value, name);
    }
  },
};

// #14 and #16. WebIDL makes CustomEvent's interface object inherit from
// Event's, and every interface object's prototype property read-only.
export const interfaceObjects = {
  test() {
    strictEqual(Object.getPrototypeOf(Event.prototype), Object.prototype);
    strictEqual(Object.getPrototypeOf(CustomEvent.prototype), Event.prototype);
    strictEqual(Object.getPrototypeOf(Event), Function.prototype);
    strictEqual(
      Object.getPrototypeOf(CustomEvent),
      usingTsImpl ? Event : Function.prototype
    );
    // So only TS's CustomEvent reaches the constants.
    strictEqual(Object.hasOwn(CustomEvent, 'AT_TARGET'), false);
    strictEqual(CustomEvent.AT_TARGET, usingTsImpl ? 2 : undefined);
    for (const ctor of [Event, CustomEvent]) {
      deepStrictEqual(Object.getOwnPropertyDescriptor(ctor, 'prototype'), {
        value: ctor.prototype,
        writable: !usingTsImpl,
        enumerable: false,
        configurable: false,
      });
      deepStrictEqual(
        Object.getOwnPropertyDescriptor(ctor.prototype, 'constructor'),
        { value: ctor, writable: true, enumerable: false, configurable: true }
      );
    }
  },
};

// #11. A sample of the WebIDL values; WPT's idlharness is not run for dom.
export const functionMetadata = {
  test() {
    strictEqual(Event.name, 'Event');
    strictEqual(CustomEvent.name, 'CustomEvent');
    strictEqual(Event.length, usingTsImpl ? 1 : 0);
    strictEqual(CustomEvent.length, usingTsImpl ? 1 : 0);
    for (const name of kOperations) {
      strictEqual(Event.prototype[name].length, 0, name);
    }
    const type = Object.getOwnPropertyDescriptor(Event.prototype, 'type');
    strictEqual(type.get.name, usingTsImpl ? 'get type' : '');
    strictEqual(type.get.length, 0);
    const detail = Object.getOwnPropertyDescriptor(
      CustomEvent.prototype,
      'detail'
    );
    strictEqual(detail.get.name, usingTsImpl ? 'get detail' : '');
    const cancelBubble = Object.getOwnPropertyDescriptor(
      Event.prototype,
      'cancelBubble'
    );
    strictEqual(cancelBubble.get.name, usingTsImpl ? 'get cancelBubble' : '');
    strictEqual(cancelBubble.set.name, usingTsImpl ? 'set cancelBubble' : '');
    strictEqual(cancelBubble.set.length, usingTsImpl ? 1 : 0);
  },
};

// Every member rejects a receiver that is not an Event, an object that
// merely inherits from Event.prototype included, and detail rejects an
// Event that is not a CustomEvent.
export const brandChecks = {
  test() {
    const cancelBubble = Object.getOwnPropertyDescriptor(
      Event.prototype,
      'cancelBubble'
    ).set;
    for (const receiver of [
      {},
      undefined,
      null,
      Object.create(Event.prototype),
    ]) {
      for (const name of kAttributes) {
        const { get } = Object.getOwnPropertyDescriptor(Event.prototype, name);
        throws(() => get.call(receiver), TypeError, name);
      }
      for (const name of kOperations) {
        throws(() => Event.prototype[name].call(receiver), TypeError, name);
      }
      throws(() => cancelBubble.call(receiver, true), TypeError);
    }
    // isTrusted's getter, where it has one (#1).
    const isTrusted = usingTsImpl
      ? Object.getOwnPropertyDescriptor(new Event('x'), 'isTrusted').get
      : Object.getOwnPropertyDescriptor(Event.prototype, 'isTrusted')?.get;
    strictEqual(typeof isTrusted, pedanticWpt ? 'undefined' : 'function');
    if (isTrusted !== undefined) {
      throws(() => isTrusted.call({}), TypeError);
    }
    const detail = Object.getOwnPropertyDescriptor(
      CustomEvent.prototype,
      'detail'
    ).get;
    throws(() => detail.call(new Event('x')), TypeError);
    // Event's members accept a CustomEvent.
    const type = Object.getOwnPropertyDescriptor(Event.prototype, 'type').get;
    strictEqual(type.call(new CustomEvent('y')), 'y');
  },
};

export const constructorsRequireNew = {
  test() {
    throws(() => Event('x'), TypeError);
    throws(() => CustomEvent('x'), TypeError);
  },
};

// #15.
export const internalClassMarker = {
  test() {
    const marker = Symbol.for('cloudflare:internal-class');
    for (const ctor of [Event, CustomEvent]) {
      strictEqual(ctor.prototype[marker], usingTsImpl ? undefined : marker);
    }
  },
};
