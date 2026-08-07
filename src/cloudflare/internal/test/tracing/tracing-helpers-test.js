// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { AsyncLocalStorage } from 'node:async_hooks';
import { DurableObject, tracing as publicTracing } from 'cloudflare:workers';

assert.strictEqual(publicTracing.getActiveSpan(), undefined);
assert.strictEqual(publicTracing.getInvocationSpan(), undefined);
const getSpansOutsideInvocationContext = AsyncLocalStorage.bind(() => ({
  active: [publicTracing.getActiveSpan(), publicTracing.getActiveSpan()],
  invocation: [
    publicTracing.getInvocationSpan(),
    publicTracing.getInvocationSpan(),
  ],
}));

// Overlapping Durable Object requests share an IoContext, but each async continuation must retain
// its originating request's tracing state. This verifies that request A resuming while request B is
// current cannot write A's invocation-span attributes into B's tail trace.
export class OverlappingRequestsObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.firstCanResume = new Promise((resolve) => {
      this.resumeFirst = resolve;
    });
    this.firstAttributed = new Promise((resolve) => {
      this.resolveFirstAttributed = resolve;
    });
  }

  async fetch(request) {
    const requestName = new URL(request.url).pathname.slice(1);

    if (requestName === 'a') {
      this.firstIsWaiting = true;
      return new Response(
        new ReadableStream({
          pull: async (controller) => {
            controller.enqueue(new TextEncoder().encode('ready'));
            await this.firstCanResume;
            const span = publicTracing.getActiveSpan();
            assert(span);
            assert.strictEqual(span.isTraced, true);
            span.setAttribute('overlapping.request', 'a');
            this.resolveFirstAttributed();
            controller.close();
          },
        })
      );
    }

    assert.strictEqual(this.firstIsWaiting, true);
    const span = publicTracing.getActiveSpan();
    assert(span);
    assert.strictEqual(span.isTraced, true);
    span.setAttribute('overlapping.request', 'b');
    this.resumeFirst();
    await this.firstAttributed;
    return new Response('b');
  }
}

export const overlappingDurableObjectRequests = {
  async test(ctrl, env) {
    const id = env.overlappingRequests.idFromName('test');
    const stub = env.overlappingRequests.get(id);
    const first = await stub.fetch('https://example.com/a');
    const firstReader = first.body.getReader();
    const ready = await firstReader.read();
    assert.strictEqual(ready.done, false);
    assert.strictEqual(new TextDecoder().decode(ready.value), 'ready');
    const second = await stub.fetch('https://example.com/b');
    const [firstEnd] = await Promise.all([firstReader.read(), second.text()]);
    assert.strictEqual(firstEnd.done, true);
    assert.deepStrictEqual([first.status, second.status], [200, 200]);
  },
};

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

// Verify the attribute setters return the span for chaining and setAttributes handles all
// currently-supported value types while ignoring undefined values.
export const setAttributes = {
  async test(ctrl, env, ctx) {
    const { withSpan } = env.tracingTest;

    withSpan('set-attributes-op', (span) => {
      assert.strictEqual(span.setAttribute('test', 'setAttributes'), span);
      assert.strictEqual(
        span.setAttributes({
          stringValue: 'value',
          numberValue: 42,
          booleanValue: true,
          skipped: undefined,
        }),
        span
      );
    });
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

export const publicImportStartActiveSpan = {
  async test(ctrl, env, ctx) {
    let capturedSpan = null;
    const result = publicTracing.startActiveSpan(
      'public-start-active-op',
      (span) => {
        capturedSpan = span;
        span.setAttribute('test', 'publicImportStartActiveSpan');
        span.setAttribute('path', 'import-from-cloudflare-workers');
        assert.strictEqual(span.isTraced, true);
        return 'public-start-active-value';
      }
    );

    assert.strictEqual(result, 'public-start-active-value');
    assert.strictEqual(capturedSpan.isTraced, true);
    capturedSpan.setAttribute('ended.explicitly', true);
    capturedSpan.end();
    assert.strictEqual(capturedSpan.isTraced, false);
  },
};

export const publicImportStartSpan = {
  async test(ctrl, env, ctx) {
    const span = publicTracing.startSpan('public-start-span-op');
    span.setAttribute('test', 'publicImportStartSpan');
    span.setAttribute('path', 'import-from-cloudflare-workers');
    assert.strictEqual(span.isTraced, true);
    span.end();
    assert.strictEqual(span.isTraced, false);
  },
};

export const activeAndInvocationSpans = {
  async test(ctrl, env, ctx) {
    const invocationSpan = publicTracing.getActiveSpan();
    assert.ok(invocationSpan);
    // All the ways to get the active span should return the same reference
    assert.strictEqual(publicTracing.getActiveSpan(), invocationSpan);
    assert.strictEqual(ctx.tracing.getActiveSpan(), invocationSpan);
    assert.strictEqual(publicTracing.getInvocationSpan(), invocationSpan);
    assert.strictEqual(ctx.tracing.getInvocationSpan(), invocationSpan);
    const detachedSpans = getSpansOutsideInvocationContext();
    assert.deepStrictEqual(detachedSpans.active, [undefined, undefined]);
    assert.strictEqual(detachedSpans.invocation[0], invocationSpan);
    assert.strictEqual(detachedSpans.invocation[1], invocationSpan);
    assert.strictEqual(invocationSpan.isTraced, true);
    // This is ignored since we control the lifecycle
    invocationSpan.end();
    assert.strictEqual(invocationSpan.isTraced, true);
    invocationSpan.setAttribute('test', 'getActiveSpanInvocation');

    await ctx.tracing.startActiveSpan('get-active-span-op', async (span) => {
      assert.strictEqual(publicTracing.getActiveSpan(), span);
      assert.strictEqual(publicTracing.getInvocationSpan(), invocationSpan);
      await Promise.resolve();
      assert.strictEqual(publicTracing.getActiveSpan(), span);
      assert.strictEqual(publicTracing.getInvocationSpan(), invocationSpan);
      publicTracing.getInvocationSpan().setAttribute('user.id', 'user-123');
      span.setAttribute('test', 'getActiveSpan');
      span.end();
    });

    assert.strictEqual(publicTracing.getActiveSpan(), invocationSpan);
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

export const detachedSpanEndsAfterStreamDrain = {
  async test(ctrl, env, ctx) {
    assert.ok(ctx.tracing, 'ctx.tracing should be defined');
    assert.strictEqual(
      typeof ctx.tracing.startActiveSpan,
      'function',
      'ctx.tracing.startActiveSpan should be a function'
    );

    let capturedSpan = null;
    const stream = ctx.tracing.startActiveSpan('detached-stream-op', (span) => {
      capturedSpan = span;
      span.setAttribute('test', 'detachedSpanEndsAfterStreamDrain');
      span.setAttribute('phase.created', true);

      return new ReadableStream({
        start(controller) {
          controller.enqueue(new TextEncoder().encode('hello'));
          controller.enqueue(new TextEncoder().encode(' world'));
          controller.close();
        },
      }).pipeThrough(
        new TransformStream({
          transform(chunk, controller) {
            controller.enqueue(chunk);
          },
          flush() {
            span.setAttribute('phase.drained', true);
            span.end();
          },
        })
      );
    });

    assert.strictEqual(
      capturedSpan.isTraced,
      true,
      'Detached span should stay open after callback returns'
    );
    assert.strictEqual(await new Response(stream).text(), 'hello world');
    assert.strictEqual(
      capturedSpan.isTraced,
      false,
      'Detached span should stop tracing after explicit end()'
    );
  },
};

export const helperStartActiveSpan = {
  async test(ctrl, env, ctx) {
    const { startActiveSpan } = env.tracingTest;
    assert.strictEqual(
      typeof startActiveSpan,
      'function',
      'tracing helpers should export startActiveSpan'
    );

    let capturedSpan = null;
    const result = startActiveSpan('helper-detached-op', (span) => {
      capturedSpan = span;
      span.setAttribute('test', 'helperStartActiveSpan');
      return 'helper-detached-value';
    });

    assert.strictEqual(result, 'helper-detached-value');
    assert.strictEqual(
      capturedSpan.isTraced,
      true,
      'Helper-created span should stay open after callback returns'
    );
    capturedSpan.setAttribute('ended.explicitly', true);
    capturedSpan.end();
    assert.strictEqual(
      capturedSpan.isTraced,
      false,
      'Helper-created span should stop tracing after explicit end()'
    );
  },
};

export const startActiveSpanSyncThrow = {
  async test(ctrl, env, ctx) {
    let capturedSpan = null;
    let caught = false;

    try {
      ctx.tracing.startActiveSpan('manual-throw-op', (span) => {
        capturedSpan = span;
        span.setAttribute('test', 'startActiveSpanSyncThrow');
        throw new Error('manual lifecycle throw');
      });
    } catch (e) {
      caught = true;
      assert.strictEqual(e.message, 'manual lifecycle throw');
    }

    assert(caught, 'startActiveSpan callback error should be rethrown');
    assert.strictEqual(
      capturedSpan.isTraced,
      true,
      'Manual span should stay open after callback throws'
    );
    capturedSpan.setAttribute('after.throw', true);
    capturedSpan.end();
    assert.strictEqual(
      capturedSpan.isTraced,
      false,
      'Manual span should stop tracing after explicit end()'
    );
  },
};
