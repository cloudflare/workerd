// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import {
  default as api,
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

// Each worker imports an independent real 1.9.0 or 1.10.0 API copy.
const tracer = trace.getTracer('real-api', '1.10.0');
const custom = createContextKey('real-api-value');

export const realPackage = {
  async test() {
    assert.strictEqual(
      globalThis[Symbol.for('opentelemetry.js.api.1')].version,
      '1.10.0'
    );
    assert.strictEqual(api.trace, trace);
    assert.strictEqual(api.context, context);
    assert.strictEqual(api.propagation, propagation);
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
    for (const parent of [
      ROOT_CONTEXT,
      trace.deleteSpan(context.active()),
      trace.setSpan(context.active(), null),
    ]) {
      const span = tracer.startSpan('real-api-root', {}, parent);
      assert.strictEqual(span.isRecording(), true);
      assert.notStrictEqual(
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
