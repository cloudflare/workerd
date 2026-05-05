// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

export default {
  async queue(batch, env, ctx) {
    assert.ok(batch.metadata, 'Expected batch.metadata to be defined');
    assert.ok(
      batch.metadata.metrics,
      'Expected batch.metadata.metrics to be defined'
    );

    if (
      batch.metadata.metrics.backlogCount === 0 &&
      batch.metadata.metrics.backlogBytes === 0 &&
      batch.metadata.metrics.oldestMessageTimestamp === undefined
    ) {
      // If metadata is omitted → counts default to zero, timestamp is undefined
      batch.ackAll();
      return;
    }

    // Explicit metadata path
    assert.strictEqual(batch.metadata.metrics.backlogCount, 100);
    assert.strictEqual(batch.metadata.metrics.backlogBytes, 2048);
    assert.ok(
      batch.metadata.metrics.oldestMessageTimestamp instanceof Date,
      'Expected oldestMessageTimestamp to be a Date'
    );
    assert.strictEqual(
      batch.metadata.metrics.oldestMessageTimestamp.getTime(),
      1000000
    );
    batch.ackAll();
  },

  async test(ctrl, env, ctx) {
    const timestamp = new Date();

    const response1 = await env.SERVICE.queue(
      'test-queue',
      [{ id: '0', timestamp, body: 'hello', attempts: 1 }],
      {
        metrics: {
          backlogCount: 100,
          backlogBytes: 2048,
          oldestMessageTimestamp: 1000000,
        },
      }
    );
    assert.strictEqual(response1.outcome, 'ok');
    assert(response1.ackAll);

    // Test with omitted metadata
    const response2 = await env.SERVICE.queue('test-queue', [
      { id: '1', timestamp, body: 'world', attempts: 1 },
    ]);
    assert.strictEqual(response2.outcome, 'ok');
    assert(response2.ackAll);
  },
};
