// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';

// Test BYOB readAtLeast with automatic atLeast handling
export const byobReadAtLeastAutomatic = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = ['hello', 'there'];
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        // When using enqueue, the stream impl will take care of properly handling the
        // at least requirement...
        c.enqueue(enc.encode(chunks.shift()));
        if (chunks.length === 0) c.close();
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    const res = await reader.readAtLeast(100, new Uint8Array(100));

    strictEqual(dec.decode(res.value), 'hellothere');
  },
};

// Test BYOB readAtLeast with manual atLeast handling
export const byobReadAtLeastManual = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = ['hello', 'there'];
    const expectedAtLeasts = [100, 95];
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (chunks.length === 0) {
          c.close();
          c.byobRequest.respond(0);
        } else {
          // The respond() can partially fulfill the minRead requirement over
          // multiple calls to pull.
          strictEqual(c.byobRequest.atLeast, expectedAtLeasts.shift());

          enc.encodeInto(chunks.shift(), c.byobRequest.view);
          c.byobRequest.respond(5);
        }
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    const res = await reader.readAtLeast(100, new Uint8Array(100));

    strictEqual(dec.decode(res.value), 'hellothere');
  },
};

// Test IdentityTransformStream with readAtLeast incremental writes
export const identityTransformReadAtLeast = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const reader = readable.getReader({ mode: 'byob' });
    const writer = writable.getWriter();

    // There's been a latent bug in IdentityTransformStream ever since
    // readAtLeast was introduced that caused it to mishandle the atLeast
    // calculation when individual writes were < atLeast.

    for (let n = 0; n < 8; n++) {
      writer.write(new Uint8Array(1));
    }
    writer.write(new Uint8Array([0x1]));
    writer.write(new Uint8Array([0x2]));
    writer.write(new Uint8Array([0x3]));
    writer.write(new Uint8Array([0x4]));

    const res = await reader.readAtLeast(8, new Uint8Array(8));
    strictEqual(res.value.byteLength, 8);

    const res2 = await reader.readAtLeast(2, new Uint8Array(4));
    const res3 = await reader.readAtLeast(2, new Uint8Array(4));

    strictEqual(res2.value.byteLength, 2);
    strictEqual(res2.value[0], 0x1);
    strictEqual(res2.value[1], 0x2);

    strictEqual(res3.value.byteLength, 2);
    strictEqual(res3.value[0], 0x3);
    strictEqual(res3.value[1], 0x4);
  },
};

// Test FixedLengthStream with readAtLeast incremental writes
export const fixedLengthStreamReadAtLeast = {
  async test() {
    const { readable, writable } = new FixedLengthStream(12);

    const reader = readable.getReader({ mode: 'byob' });
    const writer = writable.getWriter();

    // There's been a latent bug in IdentityTransformStream ever since
    // readAtLeast was introduced that caused it to mishandle the atLeast
    // calculation when individual writes were < atLeast.

    for (let n = 0; n < 8; n++) {
      writer.write(new Uint8Array(1));
    }
    writer.write(new Uint8Array([0x1]));
    writer.write(new Uint8Array([0x2]));
    writer.write(new Uint8Array([0x3]));
    writer.write(new Uint8Array([0x4]));

    const res = await reader.readAtLeast(8, new Uint8Array(8));
    strictEqual(res.value.byteLength, 8);

    const res2 = await reader.readAtLeast(2, new Uint8Array(4));
    const res3 = await reader.readAtLeast(2, new Uint8Array(4));

    strictEqual(res2.value.byteLength, 2);
    strictEqual(res2.value[0], 0x1);
    strictEqual(res2.value[1], 0x2);

    strictEqual(res3.value.byteLength, 2);
    strictEqual(res3.value[0], 0x3);
    strictEqual(res3.value[1], 0x4);
  },
};

// Test BYOB stream tee closed on start with waitUntil
// Tests that a teed BYOB stream that closes immediately after enqueuing
// still works correctly when one branch is consumed via waitUntil
export const closedByobTeeOnStart = {
  async test(ctrl, env, ctx) {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    async function consume(rs) {
      const reader = rs.getReader({ mode: 'byob' });
      let result = '';
      for (;;) {
        const res = await reader.readAtLeast(10, new Uint8Array(10));
        if (res.done) break;
        result += dec.decode(res.value, { stream: true });
      }
      result += dec.decode();
      if (result !== 'hello') throw new Error('Incorrect result in branch');
      return result;
    }

    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const [b1, b2] = rs.tee();

    const branch2Promise = consume(b2);
    ctx.waitUntil(branch2Promise);

    const result1 = await consume(b1);
    strictEqual(result1, 'hello');

    const result2 = await branch2Promise;
    strictEqual(result2, 'hello');
  },
};
