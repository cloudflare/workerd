// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  context,
  trace,
  propagation,
  ROOT_CONTEXT,
  createContextKey,
  SpanStatusCode,
  isSpanContextValid,
  createTraceState,
} from '@opentelemetry/api';
import { tracing } from 'cloudflare:workers';

// This is the actual 1.10.0 API, not direct calls into its shared-symbol registry.
const tracer = trace.getTracer('real-api', '1.10.0');
const custom = createContextKey('real-api-value');

export const realPackage = {
  async test() {
    assert.ok(isSpanContextValid(trace.getActiveSpan().spanContext()));
    assert.deepStrictEqual(
      trace.getActiveSpan().spanContext(),
      tracing.getActiveSpan().spanContext()
    );
    assert.strictEqual(
      trace.setGlobalTracerProvider({
        getTracer() {
          throw new Error('must not replace');
        },
      }),
      false
    );
    const explicit = propagation.setBaggage(
      context.active().setValue(custom, 42),
      propagation.createBaggage({ tenant: { value: 'real' } })
    );
    await context.with(explicit, async () => {
      await tracer.startActiveSpan(
        'real-api-parent',
        {},
        undefined,
        async (span) => {
          assert.strictEqual(trace.getActiveSpan(), span);
          assert.strictEqual(context.active().getValue(custom), 42);
          assert.strictEqual(
            propagation.getBaggage(context.active()).getEntry('tenant').value,
            'real'
          );
          await Promise.resolve();
          assert.strictEqual(trace.getActiveSpan(), span);
          span.setStatus({ code: SpanStatusCode.OK });
          const child = tracer.startSpan('real-api-child');
          assert.strictEqual(
            child.spanContext().traceId,
            span.spanContext().traceId
          );
          child.end();
          span.end();
        }
      );
    });
    for (const parent of [ROOT_CONTEXT, trace.deleteSpan(context.active())]) {
      const span = tracer.startSpan('real-api-root-fallback', {}, parent);
      assert.strictEqual(
        span.spanContext().traceId,
        tracing.getActiveSpan().spanContext().traceId
      );
      span.end();
    }
    const remote = trace.setSpanContext(ROOT_CONTEXT, {
      traceId: '11111111111111111111111111111111',
      spanId: '2222222222222222',
      traceFlags: 0,
      isRemote: true,
      traceState: createTraceState('vendor=real'),
    });
    tracer.startActiveSpan('real-api-unsampled', {}, remote, (span) => {
      assert.strictEqual(span.isRecording(), false);
      const output = {};
      propagation.inject(context.active(), output);
      assert.strictEqual(output.tracestate, 'vendor=real');
      assert.ok(output.traceparent.endsWith('-00'));
      span.end();
    });
  },
};
