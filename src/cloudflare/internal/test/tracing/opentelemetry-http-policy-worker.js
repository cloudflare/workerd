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
    const injected = {};
    propagation.inject(context.active(), injected);
    assert.strictEqual(
      injected.traceparent.split('-')[3],
      (active.traceFlags & 3).toString(16).padStart(2, '0')
    );
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
    if (path === '/platform-unsampled') {
      assert.strictEqual(active.traceFlags, 0);
      const foreign = trace.setSpanContext(context.active(), {
        traceId: '11111111111111111111111111111111',
        spanId: '2222222222222222',
        traceFlags: 1,
      });
      const tracer = trace.getTracer('platform-unsampled');
      tracer.startActiveSpan('platform-unsampled', {}, foreign, (span) => {
        assert.strictEqual(span.isRecording(), false);
        assert.strictEqual(
          span.spanContext().traceId,
          '11111111111111111111111111111111'
        );
        const child = tracer.startSpan('platform-unsampled-child');
        assert.strictEqual(child.isRecording(), false);
        child.end();
        const root = tracer.startSpan('platform-unsampled-root', {
          root: true,
        });
        assert.strictEqual(root.isRecording(), false);
        assert.strictEqual(root.spanContext().traceFlags, 0);
        assert.notStrictEqual(root.spanContext().traceId, active.traceId);
        assert.notStrictEqual(
          root.spanContext().traceId,
          span.spanContext().traceId
        );
        root.end();
        span.end();
      });
      // No shim startSpan: native activation must apply the owning platform decision too.
      const echo = await context.with(foreign, () => {
        const carrier = {};
        propagation.inject(context.active(), carrier);
        assert.strictEqual(
          carrier.traceparent,
          '00-11111111111111111111111111111111-2222222222222222-01'
        );
        return get('http://echo/platform-unsampled');
      });
      assert.strictEqual(
        echo.traceparent,
        '00-11111111111111111111111111111111-2222222222222222-00'
      );
      return Response.json({ active });
    } else if (path.startsWith('/state-')) {
      const base = trace.getActiveSpan().spanContext();
      const updated = base.traceState.unset('upstream').set('updated', '1');
      const foreign = path !== '/state-updated';
      const state = path === '/state-drop' ? undefined : updated;
      const selected = trace.setSpanContext(context.active(), {
        ...base,
        ...(foreign
          ? {
              traceId: '11111111111111111111111111111111',
              spanId: '2222222222222222',
            }
          : {}),
        traceState: state,
      });
      const originalState = request.headers.get('tracestate');
      const outgoing = new Request(request);
      if (path === '/state-custom')
        outgoing.headers.set('tracestate', 'custom=1');
      result = await context.with(selected, () => get(outgoing));
      assert.strictEqual(
        result.traceparent.split('-')[1],
        foreign ? '11111111111111111111111111111111' : active.traceId
      );
      assert.strictEqual(
        result.tracestate,
        path === '/state-custom' ? 'custom=1' : state?.serialize()
      );
      assert.strictEqual(request.headers.get('tracestate'), originalState);
    } else if (path === '/nested-sampling') {
      const same = trace.setSpanContext(context.active(), {
        ...trace.getActiveSpan().spanContext(),
        traceFlags: 0,
      });
      const foreign = trace.setSpanContext(context.active(), {
        traceId: '11111111111111111111111111111111',
        spanId: '2222222222222222',
        traceFlags: 0,
      });
      const tracer = trace.getTracer('nested-sampling');
      await context.with(foreign, async () => {
        await tracer.startActiveSpan(
          'nested-same-trace',
          {},
          same,
          async (span) => {
            assert.strictEqual(span.isRecording(), true);
            await get('http://echo/nested');
            span.end();
          }
        );
        const span = tracer.startSpan('rewrapped-same-trace', {}, same);
        assert.strictEqual(span.isRecording(), true);
        const rewrapped = trace.setSpan(
          context.active(),
          trace.wrapSpanContext(span.spanContext())
        );
        await context.with(rewrapped, () => get('http://echo/rewrapped'));
        span.end();
      });
      result = await get('http://echo/restored');
    } else if (path === '/malformed-context') {
      const base = context.active();
      const malformed = {
        getValue(key) {
          if (
            key === Symbol.for('OpenTelemetry SDK Context Key SUPPRESS_TRACING')
          )
            throw new Error('malformed suppression');
          return base.getValue(key);
        },
        setValue: base.setValue.bind(base),
        deleteValue: base.deleteValue.bind(base),
      };
      result = await context.with(malformed, () =>
        get('http://echo/malformed')
      );
      assert.ok(result.traceparent);
    } else if (path === '/override') {
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
      const span = tracing.startSpan('native-untraced-caller');
      const caller = span.spanContext();
      const received = await tracing
        .withActiveSpan(span, () => env.UNTRACED.fetch('http://callee/fresh'))
        .then((r) => r.json());
      assert.strictEqual(received.active.traceId, active.traceId);
      assert.notStrictEqual(received.active.spanId, caller.spanId);
      assert.strictEqual(received.active.traceFlags, 0);
      assert.strictEqual(
        received.echo.traceparent.split('-')[2],
        received.active.spanId
      );
      assert.strictEqual(received.echo.traceparent.split('-')[3], '00');
      assert.strictEqual(
        received.echo.traceparent.split('-')[1],
        active.traceId
      );
      span.end();
      console.log(JSON.stringify({ path, received, caller }));
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
