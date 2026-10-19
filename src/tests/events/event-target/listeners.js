// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Adding and removing listeners: argument conversion, identity, order, and
// how a listener is called. The options argument is options.js's.
//
// Divergence (ledger in AGENTS.md):
// - #9: C++ rejects an object listener without a callable handleEvent when
//   it is added; TS accepts any object (a WebIDL callback interface) and
//   reports the failure at dispatch (handle-event.js).

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const argumentCounts = {
  test() {
    const target = new EventTarget();
    throws(() => target.addEventListener(), TypeError);
    throws(() => target.addEventListener('x'), TypeError);
    throws(() => target.removeEventListener(), TypeError);
    throws(() => target.removeEventListener('x'), TypeError);
    throws(() => target.dispatchEvent(), TypeError);
  },
};

// The type is converted first, so its toString() runs even when the
// listener is then rejected.
export const argumentsAreConvertedInOrder = {
  test() {
    for (const method of ['addEventListener', 'removeEventListener']) {
      const log = [];
      const type = {
        toString() {
          log.push('type');
          return 'x';
        },
      };
      throws(() => new EventTarget()[method](type, 1), TypeError, method);
      deepStrictEqual(log, ['type'], method);
    }
  },
};

// A null or undefined listener is no listener: adding it does nothing,
// whatever the options. (Removing one with capture is options.js's #8.)
export const nullishListenerIsIgnored = {
  test() {
    const target = new EventTarget();
    for (const listener of [null, undefined]) {
      for (const options of [
        undefined,
        false,
        true,
        { capture: true },
        { once: true },
        { passive: true },
      ]) {
        target.addEventListener('x', listener, options);
      }
      for (const options of [undefined, false, { capture: false }]) {
        target.removeEventListener('x', listener, options);
      }
    }
    strictEqual(target.dispatchEvent(new Event('x')), true);
  },
};

export const primitiveListenersThrow = {
  test() {
    const target = new EventTarget();
    for (const listener of [1, 'listener', false, true, Symbol('s'), 1n]) {
      throws(
        () => target.addEventListener('x', listener),
        TypeError,
        String(listener)
      );
      throws(
        () => target.removeEventListener('x', listener),
        TypeError,
        String(listener)
      );
    }
  },
};

// #9. removeEventListener() accepts any object in both.
export const nonCallableObjectListener = {
  test() {
    for (const listener of [
      {},
      [],
      { handleEvent: 1 },
      { handleEvent: {} },
      new EventTarget(),
    ]) {
      const target = new EventTarget();
      if (usingTsImpl) {
        target.addEventListener('x', listener);
      } else {
        throws(() => target.addEventListener('x', listener), TypeError);
      }
      target.removeEventListener('x', listener);
    }
  },
};

// A listener is identified by itself (and its capture flag: options.js), so
// adding it again is a no-op that keeps the first registration's options.
export const duplicatesAreIgnored = {
  test() {
    const target = new EventTarget();
    let calls = 0;
    const listener = () => calls++;
    target.addEventListener('x', listener);
    target.addEventListener('x', listener);
    target.addEventListener('x', listener, false);
    target.addEventListener('x', listener, { once: true });
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 2);

    const object = {
      handleEvent() {
        calls++;
      },
    };
    target.addEventListener('y', object);
    target.addEventListener('y', object);
    target.dispatchEvent(new Event('y'));
    strictEqual(calls, 3);
  },
};

export const removalMatchesByIdentity = {
  test() {
    const target = new EventTarget();
    const log = [];
    const listener = () => log.push('listener');
    target.addEventListener('x', listener);
    target.removeEventListener('x', () => log.push('listener'));
    target.removeEventListener('y', listener);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['listener']);

    target.removeEventListener('x', listener);
    target.removeEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['listener']);
    new EventTarget().removeEventListener('x', listener);
  },
};

// Ported from WPT's Event-dispatch-order-at-target.html: registration
// order, and re-adding a removed listener moves it to the end.
export const listenersRunInRegistrationOrder = {
  test() {
    const target = new EventTarget();
    const log = [];
    const a = () => log.push('a');
    const b = { handleEvent: () => log.push('b') };
    const c = () => log.push('c');
    target.addEventListener('x', a);
    target.addEventListener('x', b);
    target.addEventListener('x', c);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['a', 'b', 'c']);

    target.removeEventListener('x', a);
    target.addEventListener('x', a);
    log.length = 0;
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['b', 'c', 'a']);
  },
};

// Ported from WPT's EventTarget-this-of-listener.html: a function is called
// with the target as `this`, a handleEvent with its object, and either with
// the event alone.
export const listenerInvocation = {
  test() {
    const target = new EventTarget();
    const event = new Event('x');
    const calls = [];
    target.addEventListener('x', function (...args) {
      calls.push({ self: this, args });
    });
    const object = {
      handleEvent(...args) {
        calls.push({ self: this, args });
      },
    };
    target.addEventListener('x', object);
    target.dispatchEvent(event);
    strictEqual(calls.length, 2);
    strictEqual(calls[0].self, target);
    strictEqual(calls[1].self, object);
    for (const { args } of calls) {
      strictEqual(args.length, 1);
      strictEqual(args[0], event);
    }
  },
};

// ToString, so a Symbol throws; types are compared exactly.
export const typeIsConvertedToString = {
  test() {
    const target = new EventTarget();
    const log = [];
    target.addEventListener({ toString: () => 'x' }, () => log.push('x'));
    target.addEventListener(null, () => log.push('null'));
    target.addEventListener(1, () => log.push('1'));
    for (const type of ['x', 'null', '1', 'X', ' x']) {
      target.dispatchEvent(new Event(type));
    }
    deepStrictEqual(log, ['x', 'null', '1']);
    throws(() => target.addEventListener(Symbol('s'), () => {}), TypeError);
    throws(() => target.removeEventListener(Symbol('s'), () => {}), TypeError);
  },
};

// One listener on two targets is two registrations.
export const registrationsArePerTarget = {
  test() {
    const first = new EventTarget();
    const second = new EventTarget();
    const seen = [];
    const listener = function () {
      seen.push(this);
    };
    first.addEventListener('x', listener);
    second.addEventListener('x', listener);
    first.removeEventListener('x', listener);
    first.dispatchEvent(new Event('x'));
    second.dispatchEvent(new Event('x'));
    strictEqual(seen.length, 1);
    strictEqual(seen[0], second);
  },
};
