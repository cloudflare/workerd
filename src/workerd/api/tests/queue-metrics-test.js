// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/metrics') {
      assert.strictEqual(request.method, 'GET');
      return Response.json({
        backlogCount: 100,
        backlogBytes: 2048,
        oldestMessageTimestamp: 1000000,
      });
    }
    return new Response();
  },

  async test(ctrl, env, ctx) {
    const metricsEnabled = env.METRICS_FLAG;
    if (metricsEnabled) {
      // Flag ON → metrics() should exist and return data
      assert.strictEqual(typeof env.QUEUE.metrics, 'function');
      const metrics = await env.QUEUE.metrics();
      assert.strictEqual(metrics.backlogCount, 100);
      assert.strictEqual(metrics.backlogBytes, 2048);
      assert.strictEqual(metrics.oldestMessageTimestamp, 1000000);
    } else {
      // Flag OFF → metrics() should not be exposed on the binding
      assert.strictEqual(typeof env.QUEUE.metrics, 'undefined');
    }
  },
};
