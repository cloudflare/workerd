// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/metrics') {
      assert.strictEqual(request.method, 'GET');
      // Regression test: read the request body before responding, matching
      // production behavior. If we omit expectedBodySize on the GET request,
      // the body pipe is never closed and this arrayBuffer() call deadlocks
      await request.arrayBuffer();
      return Response.json({
        backlogCount: 100,
        backlogBytes: 2048,
        oldestMessageTimestamp: 1000000,
      });
    }
    return new Response();
  },

  async test(ctrl, env, ctx) {
    assert.strictEqual(typeof env.QUEUE.metrics, 'function');
    const metrics = await env.QUEUE.metrics();
    assert.strictEqual(metrics.backlogCount, 100);
    assert.strictEqual(metrics.backlogBytes, 2048);
    assert.ok(
      metrics.oldestMessageTimestamp instanceof Date,
      'Expected oldestMessageTimestamp to be a Date'
    );
    assert.strictEqual(metrics.oldestMessageTimestamp.getTime(), 1000000);
  },
};
