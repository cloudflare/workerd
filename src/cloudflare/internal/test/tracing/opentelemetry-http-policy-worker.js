// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { DurableObject, tracing } from 'cloudflare:workers';
import { context, propagation, ROOT_CONTEXT, trace } from '@opentelemetry/api';

const get = (request) => fetch(request).then((r) => r.json());
const identity = () => {
  const sc = trace.getActiveSpan().spanContext();
  return { ...sc, state: sc.traceState?.serialize() };
};
const noHeaders = (result) => {
  assert.strictEqual(result.traceparent, undefined);
  assert.strictEqual(result.tracestate, undefined);
};
export default {
  async fetch(request, env) {
    const path = new URL(request.url).pathname;
    const active = identity();
    let result;
    if (path === '/mixed' && env.CALLEE) {
      assert.strictEqual(active.traceFlags, 0);
      const headers = {};
      propagation.inject(context.active(), headers);
      return env.CALLEE.fetch('http://callee/mixed', { headers });
    }
    if (path === '/mixed') {
      const extracted = propagation.extract(
        ROOT_CONTEXT,
        Object.fromEntries(request.headers)
      );
      return context.with(extracted, () =>
        trace
          .getTracer('mixed')
          .startActiveSpan('mixed-child', async (span) => {
            assert.strictEqual(span.isRecording(), true);
            result = await get('http://echo/mixed');
            span.end();
            console.log(JSON.stringify({ path, echo: result }));
            return Response.json({ recording: true });
          })
      );
    }
    if (path === '/forward' || path === '/clone') {
      const original = request.headers.get('traceparent');
      result = await get(path === '/clone' ? new Request(request) : request);
      assert.strictEqual(request.headers.get('traceparent'), original);
      console.log(JSON.stringify({ path, echo: result }));
      return Response.json({ active, echo: result, original });
    }
    if (path === '/override') {
      const explicit =
        '00-11111111111111111111111111111111-2222222222222222-ff';
      const copy = new Request(request, {
        headers: { traceparent: explicit, tracestate: 'custom=1' },
      });
      result = await get(copy);
      assert.strictEqual(result.traceparent, explicit);
      assert.strictEqual(result.tracestate, 'custom=1');
    } else if (path === '/redirect') {
      result = await get('http://echo/redirect');
      assert.strictEqual(result.host, 'other-origin');
      assert.strictEqual(result.traceparent.split('-')[1], active.traceId);
      assert.strictEqual(result.tracestate, active.state);
    } else if (path === '/exclude') {
      noHeaders(await env.BOUND.fetch('http://bound/').then((r) => r.json()));
      const actor = env.ACTOR.get(env.ACTOR.idFromName('policy'));
      const response = await actor.fetch('http://actor/');
      const headers = await response.json();
      noHeaders(headers.received);
      assert.strictEqual(
        headers.egress.traceparent.split('-')[1],
        active.traceId
      );
    } else if (path === '/native-untraced') {
      const received = await env.UNTRACED.fetch('http://callee/fresh').then(
        (r) => r.json()
      );
      assert.strictEqual(received.active.traceId, active.traceId);
      assert.strictEqual(
        received.echo.traceparent.split('-')[1],
        active.traceId
      );
      return Response.json(received);
    } else if (path === '/suppress') {
      const suppressed = context
        .active()
        .setValue(
          Symbol.for('OpenTelemetry SDK Context Key SUPPRESS_TRACING'),
          true
        );
      await context.with(suppressed, async () => {
        await Promise.resolve();
        noHeaders(await get('http://echo/suppressed'));
        const native = tracing.startSpan('suppressed-native');
        assert.strictEqual(native.isTraced, false);
        native.end();
      });
      result = await get('http://echo/restored');
      assert.ok(result.traceparent);
    } else {
      const original = new Request('http://echo/fresh');
      result = await get(original);
      assert.strictEqual(original.headers.has('traceparent'), false);
      assert.strictEqual(original.headers.has('tracestate'), false);
    }
    console.log(JSON.stringify({ path, echo: result }));
    return Response.json({ active, echo: result });
  },
};

export class Actor extends DurableObject {
  async fetch(request) {
    return Response.json({
      received: Object.fromEntries(request.headers),
      egress: await get('http://echo/actor'),
    });
  }
}
