// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { context, trace, createTraceState } from '@opentelemetry/api';
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
    provider
      .getTracer('a'.repeat(255) + '💥', 'v'.repeat(255) + '💥', {
        schemaUrl: 's'.repeat(2047) + '💥',
      })
      .startSpan('scope-limit')
      .end();
    const limits = tracer.startSpan('count-limits');
    const attributes = Object.fromEntries(
      Array.from({ length: 130 }, (_, i) => [`k${i}`, i])
    );
    for (let i = 0; i < 130; i++)
      limits.addEvent('count', i === 0 ? attributes : {});
    limits.addLinks(
      Array.from({ length: 130 }, (_, i) => ({
        context: linkedContext,
        attributes: i === 0 ? attributes : {},
      }))
    );
    limits.end();
    const budget = tracer.startSpan('event-budget');
    budget.addEvent('too-large', { text: 'x'.repeat(70000) });
    budget.setAttribute('starved', true);
    budget.end();
    const linkBudget = tracer.startSpan('link-budget');
    linkBudget.addLink({
      context: linkedContext,
      attributes: { text: 'x'.repeat(70000) },
    });
    linkBudget.end();
    const times = tracer.startSpan('time-validation', { startTime: [-1, 0] });
    times.addEvent('overload', [1, 1], [1700000000, 2]);
    times.addEvent('third-date', {}, new Date(1700000000123));
    times.addEvent('third-number', {}, 1700000000456);
    times.addEvent('normalized', [1700000000.5, 1000000001]);
    times.addEvent('negative-carry', [1700000001, -1]);
    times.addEvent('negative-time', [-1, 0]);
    times.addEvent('overflow-time', [9223372036, 854775808]);
    times.addEvent('max-time', [9223372036, 854775807]);
    times.addEvent('a'.repeat(63) + '💥');
    times.addEvent('oversized-array', { values: Array(513).fill(1) });
    times.addLink({
      context: {
        ...linkedContext,
        traceId: '00000000000000001111111111111111',
        traceState: { serialize: () => 'invalid state' },
      },
    });
    times.end([9223372036, 854775808]);
    tracer
      .startSpan('future-start', { startTime: [9223372036, 854775807] })
      .end();
    tracer.startActiveSpan('otel-parent', (parent) => {
      tracing.startSpan('runtime-child').end();
      parent.end();
    });
    context.with(
      context
        .active()
        .setValue(
          Symbol.for('OpenTelemetry SDK Context Key SUPPRESS_TRACING'),
          true
        ),
      () => {
        const suppressed = tracer.startSpan('must-not-export');
        suppressed.addEvent('suppressed');
        suppressed.addLink({ context: linkedContext });
        suppressed.recordException(new Error('suppressed'), [1700000000, 1]);
        suppressed.end();
      }
    );
    // Extra arguments to the public API remain ignored, including while the flag is off.
    tracing
      .startSpan(`native-${env.enabled}`, {
        kind: 4,
        scope: { name: 'ignored' },
      })
      .end();
  },
};
