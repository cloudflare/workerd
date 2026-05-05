// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const SEND_RESPONSE = {
  metadata: {
    metrics: {
      backlogCount: 100,
      backlogBytes: 2048,
      oldestMessageTimestamp: 1000000,
    },
  },
};

const SEND_BATCH_RESPONSE = {
  metadata: {
    metrics: {
      backlogCount: 200,
      backlogBytes: 4096,
      oldestMessageTimestamp: 2000000,
    },
  },
};

export default {
  async fetch(request) {
    const { pathname } = new URL(request.url);

    if (pathname === '/message') {
      const text = await request.text();
      assert.strictEqual(request.method, 'POST');
      assert.strictEqual(text, 'abc');
      return Response.json(SEND_RESPONSE);
    }

    if (pathname === '/batch') {
      assert.strictEqual(request.method, 'POST');
      const body = await request.json();
      assert.strictEqual(body.messages.length, 1);
      return Response.json(SEND_BATCH_RESPONSE);
    }

    return new Response('Not Found', { status: 404 });
  },

  async test(ctrl, env) {
    const sendResult = await env.QUEUE.send('abc', { contentType: 'text' });
    const sendBatchResult = await env.QUEUE.sendBatch([
      { body: 'def', contentType: 'text' },
    ]);

    assert.strictEqual(sendResult.metadata.metrics.backlogCount, 100);
    assert.strictEqual(sendResult.metadata.metrics.backlogBytes, 2048);
    assert.ok(
      sendResult.metadata.metrics.oldestMessageTimestamp instanceof Date,
      'Expected oldestMessageTimestamp to be a Date'
    );
    assert.strictEqual(
      sendResult.metadata.metrics.oldestMessageTimestamp.getTime(),
      1000000
    );

    assert.strictEqual(sendBatchResult.metadata.metrics.backlogCount, 200);
    assert.strictEqual(sendBatchResult.metadata.metrics.backlogBytes, 4096);
    assert.ok(
      sendBatchResult.metadata.metrics.oldestMessageTimestamp instanceof Date,
      'Expected oldestMessageTimestamp to be a Date'
    );
    assert.strictEqual(
      sendBatchResult.metadata.metrics.oldestMessageTimestamp.getTime(),
      2000000
    );
  },
};
