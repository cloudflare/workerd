// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Subclassing Event and CustomEvent.
//
// Divergence (ledger in AGENTS.md):
// - #1: TS's isTrusted is an own non-configurable property of every
//   instance ([LegacyUnforgeable]), so nothing can redefine it; C++'s (a
//   prototype getter, or a configurable own data property under
//   pedantic_wpt) can be shadowed, by a class field for one.

import { ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const subclassConstruction = {
  test() {
    class MyEvent extends Event {
      constructor(type) {
        super(type, { bubbles: true });
        this.extra = 1;
      }
    }
    const event = new MyEvent('x');
    ok(event instanceof MyEvent);
    ok(event instanceof Event);
    strictEqual(event.type, 'x');
    strictEqual(event.bubbles, true);
    strictEqual(event.isTrusted, false);
    strictEqual(event.extra, 1);
    strictEqual(Object.prototype.toString.call(event), '[object Event]');
  },
};

export const subclassOverridesAnAttribute = {
  test() {
    class MyEvent extends Event {
      get bubbles() {
        return 'overridden';
      }
    }
    const event = new MyEvent('x');
    strictEqual(event.bubbles, 'overridden');
    const { get } = Object.getOwnPropertyDescriptor(Event.prototype, 'bubbles');
    strictEqual(get.call(event), false);
  },
};

// #1.
export const isTrustedCannotBeRedefined = {
  test() {
    class Forged extends Event {
      isTrusted = true;
    }
    const event = new Event('x');
    if (usingTsImpl) {
      throws(() => new Forged('x'), TypeError);
      throws(
        () => Object.defineProperty(event, 'isTrusted', { value: true }),
        TypeError
      );
      strictEqual(event.isTrusted, false);
    } else {
      strictEqual(new Forged('x').isTrusted, true);
      Object.defineProperty(event, 'isTrusted', { value: true });
      strictEqual(event.isTrusted, true);
    }
  },
};

export const reflectConstructUsesNewTarget = {
  test() {
    function Derived() {}
    Derived.prototype = Object.create(Event.prototype);
    const event = Reflect.construct(
      Event,
      ['x', { cancelable: true }],
      Derived
    );
    strictEqual(Object.getPrototypeOf(event), Derived.prototype);
    strictEqual(event.type, 'x');
    event.preventDefault();
    strictEqual(event.defaultPrevented, true);

    // The instance is a CustomEvent though its prototype is not
    // CustomEvent's.
    const custom = Reflect.construct(
      CustomEvent,
      ['y', { detail: 1 }],
      Derived
    );
    strictEqual(Object.getPrototypeOf(custom), Derived.prototype);
    const { get } = Object.getOwnPropertyDescriptor(
      CustomEvent.prototype,
      'detail'
    );
    strictEqual(get.call(custom), 1);
  },
};

export const customEventSubclass = {
  test() {
    class MyCustomEvent extends CustomEvent {}
    const event = new MyCustomEvent('x', { detail: 3 });
    strictEqual(event.detail, 3);
    ok(event instanceof CustomEvent);
    ok(event instanceof Event);
    strictEqual(Object.prototype.toString.call(event), '[object CustomEvent]');
  },
};

export const subclassInstancesDispatch = {
  test() {
    class MyEvent extends Event {}
    const target = new EventTarget();
    const event = new MyEvent('x');
    let seen;
    target.addEventListener('x', (e) => {
      seen = e;
    });
    strictEqual(target.dispatchEvent(event), true);
    strictEqual(seen, event);
  },
};
