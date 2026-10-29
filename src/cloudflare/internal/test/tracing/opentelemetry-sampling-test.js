// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { context, trace } from '@opentelemetry/api';
import { tracing } from 'cloudflare:workers';

export const jsOnlySampling = {
  async test(_ctrl, env) {
    const invocation = tracing.getActiveSpan().spanContext();
    assert.strictEqual(invocation.traceFlags, env.expectedFlags);
    const tracer = trace.getTracer('sampling');
    const native = tracing.startSpan('sampling-native');
    assert.strictEqual(native.spanContext().traceFlags, env.expectedFlags);
    const rebuilt = trace.setSpan(
      context.active(),
      trace.wrapSpanContext(native.spanContext())
    );
    for (let i = 0; i < 9; i++) {
      const child = tracer.startSpan(`sampling-rebuilt-${i}`, {}, rebuilt);
      // A same-trace projected zero is not a native sampling decision. A buffered sink
      // still supplies recording spans, even though their JS projection remains zero.
      assert.strictEqual(child.isRecording(), env.expectedRecording);
      assert.strictEqual(child.spanContext().traceFlags, env.expectedFlags);
      child.end();
    }
    if (env.expectedFlags === 0 && env.callee) {
      const checkCallees = async (kind) => {
        for (const [callee, label, expected] of [
          [env.callee, 'on', 1],
          [env.flagOffCallee, 'off', 0],
        ]) {
          const response = await callee.fetch(
            `https://callee/sampling-buffered-${kind}-${label}`
          );
          assert.strictEqual(await response.json(), expected);
        }
      };
      await tracer.startActiveSpan(
        'sampling-buffered-active',
        {},
        rebuilt,
        async (span) => {
          assert.strictEqual(span.isRecording(), true);
          assert.strictEqual(span.spanContext().traceFlags, 0);
          await checkCallees('active');
          span.end();
        }
      );
      await context.with(rebuilt, () => checkCallees('rebuilt'));
    }
    if (env.expectedFlags === 1) {
      // A foreign explicit 00 is a sampling decision, unlike this invocation's projected 0.
      const unsampled = trace.setSpanContext(context.active(), {
        ...invocation,
        traceId: '11111111111111111111111111111111',
        traceFlags: 0,
      });
      await tracer.startActiveSpan(
        'sampling-unsampled',
        {},
        unsampled,
        async (span) => {
          assert.strictEqual(span.isRecording(), false);
          assert.strictEqual(tracing.getActiveSpan(), undefined);
          tracing.enterSpan('sampling-orphan-child', (child) => {
            assert.strictEqual(child.isTraced, false);
            assert.strictEqual(child.spanContext().traceFlags, 0);
          });
          assert.strictEqual(
            await (await env.callee.fetch('https://callee/unsampled')).json(),
            0
          );
          span.end();
        }
      );
      await context.with(rebuilt, async () => {
        for (const [callee, expected] of [
          [env.callee, 1],
          [env.flagOffCallee, 0],
        ]) {
          const response = await callee.fetch('https://callee/flags');
          assert.strictEqual(await response.json(), expected);
        }
        assert.strictEqual(await env.rpc.check(), 1);
      });
      const response = await env.sockets.fetch('https://callee/socket', {
        headers: { Upgrade: 'websocket' },
      });
      const socket = response.webSocket;
      socket.accept();
      const message = new Promise((resolve) =>
        socket.addEventListener('message', resolve, { once: true })
      );
      socket.send('check');
      assert.strictEqual((await message).data, 'ok');
      const closed = new Promise((resolve) =>
        socket.addEventListener('close', resolve, { once: true })
      );
      socket.close(1000, 'done');
      await closed;
    }
    native.end();
  },
};
