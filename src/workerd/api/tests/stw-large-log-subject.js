// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// The worker logs a payload that exceeds the per-event tail size.
export default {
  async fetch(req, env, ctx) {
    const url = new URL(req.url);

    if (url.pathname === '/big-attribute') {
      ctx.tracing.enterSpan('big-attribute-span', (span) => {
        span.setAttribute('marker', 'attr-marker');
        span.setAttribute('big', 'A'.repeat(300 * 1024));
      });
      console.log('subject: big-attribute span closed');
    }

    // A single console.log larger than MAX_TRACE_BYTES.
    if (url.pathname === '/big-log') {
      const big = 'A'.repeat(300 * 1024);
      console.log(big);
      console.log('marker-after-big-log');
    }

    return new Response('ok');
  },
};

// The assertions on what the tail worker received live in tail-worker-test.js
export const test = {
  async test(ctrl, env) {
    const bigLog = await env.SERVICE.fetch('http://subject/big-log');
    assert.strictEqual(await bigLog.text(), 'ok');

    const bigAttribute = await env.SERVICE.fetch(
      'http://subject/big-attribute'
    );
    assert.strictEqual(await bigAttribute.text(), 'ok');
  },
};
