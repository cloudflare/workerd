// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Subclassing EventTarget. (A subclass's on<type> method is
// reflection.js's.)

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

export const subclassConstruction = {
  test() {
    class MyTarget extends EventTarget {
      constructor() {
        super();
        this.extra = 1;
      }
    }
    const target = new MyTarget();
    ok(target instanceof MyTarget);
    ok(target instanceof EventTarget);
    strictEqual(target.extra, 1);
    strictEqual(Object.prototype.toString.call(target), '[object EventTarget]');
    const receivers = [];
    target.addEventListener('x', function () {
      receivers.push(this);
    });
    strictEqual(target.dispatchEvent(new Event('x')), true);
    strictEqual(receivers.length, 1);
    strictEqual(receivers[0], target);
  },
};

// An override can call up to the method it overrides, which, like the
// other methods, still works on the subclass's instances.
export const subclassOverridesAMethod = {
  test() {
    const log = [];
    class MyTarget extends EventTarget {
      addEventListener(type, listener, options) {
        log.push(['add', type]);
        super.addEventListener(type, listener, options);
      }
    }
    const target = new MyTarget();
    target.addEventListener('x', () => {
      log.push('x');
    });
    EventTarget.prototype.addEventListener.call(target, 'y', () => {
      log.push('y');
    });
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('y'));
    deepStrictEqual(log, [['add', 'x'], 'x', 'y']);
  },
};

export const reflectConstructUsesNewTarget = {
  test() {
    function Derived() {}
    Derived.prototype = Object.create(EventTarget.prototype);
    const target = Reflect.construct(EventTarget, [], Derived);
    strictEqual(Object.getPrototypeOf(target), Derived.prototype);
    let calls = 0;
    target.addEventListener('x', () => {
      calls++;
    });
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// A subclass with state of its own, over the listener machinery.
export const subclassWithItsOwnState = {
  test() {
    class Emitter extends EventTarget {
      #subscriptions = 0;
      on(type, listener) {
        this.addEventListener(type, listener);
        this.#subscriptions++;
        return () => {
          this.removeEventListener(type, listener);
          this.#subscriptions--;
        };
      }
      get subscriptions() {
        return this.#subscriptions;
      }
    }
    const emitter = new Emitter();
    let calls = 0;
    const off = emitter.on('x', () => {
      calls++;
    });
    strictEqual(emitter.subscriptions, 1);
    emitter.dispatchEvent(new Event('x'));
    off();
    emitter.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
    strictEqual(emitter.subscriptions, 0);
  },
};
