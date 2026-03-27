// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests that the upstream sentinel value of 0 for oldestMessageTimestamp is
// correctly converted to undefined (kj::none) by clearEpochSentinel().

import assert from 'node:assert';

export default {
  async fetch(request) {
    const { pathname } = new URL(request.url);

    if (pathname === '/metrics') {
      return Response.json({
        backlogCount: 5,
        backlogBytes: 100,
        oldestMessageTimestamp: 0,
      });
    }

    if (pathname === '/message') {
      await request.arrayBuffer();
      return Response.json({
        metadata: {
          metrics: {
            backlogCount: 5,
            backlogBytes: 100,
            oldestMessageTimestamp: 0,
          },
        },
      });
    }

    if (pathname === '/batch') {
      await request.arrayBuffer();
      return Response.json({
        metadata: {
          metrics: {
            backlogCount: 10,
            backlogBytes: 200,
            oldestMessageTimestamp: 0,
          },
        },
      });
    }

    return new Response('Not Found', { status: 404 });
  },

  async test(ctrl, env) {
    // Test metrics() zero-sentinel → undefined
    const metrics = await env.QUEUE.metrics();
    assert.strictEqual(metrics.backlogCount, 5);
    assert.strictEqual(metrics.backlogBytes, 100);
    assert.strictEqual(
      metrics.oldestMessageTimestamp,
      undefined,
      'Expected oldestMessageTimestamp to be undefined when upstream sends 0'
    );

    // Test send() zero-sentinel → undefined
    const sendResult = await env.QUEUE.send('abc', { contentType: 'text' });
    assert.strictEqual(sendResult.metadata.metrics.backlogCount, 5);
    assert.strictEqual(sendResult.metadata.metrics.backlogBytes, 100);
    assert.strictEqual(
      sendResult.metadata.metrics.oldestMessageTimestamp,
      undefined,
      'Expected send oldestMessageTimestamp to be undefined when upstream sends 0'
    );

    // Test sendBatch() zero-sentinel → undefined
    const sendBatchResult = await env.QUEUE.sendBatch([
      { body: 'def', contentType: 'text' },
    ]);
    assert.strictEqual(sendBatchResult.metadata.metrics.backlogCount, 10);
    assert.strictEqual(sendBatchResult.metadata.metrics.backlogBytes, 200);
    assert.strictEqual(
      sendBatchResult.metadata.metrics.oldestMessageTimestamp,
      undefined,
      'Expected sendBatch oldestMessageTimestamp to be undefined when upstream sends 0'
    );
  },
};
