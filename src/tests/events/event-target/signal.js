// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The {signal} option (ported in part from WPT's
// AddEventListenerOptions-signal.any.js): aborting the signal removes the
// listener, through an abort algorithm, so before the signal's 'abort'
// event and never for a synthetic one.
//
// Divergence (ledger in AGENTS.md):
// - #14: TS checks that the signal is an AbortSignal through C++'s aborted
//   getter. Under the instance-property layout (at old dates, without
//   workers_api_getters_setters_on_prototype) there is no getter, and a
//   forged {aborted: <boolean>} passes the check: utils.addAbortAlgorithm()
//   rejects it once a listener is to be added, but a null listener, or a
//   forgery claiming to have aborted, adds nothing before then. C++
//   rejects a forgery as the options are converted.

import { deepStrictEqual, strictEqual, throws } from 'node:assert';
import { NativeEvent } from 'helpers';
import { usingTsImpl, gettersSettersOnPrototype } from 'which-impl';

export const abortRemovesTheListener = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    target.addEventListener(
      'x',
      () => {
        calls++;
      },
      { signal: controller.signal }
    );
    target.dispatchEvent(new Event('x'));
    controller.abort();
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// An aborted signal adds nothing, and leaves the listener free to be added
// without one.
export const abortedSignalAddsNothing = {
  test() {
    const target = new EventTarget();
    let calls = 0;
    const listener = () => {
      calls++;
    };
    target.addEventListener('x', listener, { signal: AbortSignal.abort() });
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 0);
    target.addEventListener('x', listener);
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// The abort algorithm removes the registration it was added with, not a
// later one of the same listener.
export const abortRemovesOnlyItsOwnRegistration = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    const listener = () => {
      calls++;
    };
    target.addEventListener('x', listener, { signal: controller.signal });
    target.removeEventListener('x', listener);
    target.addEventListener('x', listener);
    controller.abort();
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// A duplicate is ignored, signal and all.
export const duplicateIgnoresItsSignal = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    const listener = () => {
      calls++;
    };
    target.addEventListener('x', listener);
    target.addEventListener('x', listener, { signal: controller.signal });
    controller.abort();
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

export const oneSignalForManyListeners = {
  test() {
    const controller = new AbortController();
    const targets = [new EventTarget(), new EventTarget()];
    const log = [];
    for (const [i, target] of targets.entries()) {
      for (const type of ['x', 'y']) {
        target.addEventListener(
          type,
          () => {
            log.push(`${i}${type}`);
          },
          { signal: controller.signal }
        );
      }
    }
    const dispatchAll = () => {
      for (const target of targets) {
        target.dispatchEvent(new Event('x'));
        target.dispatchEvent(new Event('y'));
      }
    };
    dispatchAll();
    controller.abort();
    dispatchAll();
    deepStrictEqual(log, ['0x', '0y', '1x', '1y']);
  },
};

// Aborting after a once listener has run, and removed itself, is harmless.
export const signalWithOnce = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    target.addEventListener(
      'x',
      () => {
        calls++;
      },
      { once: true, signal: controller.signal }
    );
    target.dispatchEvent(new Event('x'));
    controller.abort();
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
  },
};

// The listener is gone by the time the signal's 'abort' listeners run,
// even one added before it.
export const removedBeforeTheAbortEvent = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    const log = [];
    controller.signal.addEventListener('abort', () => {
      target.dispatchEvent(new Event('x'));
      log.push('abort');
    });
    target.addEventListener(
      'x',
      () => {
        log.push('x');
      },
      { signal: controller.signal }
    );
    controller.abort();
    deepStrictEqual(log, ['abort']);
  },
};

// Only aborting removes the listener; an 'abort' event dispatched at the
// signal does not. (A C++ event, which the signal requires: helpers.js.)
export const syntheticAbortEventRemovesNothing = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    let calls = 0;
    target.addEventListener(
      'x',
      () => {
        calls++;
      },
      { signal: controller.signal }
    );
    strictEqual(
      controller.signal.dispatchEvent(new NativeEvent('abort')),
      true
    );
    target.dispatchEvent(new Event('x'));
    strictEqual(calls, 1);
    strictEqual(controller.signal.aborted, false);
  },
};

// A listener whose signal an earlier listener aborts does not run.
export const abortDuringDispatch = {
  test() {
    const target = new EventTarget();
    const controller = new AbortController();
    const log = [];
    target.addEventListener('x', () => {
      log.push('first');
      controller.abort();
    });
    target.addEventListener(
      'x',
      () => {
        log.push('second');
      },
      { signal: controller.signal }
    );
    target.dispatchEvent(new Event('x'));
    deepStrictEqual(log, ['first']);
  },
};

// Anything but undefined must be an AbortSignal, whatever the listener.
export const signalMustBeAnAbortSignal = {
  test() {
    const listener = () => {};
    for (const signal of [
      null,
      {},
      1,
      'signal',
      true,
      new EventTarget(),
      Object.create(AbortSignal.prototype),
    ]) {
      for (const candidate of [listener, null]) {
        throws(
          () => new EventTarget().addEventListener('x', candidate, { signal }),
          TypeError,
          String(signal)
        );
      }
    }
  },
};

// #14.
export const forgedSignal = {
  test() {
    const listener = () => {};
    for (const aborted of [false, true]) {
      for (const candidate of [listener, null]) {
        const target = new EventTarget();
        const add = () =>
          target.addEventListener('x', candidate, { signal: { aborted } });
        const slipsThrough =
          usingTsImpl &&
          !gettersSettersOnPrototype &&
          (aborted || candidate === null);
        const label = `aborted: ${aborted}, listener: ${typeof candidate}`;
        if (slipsThrough) {
          add();
        } else {
          throws(add, TypeError, label);
        }
        // Nothing was added either way.
        strictEqual(target.dispatchEvent(new Event('x')), true, label);
      }
    }
  },
};
