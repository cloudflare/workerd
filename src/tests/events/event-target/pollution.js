// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// EventTarget under a polluted environment. Its bookkeeping (the listener
// map and lists, calling listeners, the {signal} abort algorithm) never goes
// through a patchable built-in. The user's own options and listener objects
// are read with [[Get]], so inherited members count, but omitted options
// read nothing.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Defines the entries on Object.prototype; returns a function removing them.
// The descriptors are null-prototype, so no entry defined so far can reach
// a later one's.
function pollute(entries) {
  for (const key of Object.keys(entries)) {
    Object.defineProperty(Object.prototype, key, {
      __proto__: null,
      value: entries[key],
      configurable: true,
      writable: true,
    });
  }
  return () => {
    for (const key of Object.keys(entries)) {
      Reflect.deleteProperty(Object.prototype, key);
    }
  };
}

// Replaces object[key] with value (or, for undefined, deletes it); returns
// a function restoring it.
function patch(object, key, value) {
  const original = Object.getOwnPropertyDescriptor(object, key);
  if (value === undefined) {
    Reflect.deleteProperty(object, key);
  } else {
    Object.defineProperty(object, key, {
      __proto__: null,
      value,
      configurable: true,
      writable: true,
    });
  }
  return () => {
    Object.defineProperty(object, key, original);
  };
}

// Nothing in the patched region may use a patched built-in, the test's own
// code included: no assertions, array methods or for...of until the
// restore.
export const patchedBuiltinsAreNotUsed = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    const first = () => {
      calls++;
    };
    const second = {
      handleEvent() {
        calls++;
      },
    };
    const third = () => {
      calls++;
    };
    const sabotage = () => {
      throw new Error('a patched built-in was used');
    };
    const restores = [
      patch(Array.prototype, 'push', sabotage),
      patch(Array.prototype, 'splice', sabotage),
      patch(Array.prototype, 'indexOf', sabotage),
      patch(Array.prototype, 'slice', sabotage),
      patch(Array.prototype, Symbol.iterator, sabotage),
      patch(Array, Symbol.species, sabotage),
      patch(Map.prototype, 'get', sabotage),
      patch(Map.prototype, 'set', sabotage),
      patch(Map.prototype, 'has', sabotage),
      patch(Map.prototype, 'delete', sabotage),
      patch(Function.prototype, 'call', sabotage),
      patch(Function.prototype, 'apply', sabotage),
      patch(Function.prototype, 'bind', sabotage),
      patch(WeakRef.prototype, 'deref', sabotage),
      patch(globalThis, 'WeakRef', undefined),
    ];
    try {
      target.addEventListener('x', first);
      target.addEventListener('x', second, { signal: controller.signal });
      target.addEventListener('x', third, { once: true });
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
      controller.abort();
      target.dispatchEvent(new Event('x'));
      target.removeEventListener('x', first);
      target.dispatchEvent(new Event('x'));
    } finally {
      for (let i = restores.length - 1; i >= 0; i--) restores[i]();
    }
    // 3 + 2 (third ran once) + 1 (second removed by the abort) + 0.
    strictEqual(calls, 6);
  },
};

// Omitted options read nothing, so polluted members do nothing to them.
export const omittedOptionsReadNothing = {
  test() {
    const restore = pollute({
      capture: true,
      once: true,
      passive: true,
      signal: AbortSignal.abort(),
    });
    let calls = 0;
    try {
      const target = new EventTarget();
      target.addEventListener('x', () => {
        calls++;
      });
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
    } finally {
      restore();
    }
    strictEqual(calls, 2);
  },
};

export const inheritedOptionMembersAreRead = {
  test() {
    let calls = 0;
    const listener = () => {
      calls++;
    };

    let restore = pollute({ once: true });
    try {
      const target = new EventTarget();
      target.addEventListener('x', listener, {});
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
    } finally {
      restore();
    }
    strictEqual(calls, 1);

    // An aborted signal adds nothing.
    restore = pollute({ signal: AbortSignal.abort() });
    try {
      const target = new EventTarget();
      target.addEventListener('x', listener, {});
      target.dispatchEvent(new Event('x'));
    } finally {
      restore();
    }
    strictEqual(calls, 1);

    // capture, which C++ rejects (options.js's #5), and which TS's
    // removeEventListener() reads too.
    let rejected = false;
    restore = pollute({ capture: true });
    try {
      const target = new EventTarget();
      try {
        target.addEventListener('x', listener, {});
      } catch (e) {
        rejected = e instanceof TypeError;
      }
      if (!rejected) {
        target.dispatchEvent(new Event('x'));
        target.removeEventListener('x', listener, {});
        target.dispatchEvent(new Event('x'));
      }
    } finally {
      restore();
    }
    strictEqual(rejected, !usingTsImpl);
    strictEqual(calls, usingTsImpl ? 2 : 1);
  },
};

export const inheritedHandleEventIsCalled = {
  test() {
    let calls = 0;
    const restore = pollute({
      handleEvent() {
        calls++;
      },
    });
    try {
      const target = new EventTarget();
      target.addEventListener('x', {});
      target.dispatchEvent(new Event('x'));
    } finally {
      restore();
    }
    strictEqual(calls, 1);
  },
};
