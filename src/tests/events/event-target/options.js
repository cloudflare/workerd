// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The options argument: its conversion, capture, passive and once
// (ported in part from WPT's EventListenerOptions-capture.html). The
// signal member is signal.js's.
//
// Divergences (ledger in AGENTS.md):
// - #5: C++ rejects capture (useCapture or {capture}) with a TypeError; in
//   TS capture is part of a listener's identity and capture listeners run
//   first.
// - #6: C++ rejects {passive: true}; in TS a passive listener cannot cancel
//   the event.
// - #7: C++ reads the members in declaration order (capture, passive, once,
//   signal); TS in WebIDL's (capture, once, passive, signal).
// - #8: C++'s removeEventListener() rejects capture too, even for a null
//   listener; in TS it removes the matching capture listener, if any.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { collectReportedErrors } from 'helpers';
import { usingTsImpl } from 'which-impl';

// The C++ side of #5: pins the rejection, then stops the test.
function acceptsCapture(target) {
  if (usingTsImpl) return true;
  throws(() => target.addEventListener('x', () => {}, true), TypeError);
  return false;
}

// The C++ side of #6, likewise.
function acceptsPassive(target) {
  if (usingTsImpl) return true;
  throws(
    () => target.addEventListener('x', () => {}, { passive: true }),
    TypeError
  );
  return false;
}

// Options that ask for nothing, in every form: ToBoolean for a primitive,
// the dictionary's members for an object (a function or an array too).
export const inertOptions = {
  test() {
    for (const options of [
      undefined,
      null,
      false,
      0,
      NaN,
      '',
      0n,
      {},
      [],
      () => {},
      { capture: false },
      { capture: null },
      { passive: false },
      { once: false },
      { signal: undefined },
    ]) {
      const target = new EventTarget();
      let calls = 0;
      target.addEventListener('x', () => calls++, options);
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
      strictEqual(calls, 2, String(options));
    }
  },
};

// #5.
export const captureOption = {
  test() {
    for (const options of [
      true,
      1,
      'capture',
      Symbol('s'),
      1n,
      { capture: true },
      { capture: 'yes' },
    ]) {
      const target = new EventTarget();
      let calls = 0;
      const add = () => target.addEventListener('x', () => calls++, options);
      if (usingTsImpl) {
        add();
        target.dispatchEvent(new Event('x'));
        strictEqual(calls, 1, String(options));
      } else {
        throws(add, TypeError, String(options));
      }
    }
  },
};

// #5. Both passes run at the target, so each listener sees AT_TARGET.
export const captureListenersRunFirst = {
  test() {
    const target = new EventTarget();
    if (!acceptsCapture(target)) return;
    const log = [];
    const record = (name) => (e) => log.push([name, e.eventPhase]);
    target.addEventListener('x', record('bubble 1'));
    target.addEventListener('x', record('capture 1'), true);
    target.addEventListener('x', record('bubble 2'), { capture: false });
    target.addEventListener('x', record('capture 2'), { capture: true });
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, [
      ['capture 1', 2],
      ['capture 2', 2],
      ['bubble 1', 2],
      ['bubble 2', 2],
    ]);
  },
};

// #5.
export const captureIsPartOfIdentity = {
  test() {
    const target = new EventTarget();
    if (!acceptsCapture(target)) return;
    let calls = 0;
    const listener = () => calls++;
    target.addEventListener('x', listener, true);
    target.addEventListener('x', listener, false);
    target.addEventListener('x', listener, { capture: true });
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 2);
    target.removeEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 3);
    target.removeEventListener('x', listener, { capture: true });
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 3);
  },
};

// #5. The target's capture listeners are one invoke, so stopPropagation()
// in one lets the rest run, then skips the bubbling pass.
export const stopPropagationInCaptureSkipsBubbling = {
  test() {
    const target = new EventTarget();
    if (!acceptsCapture(target)) return;
    const log = [];
    target.addEventListener(
      'x',
      (e) => {
        log.push('capture 1');
        e.stopPropagation();
      },
      true
    );
    target.addEventListener('x', () => log.push('capture 2'), true);
    target.addEventListener('x', () => log.push('bubble'));
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['capture 1', 'capture 2']);
  },
};

// #5. Each pass clones the list as it starts, so a listener added during
// the capturing pass runs in the bubbling pass if it is not a capture one.
export const listenerAddedDuringCaptureRunsInBubbling = {
  test() {
    const target = new EventTarget();
    if (!acceptsCapture(target)) return;
    const log = [];
    target.addEventListener(
      'x',
      () => {
        log.push('capture');
        target.addEventListener('x', () => log.push('late bubble'));
        target.addEventListener('x', () => log.push('late capture'), true);
      },
      true
    );
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['capture', 'late bubble']);
  },
};

// #6.
export const passiveListenerCannotCancel = {
  test() {
    const target = new EventTarget();
    if (!acceptsPassive(target)) return;
    const log = [];
    target.addEventListener(
      'x',
      (e) => {
        e.preventDefault();
        log.push(e.defaultPrevented);
        e.returnValue = false;
        log.push(e.defaultPrevented);
      },
      { passive: true }
    );
    target.addEventListener('x', (e) => {
      e.preventDefault();
      log.push(e.defaultPrevented);
    });
    strictEqual(
      target.dispatchEvent(new Event('x', { cancelable: true })),
      false
    );
    deepStrictEqual(log, [false, false, true]);
  },
};

// #6. A duplicate keeps the first registration's passive.
export const passiveIsNotPartOfIdentity = {
  test() {
    const target = new EventTarget();
    if (!acceptsPassive(target)) return;
    let calls = 0;
    const listener = (e) => {
      calls++;
      e.preventDefault();
    };
    target.addEventListener('x', listener, { passive: true });
    target.addEventListener('x', listener, { passive: false });
    strictEqual(
      target.dispatchEvent(new Event('x', { cancelable: true })),
      true
    );
    strictEqual(calls, 1);
    target.removeEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// #6. The flag is reset after a passive listener throws.
export const passiveFlagResetsAfterAThrow = {
  test() {
    const target = new EventTarget();
    if (!acceptsPassive(target)) return;
    const thrown = new Error('passive');
    let canceled;
    let result;
    const reported = collectReportedErrors(() => {
      target.addEventListener(
        'x',
        () => {
          throw thrown;
        },
        { passive: true }
      );
      target.addEventListener('x', (e) => {
        e.preventDefault();
        canceled = e.defaultPrevented;
      });
      result = target.dispatchEvent(new Event('x', { cancelable: true }));
    });
    strictEqual(reported.length, 1);
    strictEqual(reported[0], thrown);
    strictEqual(canceled, true);
    strictEqual(result, false);
  },
};

// #7. Every member is read once, after the type is converted, even for a
// null listener; removeEventListener() reads capture alone.
export const optionsReadOrder = {
  test() {
    function readsOf(method, listener) {
      const log = [];
      const options = {};
      for (const name of ['capture', 'once', 'passive', 'signal']) {
        Object.defineProperty(options, name, {
          get() {
            log.push(name);
            return undefined;
          },
        });
      }
      const type = {
        toString() {
          log.push('type');
          return 'x';
        },
      };
      new EventTarget()[method](type, listener, options);
      return log;
    }
    const expected = usingTsImpl
      ? ['type', 'capture', 'once', 'passive', 'signal']
      : ['type', 'capture', 'passive', 'once', 'signal'];
    deepStrictEqual(
      readsOf('addEventListener', () => {}),
      expected
    );
    deepStrictEqual(readsOf('addEventListener', null), expected);
    deepStrictEqual(
      readsOf('removeEventListener', () => {}),
      ['type', 'capture']
    );
    deepStrictEqual(readsOf('removeEventListener', null), ['type', 'capture']);
  },
};

// once removes the listener before calling it, so a listener that adds
// itself again registers afresh, for the next dispatch.
export const onceRemovesBeforeInvoking = {
  test() {
    const target = new EventTarget();
    let calls = 0;
    const listener = () => {
      calls++;
      target.addEventListener('x', listener);
    };
    target.addEventListener('x', listener, { once: true });
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 2);
  },
};

export const onceIsConvertedToBoolean = {
  test() {
    for (const [once, expected] of [
      [1, 1],
      ['yes', 1],
      [{}, 1],
      [0, 2],
      ['', 2],
      [null, 2],
    ]) {
      const target = new EventTarget();
      let calls = 0;
      target.addEventListener('x', () => calls++, { once });
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
      strictEqual(calls, expected, String(once));
    }
  },
};

// A once listener removed by an earlier one does not run.
export const onceListenerRemovedEarlierDoesNotRun = {
  test() {
    const target = new EventTarget();
    const log = [];
    const second = () => log.push('second');
    target.addEventListener('x', () => {
      log.push('first');
      target.removeEventListener('x', second);
    });
    target.addEventListener('x', second, { once: true });
    target.dispatchEvent(new Event('x'));
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['first', 'first']);
  },
};

// #8.
export const removeWithCapture = {
  test() {
    const listener = () => {};
    for (const options of [true, 1, { capture: true }]) {
      for (const candidate of [listener, null]) {
        const target = new EventTarget();
        const remove = () =>
          target.removeEventListener('x', candidate, options);
        if (usingTsImpl) {
          remove();
        } else {
          throws(remove, TypeError, String(options));
        }
      }
    }
  },
};
