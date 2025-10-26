// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export const syncFunction = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Synchronous function - span should end immediately
    const result = withSpan('sync-op', (span) => {
      span.setAttribute('type', 'sync');
      span.setAttribute('test', 'syncFunction');
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
      span.setAttribute('type', 'async');
      span.setAttribute('test', 'asyncFunction');
      await new Promise((resolve) => setTimeout(resolve, 10));
      return 'async-value';
    });

    assert.strictEqual(result, 'async-value');
  },
};

export const syncError = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    // Test: Synchronous error - span should end before throwing
    let errorCaught = false;
    try {
      withSpan('sync-error-op', (span) => {
        span.setAttribute('type', 'sync-error');
        span.setAttribute('test', 'syncError');
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
        span.setAttribute('type', 'async-error');
        span.setAttribute('test', 'asyncError');
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
