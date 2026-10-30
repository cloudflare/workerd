// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Listener exceptions (DOM's inner invoke): each is reported, as HTML's
// "report an exception" does, and the dispatch carries on. Within a request
// the report fires the global 'error' event, then logs the exception unless
// a listener cancels the event. Nothing reaches the dispatchEvent() caller.
// (Without spec_compliant_dispatch_exceptions the C++ dispatch propagates
// the first exception instead; that side is guarded by
// src/workerd/api/tests/legacy-dispatch-exceptions-test.js.)
//
// Divergence (ledger in AGENTS.md):
// - #19: outside any request (in module evaluation), C++ only logs the
//   exception; TS fires the global 'error' event there too.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { collectReportedErrors } from 'helpers';
import { usingTsImpl } from 'which-impl';

// Recorded as this module is evaluated, outside any request (#19).
const thrownAtModuleEvaluation = new Error('module evaluation');
const atModuleEvaluation = (() => {
  const target = new EventTarget();
  const log = [];
  target.addEventListener('x', () => {
    log.push('first');
    throw thrownAtModuleEvaluation;
  });
  target.addEventListener('x', () => {
    log.push('second');
  });
  let result;
  const reported = collectReportedErrors(() => {
    result = target.dispatchEvent(new Event('x'));
  });
  return { log, result, reported };
})();

export const exceptionsAreReported = {
  test() {
    const target = new EventTarget();
    const thrown = new Error('listener');
    const log = [];
    target.addEventListener('x', () => {
      log.push('first');
      throw thrown;
    });
    target.addEventListener('x', () => {
      log.push('second');
    });
    let result;
    const reported = collectReportedErrors(() => {
      result = target.dispatchEvent(new Event('x', { cancelable: true }));
    });
    // An exception does not cancel the event.
    strictEqual(result, true);
    deepStrictEqual(log, ['first', 'second']);
    strictEqual(reported.length, 1);
    strictEqual(reported[0], thrown);
  },
};

// Each exception is reported, in order, whatever was thrown.
export const everyExceptionIsReported = {
  test() {
    const target = new EventTarget();
    const first = new Error('first');
    target.addEventListener('x', () => {
      throw first;
    });
    target.addEventListener('x', {
      handleEvent() {
        throw 42;
      },
    });
    target.addEventListener('x', () => {
      throw 'third';
    });
    const reported = collectReportedErrors(() => {
      target.dispatchEvent(new Event('x'));
    });
    strictEqual(reported.length, 3);
    strictEqual(reported[0], first);
    strictEqual(reported[1], 42);
    strictEqual(reported[2], 'third');
  },
};

// A throwing 'error' listener interrupts neither the report nor the
// dispatch that caused it.
export const throwingErrorListener = {
  test() {
    const target = new EventTarget();
    const log = [];
    target.addEventListener('x', () => {
      log.push('first');
      throw new Error('listener');
    });
    target.addEventListener('x', () => {
      log.push('second');
    });
    const onError = () => {
      log.push('error');
      throw new Error('error listener');
    };
    globalThis.addEventListener('error', onError);
    let result;
    try {
      result = target.dispatchEvent(new Event('x'));
    } finally {
      globalThis.removeEventListener('error', onError);
    }
    strictEqual(result, true);
    deepStrictEqual(log, ['first', 'error', 'second']);
  },
};

// The report runs synchronously, between the listeners.
export const reportRunsBetweenListeners = {
  test() {
    const target = new EventTarget();
    const log = [];
    const second = () => {
      log.push('second');
    };
    target.addEventListener('x', () => {
      log.push('first');
      throw new Error('first');
    });
    target.addEventListener('x', second);
    const onError = (e) => {
      log.push('error');
      target.removeEventListener('x', second);
      e.preventDefault();
    };
    globalThis.addEventListener('error', onError);
    try {
      target.dispatchEvent(new Event('x'));
    } finally {
      globalThis.removeEventListener('error', onError);
    }
    deepStrictEqual(log, ['first', 'error']);
  },
};

export const throwingOnceListenerIsRemoved = {
  test() {
    const target = new EventTarget();
    let calls = 0;
    target.addEventListener(
      'x',
      () => {
        calls++;
        throw new Error('once');
      },
      { once: true }
    );
    const reported = collectReportedErrors(() => {
      target.dispatchEvent(new Event('x'));
      target.dispatchEvent(new Event('x'));
    });
    strictEqual(calls, 1);
    strictEqual(reported.length, 1);
  },
};

// The dispatch ends as ever, so the event can be dispatched again.
export const stateAfterAnException = {
  test() {
    const target = new EventTarget();
    const event = new Event('x');
    let calls = 0;
    target.addEventListener('x', () => {
      calls++;
      throw new Error('listener');
    });
    let phase;
    const reported = collectReportedErrors(() => {
      target.dispatchEvent(event);
      phase = event.eventPhase;
      target.dispatchEvent(event);
    });
    strictEqual(phase, Event.NONE);
    strictEqual(calls, 2);
    strictEqual(reported.length, 2);
  },
};

// The report goes through the runtime's own reportError(), not whatever the
// global reportError property holds.
export const replacedReportErrorIsNotCalled = {
  test() {
    const { reportError } = globalThis;
    let calls = 0;
    globalThis.reportError = () => {
      calls++;
    };
    const thrown = new Error('listener');
    let reported;
    try {
      const target = new EventTarget();
      target.addEventListener('x', () => {
        throw thrown;
      });
      reported = collectReportedErrors(() => {
        target.dispatchEvent(new Event('x'));
      });
    } finally {
      globalThis.reportError = reportError;
    }
    strictEqual(calls, 0);
    strictEqual(reported.length, 1);
    strictEqual(reported[0], thrown);
  },
};

// An exception that cannot be logged as usual (its stack getter throws) is
// still reported, and the dispatch still carries on. Nothing cancels the
// 'error' event, so the exception is logged.
export const unloggableException = {
  test() {
    const target = new EventTarget();
    const thrown = new Error('stack');
    Object.defineProperty(thrown, 'stack', {
      get() {
        throw new Error('stack getter');
      },
    });
    const log = [];
    target.addEventListener('x', () => {
      throw thrown;
    });
    target.addEventListener('x', () => {
      log.push('second');
    });
    const onError = (e) => {
      log.push(e.error === thrown);
    };
    globalThis.addEventListener('error', onError);
    let result;
    try {
      result = target.dispatchEvent(new Event('x'));
    } finally {
      globalThis.removeEventListener('error', onError);
    }
    strictEqual(result, true);
    deepStrictEqual(log, [true, 'second']);
  },
};

// #19.
export const exceptionsOutsideARequest = {
  test() {
    const { log, result, reported } = atModuleEvaluation;
    deepStrictEqual(log, ['first', 'second']);
    strictEqual(result, true);
    if (usingTsImpl) {
      strictEqual(reported.length, 1);
      strictEqual(reported[0], thrownAtModuleEvaluation);
    } else {
      strictEqual(reported.length, 0);
    }
  },
};
