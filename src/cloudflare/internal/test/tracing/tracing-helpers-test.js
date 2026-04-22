// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { tracing as publicTracing } from 'cloudflare:workers';

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

// Verify the JS-visible class name is exactly "Span" - no internal-implementation names
// should leak into JavaScript.
export const spanClassName = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    withSpan('class-name-op', (span) => {
      span.setAttribute('test', 'spanClassName');
      assert.strictEqual(span.constructor.name, 'Span');
    });
  },
};

// Verify isTraced reflects the state of the span: true while the span is live, false
// after it has been auto-ended by withSpan.
export const isTraced = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    let capturedSpan = null;
    withSpan('is-traced-op', (span) => {
      capturedSpan = span;
      // While inside the callback, the span should be traced (we're inside a tailed
      // request - the streamingTails binding is configured for this worker).
      assert.strictEqual(
        span.isTraced,
        true,
        'Span should be traced inside withSpan callback'
      );
      span.setAttribute('test', 'isTraced');
    });

    // After withSpan returns, the span has been auto-ended -> isTraced should be false.
    assert.strictEqual(
      capturedSpan.isTraced,
      false,
      'Span should no longer be traced after auto-end'
    );
  },
};

// Verify that setAttribute with undefined is a no-op (the attribute is simply not set),
// which is the idiomatic pattern for optional attributes.
export const setAttributeUndefined = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    const result = withSpan('undefined-attr-op', (span) => {
      span.setAttribute('test', 'setAttributeUndefined');
      // Passing undefined should not throw and should not record the attribute.
      span.setAttribute('skipped', undefined);
      return 'undefined-attr-value';
    });

    assert.strictEqual(result, 'undefined-attr-value');
  },
};

// Verify that nested withSpan calls produce correctly nested spans. This exercises the
// AsyncContextFrame push path in enterSpan: the inner span should be parented on the
// outer span.
export const nestedSyncSpans = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    const result = withSpan('nested-outer-op', (outerSpan) => {
      outerSpan.setAttribute('test', 'nestedSyncSpans');
      outerSpan.setAttribute('level', 'outer');
      return withSpan('nested-inner-op', (innerSpan) => {
        innerSpan.setAttribute('test', 'nestedSyncSpans');
        innerSpan.setAttribute('level', 'inner');
        return 'nested-value';
      });
    });

    assert.strictEqual(result, 'nested-value');
  },
};

// Async analog of the nested test: inner span lives inside an await that spans a
// microtask boundary, so the inner span's parent is preserved only if the
// AsyncContextFrame push correctly follows the async continuation.
export const nestedAsyncSpans = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    const result = await withSpan(
      'nested-async-outer-op',
      async (outerSpan) => {
        outerSpan.setAttribute('test', 'nestedAsyncSpans');
        outerSpan.setAttribute('level', 'outer');
        // Crossing a microtask boundary before creating the inner span.
        await new Promise((resolve) => setTimeout(resolve, 5));
        return await withSpan('nested-async-inner-op', async (innerSpan) => {
          innerSpan.setAttribute('test', 'nestedAsyncSpans');
          innerSpan.setAttribute('level', 'inner');
          await new Promise((resolve) => setTimeout(resolve, 5));
          return 'nested-async-value';
        });
      }
    );

    assert.strictEqual(result, 'nested-async-value');
  },
};

// Verify the public import path: `import { tracing } from 'cloudflare:workers'`.
// Hitting enterSpan via the public import should behave identically to the internal path.
export const publicImportTracing = {
  async test(ctrl, env, ctx) {
    const result = publicTracing.enterSpan('public-import-op', (span) => {
      span.setAttribute('test', 'publicImportTracing');
      span.setAttribute('path', 'import-from-cloudflare-workers');
      assert.strictEqual(span.isTraced, true);
      return 'public-import-value';
    });
    assert.strictEqual(result, 'public-import-value');
  },
};

// Verify ctx.tracing: same Tracing instance should be reachable off the execution context.
export const ctxTracing = {
  async test(ctrl, env, ctx) {
    assert.ok(ctx.tracing, 'ctx.tracing should be defined');
    assert.strictEqual(
      typeof ctx.tracing.enterSpan,
      'function',
      'ctx.tracing.enterSpan should be a function'
    );

    const result = ctx.tracing.enterSpan('ctx-tracing-op', (span) => {
      span.setAttribute('test', 'ctxTracing');
      span.setAttribute('path', 'ctx.tracing');
      assert.strictEqual(span.isTraced, true);
      return 'ctx-tracing-value';
    });
    assert.strictEqual(result, 'ctx-tracing-value');
  },
};
