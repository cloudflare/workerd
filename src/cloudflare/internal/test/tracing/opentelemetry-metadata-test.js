// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { trace, createTraceState } from '@opentelemetry/api';
import { tracing } from 'cloudflare:workers';

export const metadata = {
  test(_controller, env) {
    // The API's convenience getTracer(name, version) drops a third argument; the provider
    // interface accepts TracerOptions, including schemaUrl.
    const provider = trace.getTracerProvider();
    const tracer = provider.getTracer('metadata-test', '1.2.3', {
      schemaUrl: 'https://example.com/schema',
    });
    const linkedContext = {
      traceId: '11111111111111111111111111111111',
      spanId: '2222222222222222',
      traceFlags: 255,
      isRemote: true,
      traceState: createTraceState('vendor=value'),
    };
    for (let kind = 0; kind <= 4; kind++) {
      const startTime = [
        [1700000000, 123456789],
        new Date(1700000000123),
        1700000000456,
        new Date(NaN),
        [1700000000, 1000000000],
      ][kind];
      const span = tracer.startSpan(`kind-${kind}`, {
        kind,
        ...(startTime === undefined ? {} : { startTime }),
        links:
          kind === 0
            ? [{ context: linkedContext, attributes: { source: 'options' } }]
            : [],
      });
      assert.strictEqual(span.isRecording(), env.enabled);
      if (kind === 0) {
        span.recordException(
          new Error('timed exception'),
          [1700000000, 223456789]
        );
        assert.strictEqual(span.addLink({ context: linkedContext }), span);
        span.addLinks([
          {
            context: { ...linkedContext, isRemote: false },
            attributes: { source: 'addLinks', invalid: {} },
          },
          { context: linkedContext },
          { context: { ...linkedContext, spanId: '0000000000000000' } },
        ]);
        span.addEvent(
          'step',
          { text: 'hello', count: 2, flags: [true, false], invalid: {} },
          [1700000000, 123456789]
        );
        span.addEvent('dated', new Date(1700000000123));
        span.addEvent('millis', 1700000000456);
        span.addEvent('implicit', { key: 'value' });
        span.addEvent('implicit');
      }
      span.updateName(`renamed-kind-${kind}`);
      span.end(
        [
          [1700000001, 987654321],
          new Date(1700000001123),
          1700000001456,
          Infinity,
          [1700000001, -1],
        ][kind]
      );
      span.end([1700000010, 0]);
      span.addEvent('after-end');
      span.recordException(new Error('after-end'), [1700000010, 0]);
      span.addLink({ context: linkedContext });
    }
    tracer.startSpan('default-kind').end();
    trace.getTracer('name-only').startSpan('name-only').end();
    trace.getTracer('metadata-test', '2.0').startSpan('other-version').end();
    provider
      .getTracer('empty', '', { schemaUrl: '' })
      .startSpan('empty-optionals')
      .end();
    // Extra arguments to the public API remain ignored, including while the flag is off.
    tracing
      .startSpan(`native-${env.enabled}`, {
        kind: 4,
        scope: { name: 'ignored' },
      })
      .end();
  },
};
