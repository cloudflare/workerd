// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { tracing } from 'cloudflare:workers';
import { channel } from 'node:diagnostics_channel';

const key = Symbol.for('opentelemetry.js.api.1');
const spanKey = Symbol.for('OpenTelemetry Context Key SPAN');
const api = globalThis[key];
// Registration must precede user module evaluation.
assert.strictEqual(api.version, '1.10.0');
assert.strictEqual(api, globalThis[key]);
const root = api.context.active();
const tracer = api.trace.getTracer('test', '1', { schemaUrl: 'schema' });

export const registrationAndSurface = {
  test(_ctrl, _env, ctx) {
    for (const object of [tracing, ctx.tracing]) {
      assert.strictEqual(object.withActiveSpanContext, undefined);
      assert.strictEqual(object.createAsyncContextVariable, undefined);
    }
    assert.strictEqual(
      tracer,
      api.trace.getTracer('test', '1', { schemaUrl: 'schema' })
    );
    assert.notStrictEqual(tracer, api.trace.getTracer('test', '2'));
    assert.notStrictEqual(
      tracer,
      api.trace.getTracer('test', '1', { schemaUrl: 'other' })
    );
    assert.strictEqual(api.context.active(), api.context.active());
    const invocation = api.context.active().getValue(spanKey);
    assert.deepStrictEqual(invocation.spanContext(), {
      ...tracing.getActiveSpan().spanContext(),
    });
    assert.strictEqual(invocation.spanContext().traceFlags, 1);
    const span = tracer.startSpan('otel-surface', {
      attributes: { a: [true, false] },
    });
    assert.strictEqual(span.setAttribute('x', {}), span);
    assert.strictEqual(span.setAttributes(null), span);
    assert.strictEqual(span.setStatus(null), span);
    assert.strictEqual(
      span
        .addEvent('event')
        .addLink({})
        .addLinks([])
        .updateName('otel-surface'),
      span
    );
    assert.strictEqual(span.isRecording(), true);
    span.recordException(new Error('recorded'));
    span.recordException({ code: 'E_CODE', name: 'Ignored', message: 'coded' });
    span.recordException({ code: 503 });
    span.recordException(null);
    span.recordException({
      get message() {
        throw new Error('bad getter');
      },
    });
    const identity = span.spanContext();
    span.end();
    span.end();
    assert.strictEqual(span.isRecording(), false);
    assert.deepStrictEqual(span.spanContext(), identity);
    // The API owns diag registration independently; the provider must remain extensible.
    api.diag = { error() {} };
    assert.strictEqual(globalThis[key].diag, api.diag);
    delete api.diag;
  },
};

export const asyncContext = {
  async test(_ctrl, env) {
    const valueKey = Symbol('opaque');
    const ctx = api.context.active().setValue(valueKey, 'value');
    await api.context.with(
      ctx,
      async function (a) {
        assert.strictEqual(this.tag, 'this');
        assert.strictEqual(a, 42);
        assert.strictEqual(api.context.active(), ctx);
        await new Promise((resolve) => setTimeout(resolve, 1));
        await new Promise((resolve) => queueMicrotask(resolve));
        assert.strictEqual(api.context.active(), ctx);
        const target = new EventTarget();
        let fired = false;
        target.addEventListener('test', () => {
          fired = true;
          assert.strictEqual(api.context.active(), ctx);
        });
        await Promise.resolve();
        target.dispatchEvent(new Event('test'));
        assert.ok(fired);
        const bound = api.context.bind(ctx, function (x) {
          return [this.tag, x, api.context.active()];
        });
        assert.deepStrictEqual(bound.call({ tag: 'bound' }, 7), [
          'bound',
          7,
          ctx,
        ]);
        await tracer.startActiveSpan('otel-parent', async (span) => {
          assert.strictEqual(api.context.active().getValue(spanKey), span);
          await Promise.resolve();
          assert.strictEqual(api.context.active().getValue(spanKey), span);
          tracer.startSpan('otel-child').end();
          await env.fetchTarget.fetch('http://mock/');
          const stream = new ReadableStream({
            start(controller) {
              controller.enqueue('value');
              controller.close();
            },
          });
          for await (const value of stream) {
            assert.strictEqual(value, 'value');
            assert.strictEqual(api.context.active().getValue(spanKey), span);
          }
          span.end();
        });
        assert.strictEqual(api.context.active(), ctx);
      },
      { tag: 'this' },
      42
    );
    assert.strictEqual(api.context.active().getValue(valueKey), undefined);
    assert.throws(() => api.context.with(root, 42), TypeError);
    assert.strictEqual(
      api.context.bind(ctx, function (a, b, c, d) {}).length,
      4
    );
    assert.throws(
      () =>
        api.context.with(root, () => {
          throw new Error('callback');
        }),
      /callback/
    );
  },
};

export const externalParentsAndRoots = {
  async test(_ctrl, env) {
    const getter = { get: (carrier, key) => carrier[key] };
    const extracted = api.propagation.extract(
      root,
      {
        traceparent: '00-11111111111111111111111111111111-2222222222222222-01',
        tracestate: 'vendor=value',
        baggage: 'tenant=hello%20world;property',
      },
      getter
    );
    assert.strictEqual(extracted.getValue(spanKey).isRecording(), false);
    assert.strictEqual(
      extracted.getValue(spanKey).spanContext().isRemote,
      true
    );
    await api.context.with(extracted, async () => {
      assert.strictEqual(api.context.active(), extracted);
      await tracer.startActiveSpan(
        'otel-remote-child',
        {},
        extracted,
        async (span) => {
          assert.strictEqual(
            span.spanContext().traceId,
            '11111111111111111111111111111111'
          );
          assert.strictEqual(
            span.spanContext().traceState.serialize(),
            'vendor=value'
          );
          span.setAttribute('foreign.attribute', 'value');
          span.updateName('otel-remote-renamed');
          span.setStatus({ code: 2, message: 'foreign failure' });
          console.log('foreign-log', span.spanContext());
          const diagnostic = channel('otel-foreign');
          const listener = () => {};
          diagnostic.subscribe(listener);
          diagnostic.publish({ message: 'foreign-diagnostic' });
          diagnostic.unsubscribe(listener);
          span.recordException({
            code: 'E_FOREIGN',
            name: 'ForeignError',
            message: 'foreign exception',
          });
          await env.fetchTarget.fetch('http://mock/');
          span.end();
        }
      );
    });
    const carrier = {};
    api.propagation.inject(extracted, carrier, {
      set: (c, k, v) => {
        c[k] = v;
      },
    });
    assert.strictEqual(
      carrier.traceparent,
      '00-11111111111111111111111111111111-2222222222222222-01'
    );
    assert.strictEqual(carrier.baggage, 'tenant=hello%20world;property');
    const detached = api.context.active().deleteValue(spanKey);
    api.context.with(detached, () => {
      assert.strictEqual(api.context.active(), detached);
      assert.strictEqual(api.context.active().getValue(spanKey), undefined);
      tracer.startSpan('otel-deleted-root').end();
    });
    const span = tracer.startSpan('otel-new-root', { root: true }, extracted);
    assert.notStrictEqual(
      span.spanContext().traceId,
      '11111111111111111111111111111111'
    );
    assert.strictEqual(
      span.spanContext().traceId,
      tracing.getActiveSpan().spanContext().traceId
    );
    span.end();
    tracing.startActiveSpan('otel-native-parent', (nativeSpan) => {
      const ctx = root.setValue(spanKey, nativeSpan);
      api.context.with(ctx, () => {
        const wrapped = api.context.active().getValue(spanKey);
        assert.strictEqual(wrapped.isRecording(), true);
        assert.deepStrictEqual(wrapped.spanContext(), nativeSpan.spanContext());
        assert.strictEqual(wrapped.setStatus({ code: 1 }), wrapped);
        tracer.startSpan('otel-native-child').end();
      });
      nativeSpan.end();
    });
    const unsampled = api.propagation.extract(
      root,
      {
        traceparent: '00-11111111111111111111111111111111-2222222222222222-00',
      },
      getter
    );
    await tracer.startActiveSpan(
      'otel-unsampled',
      {},
      unsampled,
      async (span) => {
        assert.strictEqual(span.isRecording(), false);
        assert.strictEqual(span.spanContext().traceFlags, 0);
        assert.strictEqual(
          span.spanContext().traceId,
          '11111111111111111111111111111111'
        );
        assert.match(span.spanContext().spanId, /^[0-9a-f]{16}$/);
        assert.strictEqual(api.context.active().getValue(spanKey), span);
        assert.strictEqual(tracing.getActiveSpan(), undefined);
        const requestedRoot = tracer.startSpan(
          'otel-unsampled-requested-root',
          {
            root: true,
          }
        );
        // An explicit root restores the invocation's recording policy, not the unrelated
        // unsampled surrounding context. Its native descendants share an exported parent.
        assert.strictEqual(requestedRoot.isRecording(), true);
        assert.notStrictEqual(
          requestedRoot.spanContext().traceId,
          '00000000000000000000000000000000'
        );
        requestedRoot.end();
        const child = tracer.startSpan('otel-unsampled-child');
        assert.strictEqual(child.isRecording(), false);
        assert.strictEqual(
          child.spanContext().traceId,
          span.spanContext().traceId
        );
        child.end();
        tracing.enterSpan('otel-unsampled-native', (nativeSpan) => {
          assert.strictEqual(nativeSpan.isTraced, false);
          assert.strictEqual(nativeSpan.spanContext().traceFlags, 0);
          tracing.enterSpan(
            'otel-unsampled-native-grandchild',
            (grandchild) => {
              assert.strictEqual(grandchild.isTraced, false);
              assert.strictEqual(
                grandchild.spanContext().traceId,
                span.spanContext().traceId
              );
            }
          );
          const nested = tracer.startSpan('otel-unsampled-native-child');
          assert.strictEqual(nested.isRecording(), false);
          assert.strictEqual(
            nested.spanContext().traceId,
            span.spanContext().traceId
          );
          assert.strictEqual(nested.spanContext().traceFlags, 0);
          nested.end();
        });
        await env.fetchTarget.fetch('https://worker/unsampled');
        span.end();
      }
    );
  },
};

export const propagationValidation = {
  test() {
    const getter = { get: (carrier, key) => carrier[key] };
    const parent = '00-11111111111111111111111111111111-2222222222222222-03';
    const ctx = api.propagation.extract(
      root,
      {
        traceparent: [parent, 'ignored'],
        tracestate:
          'vendor=value,3tenant@vendor=other,3invalid=ignored,invalid@1vendor=ignored',
      },
      getter
    );
    const sc = ctx.getValue(spanKey).spanContext();
    assert.strictEqual(sc.traceFlags, 3);
    const child = tracer.startSpan('otel-extra-flags', {}, ctx);
    assert.strictEqual(child.spanContext().traceFlags, 1);
    child.end();
    const upper = root.setValue(spanKey, {
      spanContext: () => ({
        traceId: 'ABCDEFABCDEFABCDEFABCDEFABCDEFAB',
        spanId: 'ABCDEFABCDEFABCD',
        traceFlags: 1,
      }),
    });
    const upperChild = tracer.startSpan('otel-uppercase', {}, upper);
    assert.strictEqual(
      upperChild.spanContext().traceId,
      'abcdefabcdefabcdefabcdefabcdefab'
    );
    upperChild.end();
    assert.strictEqual(
      sc.traceState.serialize(),
      'vendor=value,3tenant@vendor=other'
    );
    assert.strictEqual(
      sc.traceState.set('vendor', 'new').serialize(),
      'vendor=new,3tenant@vendor=other'
    );
    assert.strictEqual(
      sc.traceState.serialize(),
      'vendor=value,3tenant@vendor=other'
    );
    const carrier = {};
    api.propagation.inject(ctx, carrier, {
      set: (c, k, v) => {
        c[k] = v;
      },
    });
    assert.strictEqual(carrier.traceparent, parent);
    for (const traceparent of [
      parent + '-extra',
      parent.replace('00-', 'ff-'),
      parent.replace('111', 'AAA'),
      parent.replace('2222222222222222', '0000000000000000'),
    ]) {
      assert.strictEqual(
        api.propagation.extract(root, { traceparent }, getter),
        root
      );
    }
    const withBrokenBaggage = api.propagation.extract(
      root,
      {},
      {
        get(_carrier, key) {
          if (key === 'traceparent') return parent;
          throw new Error('broken carrier');
        },
      }
    );
    assert.strictEqual(
      withBrokenBaggage.getValue(spanKey).spanContext().traceId,
      sc.traceId
    );
    const entries = api.propagation.extract(
      root,
      {
        baggage: `a=${'x'.repeat(4000)},b=${'y'.repeat(4000)},c=${'z'.repeat(4000)}`,
      },
      getter
    );
    const baggageCarrier = {};
    api.propagation.inject(entries, baggageCarrier, {
      set: (c, k, v) => {
        c[k] = v;
      },
    });
    assert.ok(baggageCarrier.baggage.length <= 8192);
    assert.ok(baggageCarrier.baggage.includes('b='));
    assert.ok(!baggageCarrier.baggage.includes('c='));
    const oversized = api.propagation.extract(
      root,
      {
        traceparent: parent,
        tracestate: `a=${'a'.repeat(250)},b=${'b'.repeat(250)},c=0123456789`,
        baggage: 'empty=,valid=ok',
      },
      getter
    );
    assert.strictEqual(
      oversized.getValue(spanKey).spanContext().traceState.serialize(),
      ''
    );
    const output = {};
    api.propagation.inject(oversized, output, {
      set(c, k, v) {
        c[k] = v;
      },
    });
    assert.strictEqual(output.baggage, 'valid=ok');
  },
};

export const overloads = {
  test() {
    const valueKey = Symbol('overload');
    const active = api.context.active().setValue(valueKey, 'active');
    api.context.with(active, () => {
      for (const args of [[], [{}], [{}, undefined], [{}, active]]) {
        const result = tracer.startActiveSpan(
          `otel-overload-${args.length}-${args[1] ? 'explicit' : 'active'}`,
          ...args,
          (span) => {
            assert.strictEqual(
              api.context.active().getValue(valueKey),
              'active'
            );
            assert.strictEqual(api.context.active().getValue(spanKey), span);
            span.end();
            return 42;
          }
        );
        assert.strictEqual(result, 42);
      }
    });
    tracer.startSpan('otel-root-context', {}, root).end();
  },
};

export const serviceWorkerGetter = {
  async test(_ctrl, env) {
    const response = await env.serviceWorker.fetch('https://worker/');
    assert.strictEqual(
      await (await env.newServiceWorker.fetch('https://worker/')).text(),
      'ok'
    );
    assert.strictEqual(await response.text(), 'ok');
  },
};
