// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { tracing } from 'cloudflare:workers';

const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
const spanKey = Symbol.for('OpenTelemetry Context Key SPAN');
const root = api.context.active();
const tracer = api.trace.getTracer('untraced');

export const propagationWithoutTail = {
  async test() {
    for (const flags of ['00', '01', '03']) {
      const extracted = api.propagation.extract(
        root,
        {
          traceparent: `00-11111111111111111111111111111111-2222222222222222-${flags}`,
          tracestate: 'vendor=value',
        },
        { get: (c, k) => c[k] }
      );
      await tracer.startActiveSpan('untraced', {}, extracted, async (span) => {
        assert.strictEqual(span.isRecording(), false);
        const identity = span.spanContext();
        assert.strictEqual(
          identity.traceId,
          '11111111111111111111111111111111'
        );
        assert.notStrictEqual(identity.spanId, '2222222222222222');
        assert.notStrictEqual(identity.spanId, '0000000000000000');
        assert.strictEqual(identity.traceFlags, parseInt(flags, 16) & 1);
        await Promise.resolve();
        const carrier = {};
        api.propagation.inject(api.context.active(), carrier, {
          set(c, k, v) {
            c[k] = v;
          },
        });
        assert.strictEqual(
          carrier.traceparent,
          `00-${identity.traceId}-${identity.spanId}-0${identity.traceFlags}`
        );
        assert.strictEqual(carrier.tracestate, 'vendor=value');
        const child = tracer.startSpan('child');
        assert.strictEqual(child.isRecording(), false);
        assert.notStrictEqual(child.spanContext().spanId, identity.spanId);
        assert.strictEqual(child.spanContext().traceId, identity.traceId);
        child.end();
        tracing.enterSpan('native-noop', () => {
          const nativeChild = tracer.startSpan('native-child');
          assert.strictEqual(nativeChild.isRecording(), false);
          assert.strictEqual(
            nativeChild.spanContext().traceId,
            identity.traceId
          );
          nativeChild.end();
        });
        assert.strictEqual(api.context.active().getValue(spanKey), span);
        span.end();
      });
    }
    assert.strictEqual(tracer.startSpan('no-parent').isRecording(), false);
  },
};
