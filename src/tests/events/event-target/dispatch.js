// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Dispatch: dispatchEvent()'s argument and result, an event's attributes
// during and after dispatch, the canceled and stop propagation flags across
// a dispatch, and the listener list changing during one.
//
// Divergences (ledger in AGENTS.md):
// - #1: C++ leaves currentTarget set once dispatch ends (null under
//   pedantic_wpt).
// - #2: C++'s dispatchEvent() returns false once preventDefault() has been
//   called, even for an event that is not cancelable.
// - #3: C++ unsets the canceled flag as dispatch starts.
// - #4: C++ never consults the stop propagation flag, and never unsets
//   either stop flag: stopPropagation() before dispatch stops nothing, and
//   an event stopped immediately never reaches a listener again.
// - #13: C++ runs a listener removed and re-added during the dispatch.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

export const argumentMustBeAnEvent = {
  test() {
    const target = new EventTarget();
    for (const value of [
      undefined,
      null,
      {},
      'x',
      Object.create(Event.prototype),
    ]) {
      throws(() => target.dispatchEvent(value), TypeError, String(value));
    }
  },
};

export const attributesDuringDispatch = {
  test() {
    const target = new EventTarget();
    const event = new Event('x');
    let seen;
    target.addEventListener('x', (e) => {
      const path = e.composedPath();
      seen = {
        event: e === event,
        target: e.target === target,
        currentTarget: e.currentTarget === target,
        srcElement: e.srcElement === target,
        eventPhase: e.eventPhase,
        path: path.length === 1 && path[0] === target,
        isTrusted: e.isTrusted,
      };
    });
    target.dispatchEvent(event);
    deepStrictEqual(seen, {
      event: true,
      target: true,
      currentTarget: true,
      srcElement: true,
      eventPhase: Event.AT_TARGET,
      path: true,
      isTrusted: false,
    });
  },
};

// #1.
export const attributesAfterDispatch = {
  test() {
    const target = new EventTarget();
    const event = new Event('x');
    strictEqual(target.dispatchEvent(event), true);
    strictEqual(event.target, target);
    strictEqual(event.srcElement, target);
    strictEqual(event.eventPhase, Event.NONE);
    deepStrictEqual(event.composedPath(), []);
    strictEqual(
      event.currentTarget,
      usingTsImpl || pedanticWpt ? null : target
    );
  },
};

// #2.
export const dispatchResult = {
  test() {
    function dispatch(init, cancel) {
      const target = new EventTarget();
      if (cancel) {
        target.addEventListener('x', (e) => {
          e.preventDefault();
        });
      }
      const event = new Event('x', init);
      return [target.dispatchEvent(event), event.defaultPrevented];
    }
    deepStrictEqual(dispatch({ cancelable: true }, false), [true, false]);
    deepStrictEqual(dispatch({ cancelable: true }, true), [false, true]);
    deepStrictEqual(dispatch({}, false), [true, false]);
    // preventDefault() does nothing to an event that is not cancelable.
    deepStrictEqual(dispatch({}, true), [usingTsImpl, false]);
  },
};

// #3. Per spec only initEvent() unsets the canceled flag, so an event
// canceled before dispatch, or in an earlier one, stays canceled.
export const canceledFlagSurvivesDispatch = {
  test() {
    const target = new EventTarget();
    const seen = [];
    target.addEventListener('x', (e) => {
      seen.push(e.defaultPrevented);
    });

    const before = new Event('x', { cancelable: true });
    before.preventDefault();
    const beforeResult = target.dispatchEvent(before);

    const canceler = new EventTarget();
    canceler.addEventListener('x', (e) => {
      e.preventDefault();
    });
    const earlier = new Event('x', { cancelable: true });
    strictEqual(canceler.dispatchEvent(earlier), false);
    const earlierResult = target.dispatchEvent(earlier);

    const kept = usingTsImpl;
    deepStrictEqual([beforeResult, earlierResult], [!kept, !kept]);
    deepStrictEqual(seen, [kept, kept]);
    strictEqual(before.defaultPrevented, kept);
    strictEqual(earlier.defaultPrevented, kept);
  },
};

// #4. Per spec a dispatch of a stopped event runs no listener, and every
// dispatch unsets the stop flags as it ends.
export const stopFlagsSetBeforeDispatch = {
  test() {
    // [calls after one dispatch, after two, cancelBubble]
    const expected = usingTsImpl
      ? {
          stopPropagation: [0, 1, false],
          stopImmediatePropagation: [0, 1, false],
        }
      : {
          stopPropagation: [1, 2, true],
          stopImmediatePropagation: [0, 0, false],
        };
    for (const stop of ['stopPropagation', 'stopImmediatePropagation']) {
      const target = new EventTarget();
      let calls = 0;
      target.addEventListener('x', () => {
        calls++;
      });
      const event = new Event('x');
      event[stop]();
      target.dispatchEvent(event);
      const afterOne = calls;
      target.dispatchEvent(event);
      deepStrictEqual(
        [afterOne, calls, event.cancelBubble],
        expected[stop],
        stop
      );
    }
  },
};

// #4. stopPropagation() lets the target's other listeners run, as there is
// no other target to stop at; stopImmediatePropagation() stops them.
export const stopFlagsSetDuringDispatch = {
  test() {
    // [first dispatch, cancelBubble after it, second dispatch]
    const expected = {
      stopPropagation: [['a', 'b'], !usingTsImpl, ['a', 'b']],
      stopImmediatePropagation: [['a'], false, usingTsImpl ? ['a'] : []],
    };
    for (const stop of ['stopPropagation', 'stopImmediatePropagation']) {
      const target = new EventTarget();
      const log = [];
      target.addEventListener('x', (e) => {
        log.push('a');
        e[stop]();
      });
      target.addEventListener('x', () => {
        log.push('b');
      });
      const event = new Event('x');
      target.dispatchEvent(event);
      const first = log.splice(0);
      const cancelBubble = event.cancelBubble;
      target.dispatchEvent(event);
      deepStrictEqual([first, cancelBubble, log], expected[stop], stop);
    }
  },
};

export const removedListenerDoesNotRun = {
  test() {
    const target = new EventTarget();
    const log = [];
    const second = () => {
      log.push('second');
    };
    target.addEventListener('x', () => {
      log.push('first');
      target.removeEventListener('x', second);
    });
    target.addEventListener('x', second);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['first']);
  },
};

// A listener added during a dispatch waits for the next one.
export const addedListenerWaitsForTheNextDispatch = {
  test() {
    const target = new EventTarget();
    const log = [];
    target.addEventListener(
      'x',
      () => {
        log.push('first');
        target.addEventListener('x', () => {
          log.push('late');
        });
      },
      { once: true }
    );
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['first']);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['first', 'late']);
  },
};

// #13. Per spec a dispatch runs over a clone of the listener list taken as
// it starts: removing a listener marks its registration removed, and adding
// it again makes a new registration, which the clone does not hold.
export const reAddedListenerDuringDispatch = {
  test() {
    const target = new EventTarget();
    const log = [];
    const second = () => {
      log.push('second');
    };
    target.addEventListener('x', () => {
      log.push('first');
      target.removeEventListener('x', second);
      target.addEventListener('x', second);
    });
    target.addEventListener('x', second);
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, usingTsImpl ? ['first'] : ['first', 'second']);
  },
};

// An event being dispatched cannot be dispatched again, at any target, and
// the failed attempt leaves the dispatch in progress alone.
export const reentrantDispatchThrows = {
  test() {
    const target = new EventTarget();
    const other = new EventTarget();
    const log = [];
    const errors = [];
    target.addEventListener('x', (e) => {
      log.push('first');
      for (const at of [target, other]) {
        try {
          at.dispatchEvent(e);
        } catch (error) {
          errors.push(error);
        }
      }
      log.push(e.currentTarget === target);
    });
    target.addEventListener('x', () => {
      log.push('second');
    });
    strictEqual(target.dispatchEvent(new Event('x')), true);
    deepStrictEqual(log, ['first', true, 'second']);
    strictEqual(errors.length, 2);
    for (const error of errors) {
      ok(error instanceof DOMException);
      strictEqual(error.name, 'InvalidStateError');
      strictEqual(error.code, 11);
    }
  },
};

// Another event can be dispatched from a listener, at the same target.
export const nestedDispatch = {
  test() {
    const target = new EventTarget();
    const outer = new Event('outer');
    const inner = new Event('inner');
    const log = [];
    target.addEventListener('outer', (e) => {
      log.push(['outer', e.eventPhase]);
      target.dispatchEvent(inner);
      log.push(['outer again', e.eventPhase, e.currentTarget === target]);
    });
    target.addEventListener('inner', (e) => {
      log.push(['inner', e.eventPhase, outer.eventPhase]);
    });
    target.dispatchEvent(outer);
    deepStrictEqual(log, [
      ['outer', Event.AT_TARGET],
      ['inner', Event.AT_TARGET, Event.AT_TARGET],
      ['outer again', Event.AT_TARGET, true],
    ]);
  },
};

export const redispatchAtAnotherTarget = {
  test() {
    const first = new EventTarget();
    const second = new EventTarget();
    const event = new Event('x');
    let seen;
    second.addEventListener('x', (e) => {
      seen = e.target;
    });
    first.dispatchEvent(event);
    strictEqual(event.target, first);
    second.dispatchEvent(event);
    strictEqual(seen, second);
    strictEqual(event.target, second);
  },
};

// Types are arbitrary strings, matched exactly, even ones that name
// Object.prototype members.
export const unusualTypes = {
  test() {
    const types = [
      '',
      ' ',
      'é',
      '\u{1F600}',
      'on',
      '__proto__',
      'constructor',
      'hasOwnProperty',
    ];
    const target = new EventTarget();
    const log = [];
    for (const type of types) {
      target.addEventListener(type, () => {
        log.push(type);
      });
    }
    for (const type of types) {
      strictEqual(target.dispatchEvent(new Event(type)), true, type);
    }
    deepStrictEqual(log, types);
  },
};
