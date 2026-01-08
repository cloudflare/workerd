// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, ok } from 'node:assert';

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

// Test IdentityTransformStream properly handles readAtLeast
export const identityTransformStreamReadAtLeast = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();

    const reader = readable.getReader({ mode: 'byob' });
    const writer = writable.getWriter();

    const expectedReads = [100, 100, 1, 0];

    async function consume(reader) {
      const res = await reader.readAtLeast(100, new Uint8Array(100));
      if (!res.done) {
        strictEqual(res.value.byteLength, expectedReads.shift());
        return consume(reader);
      }
    }

    await Promise.all([
      consume(reader),
      writer.write(new Uint8Array(100)),
      writer.write(new Uint8Array(1)),
      writer.write(new Uint8Array(100)),
      writer.close(),
    ]);
  },
};

// Test BYOB readAtLeast partially filled
export const partiallyFilledByobAtLeast = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const reader = readable.getReader({ mode: 'byob' });
    const rs = new ReadableStream({
      type: 'bytes',
      async pull(controller) {
        const chunk = await reader.readAtLeast(100, new Uint8Array(100));
        if (!chunk.done) {
          controller.enqueue(chunk.value);
        } else {
          controller.close();
        }
      },
    });

    async function consume(readable) {
      let ab = new ArrayBuffer(102);
      const dec = new TextDecoder();
      let ret = '';
      const reader = readable.getReader({ mode: 'byob' });
      for (;;) {
        const read = await reader.readAtLeast(102, new Uint8Array(ab));
        if (!read.done) {
          ret += dec.decode(read.value);
          ab = read.value.buffer;
          continue;
        } else {
          break;
        }
      }
      strictEqual(ret, 'hello'.repeat(1000));
      return ret.length;
    }

    const p = consume(rs);

    const enc = new TextEncoder();
    const writer = writable.getWriter();
    writer.write(enc.encode('hello'.repeat(1000)));
    writer.close();

    strictEqual(await p, 5000);
  },
};

// Test BYOB readAtLeast with tee
export const byobReadAtLeastTee = {
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
          strictEqual(c.byobRequest.atLeast, expectedAtLeasts.shift());
          enc.encodeInto(chunks.shift(), c.byobRequest.view);
          c.byobRequest.respond(5);
        }
      },
    });

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    const p1 = reader.readAtLeast(100, new Uint8Array(100));
    const p2 = reader2.readAtLeast(5, new Uint8Array(100));
    const p3 = reader3.readAtLeast(3, new Uint8Array(3));

    const res = await Promise.all([p1, p2, p3]);

    strictEqual(dec.decode(res[0].value), 'hellothere');
    strictEqual(dec.decode(res[1].value), 'hello');
    strictEqual(dec.decode(res[2].value), 'hel');

    const res2 = await reader2.readAtLeast(5, new Uint8Array(100));
    strictEqual(dec.decode(res2.value), 'there');

    const res3 = await reader3.readAtLeast(4, new Uint8Array(4));
    strictEqual(dec.decode(res3.value), 'loth');

    const res4 = await reader.readAtLeast(100, new Uint8Array(100));
    strictEqual(res4.done, true);

    const res5 = await reader2.readAtLeast(5, new Uint8Array(100));
    strictEqual(res5.done, true);

    const res6 = await reader3.readAtLeast(4, new Uint8Array(4));
    strictEqual(dec.decode(res6.value), 'ere');

    const res7 = await reader2.readAtLeast(5, new Uint8Array(100));
    strictEqual(res7.done, true);
  },
};

// Test BYOB readAtLeast with tee complex variant 1
export const byobReadAtLeastTeeComplex1 = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = ['helloth', 'ere'];
    let previousByobRequest;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        const req = c.byobRequest;
        if (chunks.length === 0) {
          c.close();
          req.respond(0);
        } else {
          ok(!(req === previousByobRequest));
          const chunk = chunks.shift();
          enc.encodeInto(chunk, req.view);
          req.respond(chunk.length);
        }
      },
    });

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    const res1 = await reader1.readAtLeast(5, new Uint8Array(10));
    strictEqual(dec.decode(res1.value), 'helloth');
    const res2 = await reader2.readAtLeast(10, new Uint8Array(10));
    strictEqual(dec.decode(res2.value), 'hellothere');

    const res3 = await reader1.readAtLeast(5, new Uint8Array(10));
    strictEqual(dec.decode(res3.value), 'ere');

    const res4 = await reader3.readAtLeast(2, new Uint8Array(12));
    strictEqual(dec.decode(res4.value), 'hellothere');
  },
};

// Test BYOB readAtLeast with tee complex variant 2
export const byobReadAtLeastTeeComplex2 = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = ['helloth', 'ere'];
    let previousByobRequest;
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (chunks.length === 0) {
          c.close();
          c.byobRequest.respond(0);
        } else {
          ok(!(c.byobRequest === previousByobRequest));
          const chunk = chunks.shift();
          enc.encodeInto(chunk, c.byobRequest.view);
          c.byobRequest.respond(chunk.length);
        }
      },
    });

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    const res1 = await reader1.readAtLeast(5, new Uint8Array(10));
    strictEqual(dec.decode(res1.value), 'helloth');
    const res2 = await reader2.readAtLeast(10, new Uint8Array(10));
    strictEqual(dec.decode(res2.value), 'hellothere');

    const res3 = await reader1.readAtLeast(5, new Uint8Array(10));
    strictEqual(dec.decode(res3.value), 'ere');

    const res4 = await reader3.readAtLeast(2, new Uint8Array(12));
    strictEqual(dec.decode(res4.value), 'hellothere');
  },
};

// Test BYOB readAtLeast with tee complex variant 3 (typed arrays)
export const byobReadAtLeastTeeComplex3 = {
  async test() {
    const chunks = [
      new Uint8Array([0x01]),
      new Uint8Array([0x02]),
      new Uint8Array([0x03]),
      new Uint8Array([0x04]),
      new Uint8Array([0x05, 0x06]),
    ];

    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (chunks.length === 0) {
          c.close();
          c.byobRequest.respond(0);
        } else {
          const view = c.byobRequest.view;
          const chunk = chunks.shift();
          for (let n = 0; n < chunk.length; n++) {
            view[n] = chunk[n];
          }
          c.byobRequest.respond(chunk.length);
        }
      },
    });

    const [branch1, branch2] = rs.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });

    const [res1, res2, res3, res4] = await Promise.all([
      reader1.readAtLeast(2, new Uint16Array(2)),
      reader1.readAtLeast(2, new Uint8Array(2)),
      reader2.readAtLeast(2, new Uint8Array(2)),
      reader2.readAtLeast(1, new Uint32Array(1)),
    ]);

    strictEqual(res1.value instanceof Uint16Array, true);
    strictEqual(res2.value instanceof Uint8Array, true);
    strictEqual(res1.value[0], 0x0201);
    strictEqual(res1.value[1], 0x0403);
    strictEqual(res2.value[0], 0x05);
    strictEqual(res2.value[1], 0x06);

    strictEqual(res3.value instanceof Uint8Array, true);
    strictEqual(res4.value instanceof Uint32Array, true);
    strictEqual(res3.value[0], 0x1);
    strictEqual(res3.value[1], 0x2);
    strictEqual(res4.value[0], 0x06050403);
  },
};

export const requestCloneByob = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const chunks = [
      enc.encode('hello'),
      enc.encode('there'),
      enc.encode('!!!!!'),
    ];
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(chunks.shift());
        if (chunks.length === 0) c.close();
      },
    });

    const newRequest = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });
    const reader = newRequest.clone().body.getReader({ mode: 'byob' });

    strictEqual(
      dec.decode((await reader.readAtLeast(10, new Uint8Array(10))).value),
      'hellothere'
    );
  },
};

export const textDecoderStreamRequest = {
  async test() {
    const enc = new TextEncoder();
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });

    const request = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });

    const reader = request.body
      .pipeThrough(new TextDecoderStream('utf-8'))
      .getReader();
    strictEqual(typeof (await reader.read()).value, 'string');
  },
};
