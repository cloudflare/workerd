// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How a listener is called: an object listener's handleEvent, and what a
// listener returns.
//
// Divergences (ledger in AGENTS.md):
// - #9 (its dispatch side; listeners.js has the add): TS reports a
//   TypeError for an object listener without a callable handleEvent.
// - #10: C++ looks handleEvent up once, as the listener is added, so a
//   throwing getter makes addEventListener() throw; TS looks it up at every
//   call, as WebIDL's "call a user object's operation" does.
// - #11: C++ cancels the event when a listener returns true (and warns,
//   once per target, of any other result that is not a boolean); TS ignores
//   the result.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { collectReportedErrors } from 'helpers';
import { usingTsImpl } from 'which-impl';

// #10.
export const handleEventLookup = {
  test() {
    const target = new EventTarget();
    let reads = 0;
    let calls = 0;
    const listener = {
      get handleEvent() {
        reads++;
        return () => {
          calls++;
        };
      },
    };
    target.addEventListener('x', listener);
    const readsAtAdd = reads;
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(
      { readsAtAdd, reads, calls },
      usingTsImpl
        ? { readsAtAdd: 0, reads: 2, calls: 2 }
        : { readsAtAdd: 1, reads: 1, calls: 2 }
    );
  },
};

// #10.
export const handleEventReplacedAfterAdd = {
  test() {
    const target = new EventTarget();
    const log = [];
    const listener = {
      handleEvent() {
        log.push('old');
      },
    };
    target.addEventListener('x', listener);
    listener.handleEvent = () => {
      log.push('new');
    };
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, [usingTsImpl ? 'new' : 'old']);
  },
};

// #10. In TS the getter's exception is a listener exception, reported as
// any other is.
export const throwingHandleEventGetter = {
  test() {
    const target = new EventTarget();
    const thrown = new Error('getter');
    const listener = {
      get handleEvent() {
        throw thrown;
      },
    };
    if (!usingTsImpl) {
      throws(
        () => target.addEventListener('x', listener),
        (e) => e === thrown
      );
      return;
    }
    const log = [];
    target.addEventListener('x', listener);
    target.addEventListener('x', () => {
      log.push('next');
    });
    const reported = collectReportedErrors(() => {
      target.dispatchEvent(new Event('x'));
    });
    strictEqual(reported.length, 1);
    strictEqual(reported[0], thrown);
    deepStrictEqual(log, ['next']);
  },
};

// #9. A handleEvent defined after the add is used.
export const nonCallableHandleEventIsReported = {
  test() {
    const target = new EventTarget();
    const listener = {};
    if (!usingTsImpl) {
      throws(() => target.addEventListener('x', listener), TypeError);
      return;
    }
    const log = [];
    target.addEventListener('x', listener);
    target.addEventListener('x', () => {
      log.push('next');
    });
    const reported = collectReportedErrors(() => {
      target.dispatchEvent(new Event('x'));
    });
    strictEqual(reported.length, 1);
    ok(reported[0] instanceof TypeError);
    deepStrictEqual(log, ['next']);

    listener.handleEvent = function (e) {
      log.push([this === listener, e.type]);
    };
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['next', [true, 'x'], 'next']);
  },
};

// A callable listener is called itself, whatever handleEvent it has.
export const callableListenerIgnoresHandleEvent = {
  test() {
    const target = new EventTarget();
    const log = [];
    const listener = () => {
      log.push('function');
    };
    listener.handleEvent = () => {
      log.push('handleEvent');
    };
    target.addEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['function']);
  },
};

export const inheritedHandleEvent = {
  test() {
    const log = [];
    class Listener {
      handleEvent(e) {
        log.push([this instanceof Listener, e.type]);
      }
    }
    const target = new EventTarget();
    target.addEventListener('x', new Listener());
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, [[true, 'x']]);
  },
};

// #11. Per spec a listener's result is ignored.
export const listenerReturningTrue = {
  test() {
    for (const cancelable of [true, false]) {
      const target = new EventTarget();
      target.addEventListener('x', () => true);
      const event = new Event('x', { cancelable });
      const result = target.dispatchEvent(event);
      // C++ calls preventDefault(), which only a cancelable event heeds,
      // though the result ignores that (#2).
      deepStrictEqual(
        [result, event.defaultPrevented],
        usingTsImpl ? [true, false] : [false, cancelable],
        `cancelable: ${cancelable}`
      );
    }
  },
};

export const otherResultsAreIgnored = {
  test() {
    for (const value of [false, 1, 'true', {}, null, Promise.resolve(true)]) {
      const target = new EventTarget();
      target.addEventListener('x', () => value);
      const event = new Event('x', { cancelable: true });
      strictEqual(target.dispatchEvent(event), true, String(value));
      strictEqual(event.defaultPrevented, false, String(value));
    }
  },
};
