// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

export default testTailHandler;

export const test = {
  async test() {
    await Promise.allSettled(invocationPromises);

    const received = Array.from(spans.values()).filter(
      (span) => span.name === 'queue_send'
    );
    assert.strictEqual(received.length, 5);

    const common = {
      name: 'queue_send',
      'cloudflare.binding.name': 'QUEUE',
      'cloudflare.binding.type': 'queue',
      'messaging.system': 'cloudflare.queues',
      'messaging.destination.name': 'queue-test',
      'messaging.operation.name': 'send',
      'messaging.operation.type': 'send',
      closed: true,
    };

    for (const span of received.slice(0, 4)) {
      const { ['messaging.message.body.size']: bodySize, ...attributes } = span;
      assert.deepStrictEqual(attributes, {
        ...common,
        'cloudflare.queue.operation': 'send',
      });
      assert.strictEqual(typeof bodySize, 'bigint');
    }

    assert.deepStrictEqual(received[4], {
      ...common,
      'cloudflare.queue.operation': 'sendBatch',
      'messaging.batch.message_count': 4n,
      'cloudflare.queue.batch.body.size': 31n,
      'cloudflare.queue.batch.largest_message.size': 13n,
    });
  },
};
