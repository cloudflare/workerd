// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Scenarios for tracing.withActiveSpanContext(). Each test asserts what is visible from
// JavaScript; the tail worker in tracing-active-span-context-instrumentation-test.js then
// validates the traces and parents the runtime reported in the streaming-tail events.

import assert from 'node:assert';
import { DurableObject, tracing } from 'cloudflare:workers';

// Keep in sync with tracing-active-span-context-instrumentation-test.js.
const REMOTE_CONTEXT = {
  traceId: '4bf92f3577b34da6a3ce929d0e0e4736',
  spanId: '00f067aa0ba902b7',
  traceFlags: 1,
};
const SAME_TRACE_SPAN_ID = '1234567890abcdef';
const ZERO_CONTEXT = {
  traceId: '0'.repeat(32),
  spanId: '0'.repeat(16),
  traceFlags: 0,
};

// Outside an invocation, withActiveSpanContext() only changes what getActiveSpan() returns.
assert.deepStrictEqual(
  tracing.withActiveSpanContext(REMOTE_CONTEXT, () =>
    tracing.getActiveSpan().spanContext()
  ),
  REMOTE_CONTEXT
);
assert.strictEqual(
  tracing.withActiveSpanContext(undefined, () => tracing.getActiveSpan()),
  undefined
);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export const remoteContextIsActive = {
  async test() {
    const invocationSpan = tracing.getActiveSpan();
    const result = await tracing.withActiveSpanContext(
      REMOTE_CONTEXT,
      async (a, b) => {
        const remote = tracing.getActiveSpan();
        assert.strictEqual(tracing.getActiveSpan(), remote);
        assert.deepStrictEqual(remote.spanContext(), REMOTE_CONTEXT);
        // Like OpenTelemetry's NonRecordingSpan, the remote span records nothing.
        assert.strictEqual(remote.isTraced, false);
        assert.strictEqual(remote.setAttribute('ignored', true), remote);
        remote.end();
        await sleep(1);
        assert.strictEqual(tracing.getActiveSpan(), remote);
        return a + b;
      },
      1,
      2
    );
    assert.strictEqual(result, 3);
    assert.strictEqual(tracing.getActiveSpan(), invocationSpan);
  },
};

export const remoteContextParentsNewSpans = {
  async test(ctrl, env, ctx) {
    await tracing.withActiveSpanContext(REMOTE_CONTEXT, async () => {
      const child = tracing.startSpan('wasc-remote-start-span');
      assert.strictEqual(child.isTraced, true);
      assert.strictEqual(child.spanContext().traceId, REMOTE_CONTEXT.traceId);
      child.end();
      await ctx.tracing.enterSpan('wasc-remote-enter-span', async () => {
        console.log('wasc-remote-log');
        tracing.startSpan('wasc-remote-grandchild').end();
        const response = await env.fetchTarget.fetch('http://mock/wasc-remote');
        await response.text();
      });
    });
  },
};

export const remoteContextLogsAndAwait = {
  async test() {
    await tracing.withActiveSpanContext(REMOTE_CONTEXT, async () => {
      console.log('wasc-remote-direct-log');
      await sleep(1);
      tracing.startSpan('wasc-remote-after-await').end();
    });
  },
};

export const unsampledRemoteContextIsRecorded = {
  test() {
    tracing.withActiveSpanContext({ ...REMOTE_CONTEXT, traceFlags: 0 }, () => {
      const span = tracing.startSpan('wasc-unsampled-child');
      assert.strictEqual(span.isTraced, true);
      assert.strictEqual(span.spanContext().traceFlags, 0);
      span.end();
    });
  },
};

export const sameTraceContext = {
  test() {
    // A span context in the invocation's own trace, such as one for a span made by an OpenTelemetry
    // SDK running in the Worker, parents new spans in the invocation's trace.
    const { traceId } = tracing.getActiveSpan().spanContext();
    tracing.withActiveSpanContext(
      { traceId, spanId: SAME_TRACE_SPAN_ID, traceFlags: 1 },
      () => {
        tracing.startSpan('wasc-same-trace-child').end();
      }
    );
  },
};

export const localSpanInsideRemoteContext = {
  test() {
    const local = tracing.startSpan('wasc-local-parent');
    tracing.withActiveSpanContext(REMOTE_CONTEXT, () => {
      const remote = tracing.getActiveSpan();
      // An explicitly activated local span takes precedence over the remote context.
      tracing.withActiveSpan(local, () => {
        tracing.startSpan('wasc-local-child').end();
        // Activating the remote span again parents new spans under it.
        tracing.withActiveSpan(remote, () => {
          assert.strictEqual(tracing.getActiveSpan(), remote);
          tracing.startSpan('wasc-reactivated-remote-child').end();
        });
      });
    });
    local.end();
  },
};

export const undefinedContextStartsNewTraces = {
  async test(ctrl, env, ctx) {
    const invocationTraceId = tracing.getActiveSpan().spanContext().traceId;
    await ctx.tracing.enterSpan('wasc-root-enclosing', async () => {
      await tracing.withActiveSpanContext(undefined, async () => {
        assert.strictEqual(tracing.getActiveSpan(), undefined);
        let firstTraceId;
        await ctx.tracing.enterSpan('wasc-root-a', async (a) => {
          firstTraceId = a.spanContext().traceId;
          assert.notStrictEqual(firstTraceId, invocationTraceId);
          // As with OpenTelemetry's default sampler, a new trace is sampled.
          assert.strictEqual(a.spanContext().traceFlags, 1);
          console.log('wasc-root-log');
          tracing.startSpan('wasc-root-a-child').end();
          const response = await env.fetchTarget.fetch(
            'http://mock/wasc-root-a'
          );
          await response.text();
        });
        const second = tracing.startSpan('wasc-root-b');
        assert.notStrictEqual(second.spanContext().traceId, firstTraceId);
        assert.notStrictEqual(second.spanContext().traceId, invocationTraceId);
        assert.strictEqual(second.spanContext().traceFlags, 1);
        second.end();
        // Built-in instrumentation with no span around it starts its own trace too.
        const response = await env.fetchTarget.fetch(
          'http://mock/wasc-root-fetch'
        );
        await response.text();
      });
      tracing.startSpan('wasc-root-after').end();
    });
  },
};

export const invalidContextStartsNewTraces = {
  test(ctrl, env, ctx) {
    const invalidContexts = [
      { ...REMOTE_CONTEXT, traceId: ZERO_CONTEXT.traceId },
      { ...REMOTE_CONTEXT, spanId: ZERO_CONTEXT.spanId },
      { ...REMOTE_CONTEXT, traceId: 'not-hex' },
      { ...REMOTE_CONTEXT, spanId: REMOTE_CONTEXT.spanId + '0' },
    ];
    for (const context of invalidContexts) {
      tracing.withActiveSpanContext(context, () => {
        // As in OpenTelemetry, the active span is a non-recording span with an invalid context.
        const span = tracing.getActiveSpan();
        assert.deepStrictEqual(span.spanContext(), ZERO_CONTEXT);
        assert.strictEqual(span.isTraced, false);
      });
    }
    ctx.tracing.enterSpan('wasc-invalid-enclosing', () => {
      tracing.withActiveSpanContext(invalidContexts[0], () => {
        const invalidSpan = tracing.getActiveSpan();
        ctx.tracing.enterSpan('wasc-invalid-root', () => {
          // Activating the invalid-context span again starts new traces too.
          tracing.withActiveSpan(invalidSpan, () => {
            tracing.startSpan('wasc-invalid-reactivated-root').end();
          });
        });
      });
    });
  },
};

export const lenientSpanContextInput = {
  test() {
    // As in OpenTelemetry, a missing or out-of-range traceFlags counts as 0.
    for (const traceFlags of [undefined, 256, -1]) {
      tracing.withActiveSpanContext({ ...REMOTE_CONTEXT, traceFlags }, () => {
        assert.deepStrictEqual(tracing.getActiveSpan().spanContext(), {
          ...REMOTE_CONTEXT,
          traceFlags: 0,
        });
      });
    }
    // Missing IDs, or null, make the context invalid.
    for (const context of [{ traceId: REMOTE_CONTEXT.traceId }, null]) {
      tracing.withActiveSpanContext(context, () => {
        assert.deepStrictEqual(
          tracing.getActiveSpan().spanContext(),
          ZERO_CONTEXT
        );
      });
    }
  },
};

export const rejectsBadArguments = {
  test() {
    assert.throws(
      () => tracing.withActiveSpanContext('not an object', () => {}),
      TypeError
    );
    assert.throws(
      () => tracing.withActiveSpanContext(REMOTE_CONTEXT, 'not a function'),
      TypeError
    );
    assert.throws(
      () => tracing.withActiveSpanContext(REMOTE_CONTEXT),
      TypeError
    );
  },
};

// Requests to a Durable Object share an IoContext. The first request resumes while the second is
// still running, so the second is the IoContext's current request, but spans created by the first
// must still be reported in the first request's invocation.
export class OverlappingRequests extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.released = new Promise((resolve) => (this.release = resolve));
    this.finished = new Promise((resolve) => (this.finish = resolve));
  }

  async fetch(request) {
    if (new URL(request.url).pathname === '/first') {
      tracing.startSpan('wasc-do-first-marker').end();
      await this.released;
      await tracing.withActiveSpanContext(REMOTE_CONTEXT, async () => {
        tracing.startSpan('wasc-do-remote-child').end();
      });
      tracing.withActiveSpanContext(undefined, () => {
        tracing.startSpan('wasc-do-root').end();
      });
      this.finish();
      return new Response('first');
    }

    tracing.startSpan('wasc-do-second-marker').end();
    this.release();
    await this.finished;
    return new Response('second');
  }
}

export const durableObjectOverlappingRequests = {
  async test(ctrl, env) {
    const stub = env.overlapping.get(env.overlapping.idFromName('overlap'));
    const first = stub.fetch('https://example.com/first');
    // Let the first request start running before the second is delivered. Otherwise both are
    // delivered first, and the second is already the current request when the first starts.
    await sleep(10);
    const second = await stub.fetch('https://example.com/second');
    assert.strictEqual(await second.text(), 'second');
    assert.strictEqual(await (await first).text(), 'first');
  },
};
