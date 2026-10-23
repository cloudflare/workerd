// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { Buffer } from 'node:buffer';

export default {
  async fetch(request) {
    assert.strictEqual(new URL(request.url).pathname, '/batch');
    const { messages } = await request.json();
    assert.strictEqual(messages.length, 2);
    assert.strictEqual(messages[0].contentType, 'bytes');
    assert.strictEqual(messages[1].contentType, 'json');
    const expected = JSON.parse(Buffer.from(messages[1].body, 'base64'));
    assert.deepStrictEqual(
      [...Buffer.from(messages[0].body, 'base64')],
      expected
    );
    return Response.json({
      metadata: {
        metrics: {
          backlogCount: 0,
          backlogBytes: 0,
          oldestMessageTimestamp: 0,
        },
      },
    });
  },

  async test(_ctrl, env) {
    for (const operation of [
      'detach',
      'mutate',
      'transferAndMutate',
      'resize',
    ]) {
      for (const dataView of [false, true]) {
        const resizable = operation === 'resize';
        const buffer = resizable
          ? new ArrayBuffer(32, { maxByteLength: 64 })
          : new ArrayBuffer(32);
        const bytes = new Uint8Array(buffer);
        for (let i = 0; i < bytes.length; i++) bytes[i] = i;
        // Only the selected view, including its nonzero offset, is sent.
        const view = dataView
          ? new DataView(buffer, 8, 16)
          : new Uint8Array(buffer, 8, 16);
        const expected = [...bytes.slice(8, 24)];
        let serialized = false;
        await env.QUEUE.sendBatch([
          { body: view, contentType: 'bytes' },
          {
            contentType: 'json',
            body: {
              toJSON() {
                assert.strictEqual(buffer.detached, false);
                serialized = true;
                if (operation === 'detach') {
                  buffer.transfer(0);
                } else if (operation === 'resize') {
                  buffer.resize(0);
                } else {
                  // Fixed stores alias mutations by later messages, even through transfer.
                  const target =
                    operation === 'transferAndMutate'
                      ? new Uint8Array(buffer.transfer())
                      : bytes;
                  target.fill(0xa5, 8, 24);
                  expected.fill(0xa5);
                }
                return expected;
              },
            },
          },
        ]);
        assert.strictEqual(serialized, true);
        assert.strictEqual(
          buffer.detached,
          operation === 'detach' || operation === 'transferAndMutate'
        );
        if (operation === 'mutate') {
          // sendBatch does not detach or take ownership of the caller's buffer.
          bytes.fill(0x5a);
          await env.QUEUE.sendBatch([
            { body: view, contentType: 'bytes' },
            { body: Array(16).fill(0x5a), contentType: 'json' },
          ]);
          assert.strictEqual(buffer.detached, false);
        }
      }
    }

    // Empty and already-detached views encode as empty byte messages.
    const detached = new Uint8Array(16);
    detached.buffer.transfer(0);
    for (const body of [new Uint8Array(0), detached]) {
      await env.QUEUE.sendBatch([
        { body, contentType: 'bytes' },
        { body: [], contentType: 'json' },
      ]);
    }
  },
};
