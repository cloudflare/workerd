// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export const syncFunction = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Synchronous function - span should end immediately
    const result = withSpan('sync-op', (span) => {
      span.setTag('type', 'sync');
      span.setTag('test', 'syncFunction');
      return 'sync-value';
    });

    assert.strictEqual(result, 'sync-value');
  },
};

export const asyncFunction = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Async function returning Promise - span should end after promise resolves
    const result = await withSpan('async-op', async (span) => {
      span.setTag('type', 'async');
      span.setTag('test', 'asyncFunction');
      await new Promise((resolve) => setTimeout(resolve, 10));
      return 'async-value';
    });

    assert.strictEqual(result, 'async-value');
  },
};

export const thenableObject = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Thenable object (not a real Promise) - should be wrapped and span ended
    const thenable = {
      value: 'thenable-value',
      then(onResolve) {
        setTimeout(() => onResolve(this.value), 10);
      },
    };

    const result = await withSpan('thenable-op', (span) => {
      span.setTag('type', 'thenable');
      span.setTag('test', 'thenableObject');
      return thenable;
    });

    assert.strictEqual(result, 'thenable-value');
  },
};

export const syncError = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Synchronous error - span should end before throwing
    let errorCaught = false;
    try {
      withSpan('sync-error-op', (span) => {
        span.setTag('type', 'sync-error');
        span.setTag('test', 'syncError');
        throw new Error('sync error');
      });
    } catch (e) {
      errorCaught = true;
      assert.strictEqual(e.message, 'sync error');
    }
    assert(errorCaught, 'Sync error should have been caught');
  },
};

export const asyncError = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Async error (rejected promise) - span should end before rejection
    let errorCaught = false;
    try {
      await withSpan('async-error-op', async (span) => {
        span.setTag('type', 'async-error');
        span.setTag('test', 'asyncError');
        await new Promise((resolve) => setTimeout(resolve, 10));
        throw new Error('async error');
      });
    } catch (e) {
      errorCaught = true;
      assert.strictEqual(e.message, 'async error');
    }
    assert(errorCaught, 'Async error should have been caught');
  },
};

export const notThenableObject = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Object with 'then' property that is NOT a function - should be treated as sync
    const notThenable = {
      value: 'not-thenable',
      then: 'not-a-function', // then exists but is not a function
    };

    const result = withSpan('not-thenable-op', (span) => {
      span.setTag('type', 'not-thenable');
      span.setTag('test', 'notThenableObject');
      return notThenable;
    });

    assert.strictEqual(result, notThenable);
  },
};

export const nullAndUndefinedReturns = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Null and undefined returns - should be treated as sync
    const nullResult = withSpan('null-op', (span) => {
      span.setTag('type', 'null');
      return null;
    });
    assert.strictEqual(nullResult, null);

    const undefinedResult = withSpan('undefined-op', (span) => {
      span.setTag('type', 'undefined');
      return undefined;
    });
    assert.strictEqual(undefinedResult, undefined);
  },
};

export const generatorFunction = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Generator function (documenting current behavior)
    function* generatorFn(span) {
      span.setTag('type', 'generator');
      span.setTag('test', 'generatorFunction');
      yield 1;
      yield 2;
      yield 3;
    }

    const generator = withSpan('generator-op', generatorFn);
    // Generator object is returned immediately, span ends right away
    // Verify generator still works
    const genValues = [];
    for (const value of generator) {
      genValues.push(value);
    }
    assert.deepStrictEqual(genValues, [1, 2, 3]);
    // Note: span already ended before generator was consumed
  },
};

export const asyncGeneratorFunction = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Async generator function (documenting current behavior)
    async function* asyncGeneratorFn(span) {
      span.setTag('type', 'async-generator');
      span.setTag('test', 'asyncGeneratorFunction');
      yield 1;
      await new Promise((resolve) => setTimeout(resolve, 10));
      yield 2;
    }

    const asyncGen = withSpan('async-generator-op', asyncGeneratorFn);
    // Async generator object is returned immediately, span ends right away
    // Verify async generator still works
    const asyncGenValues = [];
    for await (const value of asyncGen) {
      asyncGenValues.push(value);
    }
    assert.deepStrictEqual(asyncGenValues, [1, 2]);
    // Note: span already ended before async generator was consumed
  },
};

export const callableThenable = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Function that is also thenable (edge case)
    const callableThenable = Object.assign(
      function () {
        return 'called';
      },
      {
        then(onResolve) {
          setTimeout(() => onResolve('resolved'), 10);
        },
      }
    );

    const result = await withSpan('callable-thenable-op', (span) => {
      span.setTag('type', 'callable-thenable');
      span.setTag('test', 'callableThenable');
      return callableThenable;
    });

    assert.strictEqual(result, 'resolved');
  },
};

export const multipleEndCalls = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Multiple spans ending idempotently (span.end() called multiple times is safe)
    const result = withSpan('multi-end-op', (span) => {
      span.setTag('type', 'multi-end');
      span.setTag('test', 'multipleEndCalls');
      span.end(); // Manual end
      span.end(); // Should be idempotent
      return 'multi-end-value';
    });

    assert.strictEqual(result, 'multi-end-value');
    // Should only see one span-ended event due to idempotency
  },
};

export const nestedSpans = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Nested spans
    const result = await withSpan('outer-op', async (outerSpan) => {
      outerSpan.setTag('type', 'outer');

      const innerResult = await withSpan('inner-op', async (innerSpan) => {
        innerSpan.setTag('type', 'inner');
        await new Promise((resolve) => setTimeout(resolve, 10));
        return 'inner-value';
      });

      return `outer-${innerResult}`;
    });

    assert.strictEqual(result, 'outer-inner-value');
  },
};
