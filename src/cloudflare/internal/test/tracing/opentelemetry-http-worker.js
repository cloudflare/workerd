// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { WorkerEntrypoint, tracing } from 'cloudflare:workers';
import { context, propagation, ROOT_CONTEXT, trace } from '@opentelemetry/api';

const traceId = 'a3ce929d0e0e47364bf92f3577b34da6';
const spanId = 'c0ffee0ba902b7aa';
const custom = `00-${'1'.repeat(32)}-${spanId}-00`;
const echo = (path, init) =>
  fetch(`http://echo/${path}`, init).then((r) => r.json());

export default {
  async fetch(request, env) {
    const path = new URL(request.url).pathname;
    if (env.UNTRACED) {
      const flags = path.slice(-2);
      const extracted = propagation.extract(ROOT_CONTEXT, {
        traceparent: `00-${traceId}-${spanId}-${flags}`,
        tracestate: 'vendor=pass',
      });
      const result = await context.with(extracted, async () => {
        await Promise.resolve();
        return echo('pass');
      });
      const empty = await context.with(ROOT_CONTEXT, () => echo('empty'));
      const invalid = await context.with(
        trace.setSpanContext(ROOT_CONTEXT, {
          traceId: '0'.repeat(32),
          spanId,
          traceFlags: 1,
        }),
        () => echo('invalid')
      );
      return Response.json({ echo: result, empty, invalid });
    }
    const raw = (
      env.ON ? trace.getActiveSpan() : tracing.getActiveSpan()
    ).spanContext();
    if (env.ON && path.startsWith('/valid-')) {
      assert.strictEqual(
        trace.getActiveSpan().isRecording(),
        path !== '/valid-00'
      );
    }
    const active = { ...raw, traceState: raw.traceState?.serialize() };
    const tracer = trace.getTracer('http-test');
    let childRecording;
    const result = await tracer.startActiveSpan('http-parent', async (span) => {
      childRecording = span.isRecording();
      if (env.ON)
        assert.strictEqual(
          span.spanContext().traceState?.serialize(),
          active.traceState
        );
      const result = await echo(path);
      const original = new Request('http://echo/custom', {
        headers: { traceparent: custom, tracestate: 'vendor=custom' },
      });
      const overridden = await fetch(original).then((r) => r.json());
      assert.strictEqual(overridden.traceparent, custom);
      assert.strictEqual(overridden.tracestate, 'vendor=custom');
      assert.strictEqual(original.headers.get('traceparent'), custom);
      const invalid = await echo('custom-invalid', {
        headers: { traceparent: 'invalid' },
      });
      assert.strictEqual(invalid.traceparent, 'invalid');
      if (env.ON) {
        const bound = await (
          await env.NATIVE.fetch('http://native/binding', {
            headers: { traceparent: custom },
          })
        ).json();
        assert.strictEqual(bound.traceId, span.spanContext().traceId);
        assert.notStrictEqual(bound.spanId, spanId);
        const rpc = await env.NATIVE.identity();
        assert.strictEqual(rpc.traceId, span.spanContext().traceId);
        // RPC uses its existing native trigger, not HTTP header adoption. Vendor state is
        // in-memory only; this test does not extend the internal RPC/tail wire schema.
      }
      span.end();
      return result;
    });
    console.log(
      JSON.stringify({ path, active, echo: result, enabled: env.ON })
    );
    return Response.json({
      enabled: env.ON,
      active,
      childRecording,
      echo: result,
    });
  },
};

export class Native extends WorkerEntrypoint {
  identity() {
    const sc = trace.getActiveSpan().spanContext();
    return {
      traceId: sc.traceId,
      spanId: sc.spanId,
      state: sc.traceState?.serialize(),
    };
  }
  fetch(request) {
    // Binding fetch has only the explicit header, never automatic HTTP state injection.
    assert.strictEqual(request.headers.get('tracestate'), null);
    return Response.json(this.identity());
  }
}
