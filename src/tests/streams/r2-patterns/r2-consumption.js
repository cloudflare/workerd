// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// R2-SDK-style consumption patterns, migrated wholesale from
// streams-r2-patterns-test.js: readAtLeast-driven reads over JS byte
// streams, identity and FixedLength streams, BYOB reads over tee
// branches, Request clone consumption, and TextDecoderStream over a
// Request body.

import { strictEqual, ok } from 'node:assert';

// Bounded observation of a promise's outcome, so a regression back to a
// hang fails the assertion instead of wedging the test.
const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

// Close-below-min follows the DECIDED readAtLeast tail contract on both
// implementations: the available bytes are folded into a done=false
// result, and a follow-up read resolves done. Tee-driven pulls under
// TypeScript present a NULL byobRequest (readable-byte ledger #5/#6),
// so sources that touch c.byobRequest unconditionally throw TypeError
// into the stream — the dual-path source below is the supported shape.

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

    // Close arrives below the 100-byte minimum: the available 10 bytes
    // fold into a done=false result (the DECIDED tail contract, parity).
    const res = await reader.readAtLeast(100, new Uint8Array(100));

    strictEqual(res.done, false);
    strictEqual(dec.decode(res.value), 'hellothere');

    const tail = await reader.readAtLeast(4, new Uint8Array(4));
    strictEqual(tail.done, true);
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

// The SUPPORTED tee source pattern (decided 2026-08-28): tee-driven
// pulls under the TypeScript shared-queue model present a NULL
// byobRequest, so sources must be null-tolerant — fill the request when
// present (the C++ path), enqueue() otherwise. Sources that touch
// c.byobRequest unconditionally are INCORRECT under tee.
function dualPathTeeSource(chunks, onByobRequest) {
  const pending = [...chunks];
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      if (pending.length === 0) {
        c.close();
        c.byobRequest?.respond(0);
        return;
      }
      const chunk = pending.shift();
      if (c.byobRequest) {
        onByobRequest?.(c.byobRequest);
        c.byobRequest.view.set(chunk);
        c.byobRequest.respond(chunk.length);
      } else {
        c.enqueue(chunk.slice());
      }
    },
  });
}

// Test BYOB readAtLeast with tee, using the supported null-tolerant
// source pattern. C++ tee pulls carry a byobRequest (atLeast asserted);
// TypeScript tee pulls enqueue.
export const byobReadAtLeastTee = {
  async test() {
    const dec = new TextDecoder();
    const expectedAtLeasts = [100, 95];
    const rs = dualPathTeeSource(
      ['hello', 'there'].map((t) => new TextEncoder().encode(t)),
      (req) => {
        strictEqual(req.atLeast, expectedAtLeasts.shift());
      }
    );

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    // PARITY with the null-tolerant source: all three minimums deliver
    // the same bytes on both implementations (the min-100 read collects
    // the full 10 available bytes at close — the below-min tail-shape
    // done flag is pinned in the complex variants).
    const p1 = outcomeOf(reader.readAtLeast(100, new Uint8Array(100)));
    const p2 = outcomeOf(reader2.readAtLeast(5, new Uint8Array(100)));
    const p3 = outcomeOf(reader3.readAtLeast(3, new Uint8Array(3)));
    const [o1, o2, o3] = await Promise.all([p1, p2, p3]);
    strictEqual(o1.state, 'fulfilled');
    strictEqual(dec.decode(o1.value.value), 'hellothere');
    strictEqual(o2.state, 'fulfilled');
    strictEqual(dec.decode(o2.value.value), 'hello');
    strictEqual(o3.state, 'fulfilled');
    strictEqual(dec.decode(o3.value.value), 'hel');
  },
};

// Complex variant 1: staggered reads across three branches with a
// null-tolerant source ('helloth' + 'ere').
export const byobReadAtLeastTeeComplex1 = {
  async test() {
    const dec = new TextDecoder();
    const enc = new TextEncoder();
    const rs = dualPathTeeSource([enc.encode('helloth'), enc.encode('ere')]);

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    const r1 = await outcomeOf(reader1.readAtLeast(5, new Uint8Array(10)));
    const r2 = await outcomeOf(reader2.readAtLeast(10, new Uint8Array(10)));
    const r3 = await outcomeOf(reader1.readAtLeast(5, new Uint8Array(10)));
    const r4 = await outcomeOf(reader3.readAtLeast(2, new Uint8Array(12)));
    strictEqual(r1.state, 'fulfilled');
    strictEqual(dec.decode(r1.value.value), 'helloth');
    strictEqual(r1.value.done, false);
    strictEqual(r2.state, 'fulfilled');
    strictEqual(dec.decode(r2.value.value), 'hellothere');
    strictEqual(r2.value.done, false);
    // The below-min tail at close is delivered done=false (a separate
    // zero-length done read follows) — the DECIDED contract, parity.
    strictEqual(r3.state, 'fulfilled');
    strictEqual(dec.decode(r3.value.value), 'ere');
    strictEqual(r3.value.done, false);
    strictEqual(r4.state, 'fulfilled');
    strictEqual(dec.decode(r4.value.value), 'hellothere');
    strictEqual(r4.value.done, false);
  },
};

// Complex variant 2: as variant 1, with byobRequest freshness asserted
// per C++ pull.
export const byobReadAtLeastTeeComplex2 = {
  async test() {
    const dec = new TextDecoder();
    const enc = new TextEncoder();
    let previousByobRequest;
    const rs = dualPathTeeSource(
      [enc.encode('helloth'), enc.encode('ere')],
      (req) => {
        ok(req !== previousByobRequest);
        previousByobRequest = req;
      }
    );

    const [branch1, branchB] = rs.tee();
    const [branch2, branch3] = branchB.tee();

    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });
    const reader3 = branch3.getReader({ mode: 'byob' });

    const r1 = await outcomeOf(reader1.readAtLeast(5, new Uint8Array(10)));
    const r2 = await outcomeOf(reader2.readAtLeast(10, new Uint8Array(10)));
    const r3 = await outcomeOf(reader1.readAtLeast(5, new Uint8Array(10)));
    const r4 = await outcomeOf(reader3.readAtLeast(2, new Uint8Array(12)));
    strictEqual(r1.state, 'fulfilled');
    strictEqual(dec.decode(r1.value.value), 'helloth');
    strictEqual(r1.value.done, false);
    strictEqual(r2.state, 'fulfilled');
    strictEqual(dec.decode(r2.value.value), 'hellothere');
    strictEqual(r2.value.done, false);
    // The below-min tail at close is delivered done=false (a separate
    // zero-length done read follows) — the DECIDED contract, parity.
    strictEqual(r3.state, 'fulfilled');
    strictEqual(dec.decode(r3.value.value), 'ere');
    strictEqual(r3.value.done, false);
    strictEqual(r4.state, 'fulfilled');
    strictEqual(dec.decode(r4.value.value), 'hellothere');
    strictEqual(r4.value.done, false);
  },
};

// Complex variant 3: mixed view types (Uint16/Uint8/Uint32) across two
// branches over five small chunks.
export const byobReadAtLeastTeeComplex3 = {
  async test() {
    const rs = dualPathTeeSource([
      new Uint8Array([0x01]),
      new Uint8Array([0x02]),
      new Uint8Array([0x03]),
      new Uint8Array([0x04]),
      new Uint8Array([0x05, 0x06]),
    ]);

    const [branch1, branch2] = rs.tee();
    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader({ mode: 'byob' });

    const [o1, o2, o3, o4] = await Promise.all([
      outcomeOf(reader1.readAtLeast(2, new Uint16Array(2))),
      outcomeOf(reader1.readAtLeast(2, new Uint8Array(2))),
      outcomeOf(reader2.readAtLeast(2, new Uint8Array(2))),
      outcomeOf(reader2.readAtLeast(1, new Uint32Array(1))),
    ]);
    // PARITY: mixed view types deliver identical assemblies.
    for (const [o, Ctor, expected] of [
      [o1, Uint16Array, [513, 1027]],
      [o2, Uint8Array, [5, 6]],
      [o3, Uint8Array, [1, 2]],
      [o4, Uint32Array, [100992003]],
    ]) {
      strictEqual(o.state, 'fulfilled');
      strictEqual(o.value.value instanceof Ctor, true);
      strictEqual(Array.from(o.value.value).join(','), expected.join(','));
    }
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
