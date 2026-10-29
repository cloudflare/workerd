// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { tracing, WorkerEntrypoint } from 'cloudflare:workers';
import { context, propagation, ROOT_CONTEXT, trace } from '@opentelemetry/api';

export default class extends WorkerEntrypoint {
  identity() {
    return trace.getActiveSpan().spanContext();
  }
  async fetch(request) {
    if (new URL(request.url).pathname === '/callee')
      return Response.json(this.identity());
    const tracer = trace.getTracer('roots');
    const invocation = trace.getActiveSpan()?.spanContext();
    // A foreign unsampled context must not choose a requested root's recording mode.
    const remote = propagation.extract(ROOT_CONTEXT, {
      traceparent: '00-33333333333333333333333333333333-4444444444444444-00',
      tracestate: 'remote=1',
    });
    let result;
    await context.with(remote, () =>
      tracer.startActiveSpan('detached', { root: true }, async (root) => {
        assert.strictEqual(
          root.spanContext().traceState?.serialize() ?? '',
          ''
        );
        assert.notStrictEqual(root.spanContext().isRemote, true);
        const rootId = root.spanContext();
        root.setAttribute('root.trace', rootId.traceId);
        root.addEvent('event');
        if (invocation !== undefined) root.addLink({ context: invocation });
        root.recordException(new Error('handled'), [1700000000, 1]);
        root.updateName('detached');
        root.setStatus({ code: 1 });
        console.log(JSON.stringify({ marker: 'root', ...rootId }));
        const echo = await fetch('http://echo/').then((r) => r.json());
        const childEcho = await tracer.startActiveSpan(
          'otel-child',
          async (child) => {
            assert.strictEqual(child.isRecording(), root.isRecording());
            assert.strictEqual(child.spanContext().traceId, rootId.traceId);
            const headers = await fetch('http://echo/child').then((r) =>
              r.json()
            );
            child.end();
            return headers;
          }
        );
        if (this.env.CALLEE && root.isRecording()) {
          const callee = await this.env.CALLEE.fetch(
            'http://callee/callee'
          ).then((r) => r.json());
          assert.strictEqual(callee.traceId, rootId.traceId);
          const rpc = await this.env.CALLEE.identity();
          assert.strictEqual(rpc.traceId, rootId.traceId);
        }
        tracing.startActiveSpan('native-child', (native) => {
          if (root.isRecording())
            assert.strictEqual(native.spanContext().traceId, rootId.traceId);
          native.end();
        });
        const carrier = {};
        propagation.inject(context.active(), carrier);
        if (rootId.traceId !== '0'.repeat(32))
          assert.strictEqual(carrier.traceparent.split('-')[1], rootId.traceId);
        else assert.strictEqual(carrier.traceparent, undefined);
        result = {
          root: rootId,
          invocation,
          echo,
          childEcho,
          recording: root.isRecording(),
        };
        root.end();
      })
    );
    context.with(ROOT_CONTEXT, () => {
      tracing.startSpan('bare-native').end();
      tracer.startSpan('context-root').end();
    });
    context.with(trace.deleteSpan(context.active()), () =>
      tracer.startSpan('deleted-root').end()
    );
    const invalid = trace.setSpanContext(context.active(), {
      traceId: '0'.repeat(32),
      spanId: '0'.repeat(16),
      traceFlags: 0,
    });
    tracer.startSpan('invalid-parent', {}, invalid).end();
    // Destructor-driven closure must precede outcome, even for roots not explicitly ended.
    tracer.startSpan('left-open', { root: true });
    return Response.json(result);
  }
}
