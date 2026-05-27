// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test: sendBatch with a resizable ArrayBuffer "bytes" message
// followed by a "json" message whose toJSON() resizes the buffer to 0.
// Before the fix, the shallow reference captured for the first message would
// read from decommitted pages when base64-encoding the batch body.

import assert from 'node:assert';
import { Buffer } from 'node:buffer';

export default {
  async fetch(request) {
    const { pathname } = new URL(request.url);
    if (pathname === '/batch') {
      const body = await request.json();
      assert(Array.isArray(body?.messages));
      assert.strictEqual(body.messages.length, 2);

      // The bytes message should contain the original data, not zeros.
      assert.strictEqual(body.messages[0].contentType, 'bytes');
      const bytes = Buffer.from(body.messages[0].body, 'base64');
      assert.strictEqual(bytes.length, 64);
      for (let i = 0; i < 64; i++) {
        assert.strictEqual(bytes[i], 0xaa, `byte ${i} should be 0xAA`);
      }

      // The json message should contain the hostile toJSON() result.
      assert.strictEqual(body.messages[1].contentType, 'json');
      assert.deepStrictEqual(
        JSON.parse(Buffer.from(body.messages[1].body, 'base64')),
        { poisoned: true }
      );
    }
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

  async test(ctrl, env, ctx) {
    // Create a resizable ArrayBuffer and fill with a known pattern.
    const rab = new ArrayBuffer(64, { maxByteLength: 128 });
    new Uint8Array(rab).fill(0xaa);
    const view = new Uint8Array(rab);

    // Craft a hostile object whose toJSON() shrinks the earlier message's buffer.
    const hostile = {
      toJSON() {
        rab.resize(0);
        return { poisoned: true };
      },
    };

    // sendBatch: first message holds a shallow reference to the resizable buffer,
    // second message's serialization runs toJSON() which resizes it to 0.
    // Pre-fix: OOB read / SIGSEGV in kj::encodeBase64.
    await env.QUEUE.sendBatch([
      { body: view, contentType: 'bytes' },
      { body: hostile, contentType: 'json' },
    ]);

    // sendBatch must not detach the buffer — users may reuse it across calls.
    assert.strictEqual(
      rab.detached,
      false,
      'sendBatch should not detach the ArrayBuffer'
    );
  },
};
