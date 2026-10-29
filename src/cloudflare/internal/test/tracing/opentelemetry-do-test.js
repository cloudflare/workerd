// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

const api = globalThis[Symbol.for('opentelemetry.js.api.1')];
const root = api.context.active();
const tracer = api.trace.getTracer('overlapping');

export class Overlapping extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.resume = new Promise((resolve) => {
      this.release = resolve;
    });
    this.attributed = new Promise((resolve) => {
      this.finish = resolve;
    });
  }

  record(name) {
    // Both paths use the private context primitive, not an already-observed native parent.
    tracer.startSpan(`otel-do-root-${name}`, { root: true }).end();
    const remote = api.propagation.extract(
      root,
      {
        traceparent: '00-11111111111111111111111111111111-2222222222222222-01',
      },
      { get: (c, k) => c[k] }
    );
    tracer.startActiveSpan(`otel-do-remote-${name}`, {}, remote, (span) => {
      console.log(`otel-do-child-${name}`);
      span.end();
    });
  }

  async fetch(request) {
    const name = new URL(request.url).pathname.slice(1);
    console.log(`otel-do-owner-${name}`);
    if (name === 'a') {
      return new Response(
        new ReadableStream({
          pull: async (controller) => {
            controller.enqueue(new TextEncoder().encode('ready'));
            await this.resume;
            this.record('a');
            this.finish();
            controller.close();
          },
        })
      );
    }
    this.record('b');
    this.release();
    await this.attributed;
    return new Response('b');
  }
}

export const overlapping = {
  async test(_ctrl, env) {
    const stub = env.OVERLAPPING.get(env.OVERLAPPING.idFromName('overlap'));
    const first = await stub.fetch('https://object/a');
    const reader = first.body.getReader();
    assert.strictEqual(
      new TextDecoder().decode((await reader.read()).value),
      'ready'
    );
    const second = await stub.fetch('https://object/b');
    assert.strictEqual((await reader.read()).done, true);
    assert.strictEqual(await second.text(), 'b');
  },
};
