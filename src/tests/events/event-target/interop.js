// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Targets and events beyond the events implementation. Until the
// integration phases the runtime's own targets (the global, AbortSignal)
// and events (MessageEvent and the rest) stay C++ in the TS cell, and the
// two implementations accept none of each other's objects, so each of these
// diverges (ledger rows marked †; how events cross structuredClone() and
// util.inspect() is event/interop.js's).
//
// Divergences (ledger in AGENTS.md):
// - #20 †: events cannot be dispatched across the implementations, nor
//   their methods called on the other's targets.
// - #21 †: AbortSignal is not an EventTarget.
// - #22 †: node:events' helpers reject AbortSignal.
// - #23 †: structuredClone() of a target.

import {
  deepStrictEqual,
  notStrictEqual,
  strictEqual,
  throws,
} from 'node:assert';
import { getEventListeners, setMaxListeners } from 'node:events';
import util from 'node:util';
import { NativeEvent, NativeEventTarget } from 'helpers';
import { usingTsImpl } from 'which-impl';

// #20 †.
export const eventsAcrossImplementations = {
  test() {
    const signal = new AbortController().signal;
    const target = new EventTarget();
    let signalCalls = 0;
    let targetCalls = 0;
    signal.addEventListener('x', () => {
      signalCalls++;
    });
    target.addEventListener('x', () => {
      targetCalls++;
    });
    strictEqual(signal.dispatchEvent(new NativeEvent('x')), true);
    strictEqual(signalCalls, 1);

    const nativeAtTarget = () => target.dispatchEvent(new MessageEvent('x'));
    const tsAtSignal = () => signal.dispatchEvent(new Event('x'));
    if (usingTsImpl) {
      throws(nativeAtTarget, TypeError);
      throws(tsAtSignal, TypeError);
    } else {
      strictEqual(nativeAtTarget(), true);
      strictEqual(tsAtSignal(), true);
    }
    strictEqual(targetCalls, usingTsImpl ? 0 : 1);
    strictEqual(signalCalls, usingTsImpl ? 1 : 2);
  },
};

// #20 †.
export const methodsAcrossImplementations = {
  test() {
    const listener = () => {};
    const tsMethodOnSignal = () =>
      EventTarget.prototype.addEventListener.call(
        new AbortController().signal,
        'x',
        listener
      );
    const nativeMethodOnTarget = () =>
      NativeEventTarget.prototype.addEventListener.call(
        new EventTarget(),
        'x',
        listener
      );
    if (usingTsImpl) {
      throws(tsMethodOnSignal, TypeError);
      throws(nativeMethodOnTarget, TypeError);
    } else {
      tsMethodOnSignal();
      nativeMethodOnTarget();
    }
  },
};

// The global is a C++ target in both cells, though no EventTarget (its
// prototype chain does not hold EventTarget.prototype): its own methods are
// other functions, which work called bare.
export const theGlobal = {
  test() {
    strictEqual(globalThis instanceof EventTarget, false);
    notStrictEqual(
      globalThis.addEventListener,
      EventTarget.prototype.addEventListener
    );
    const { addEventListener, dispatchEvent, removeEventListener } = globalThis;
    let calls = 0;
    const listener = () => {
      calls++;
    };
    addEventListener('interop', listener);
    try {
      strictEqual(dispatchEvent(new NativeEvent('interop')), true);
      // #20 †.
      if (usingTsImpl) {
        throws(() => dispatchEvent(new Event('interop')), TypeError);
      } else {
        strictEqual(dispatchEvent(new Event('interop')), true);
      }
    } finally {
      removeEventListener('interop', listener);
    }
    strictEqual(calls, usingTsImpl ? 1 : 2);
  },
};

// #21 †.
export const abortSignalIsNotAnEventTarget = {
  test() {
    const { signal } = new AbortController();
    strictEqual(signal instanceof NativeEventTarget, true);
    strictEqual(signal instanceof EventTarget, !usingTsImpl);
    strictEqual(
      Object.getPrototypeOf(AbortSignal.prototype) === EventTarget.prototype,
      !usingTsImpl
    );
  },
};

// #22 †. They take any EventTarget, which they check with instanceof.
// (getEventListeners() returns nothing for any EventTarget: the runtime has
// no way to list a target's listeners.)
export const nodeEventsHelpers = {
  test() {
    const target = new EventTarget();
    target.addEventListener('x', () => {});
    setMaxListeners(5, target);
    deepStrictEqual(getEventListeners(target, 'x'), []);

    const { signal } = new AbortController();
    if (usingTsImpl) {
      throws(() => setMaxListeners(5, signal), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
      throws(() => getEventListeners(signal, 'abort'), {
        code: 'ERR_INVALID_ARG_TYPE',
      });
    } else {
      setMaxListeners(5, signal);
      deepStrictEqual(getEventListeners(signal, 'abort'), []);
    }
  },
};

// #23 †. TS clones a target as an ordinary object of its own enumerable
// properties.
export const structuredCloneDiverges = {
  test() {
    class Target extends EventTarget {
      extra = 1;
    }
    for (const [target, clone] of [
      [new EventTarget(), {}],
      [new Target(), { extra: 1 }],
    ]) {
      if (usingTsImpl) {
        deepStrictEqual(structuredClone(target), clone);
      } else {
        throws(() => structuredClone(target), { name: 'DataCloneError' });
      }
    }
  },
};

// Neither shows a target's listeners.
export const inspectHidesListeners = {
  test() {
    const target = new EventTarget();
    target.addEventListener('x', () => {});
    strictEqual(util.inspect(target), 'EventTarget {}');
  },
};
