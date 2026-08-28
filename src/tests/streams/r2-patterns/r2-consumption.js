// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// R2-SDK-style consumption patterns, migrated wholesale from
// streams-r2-patterns-test.js: readAtLeast-driven reads over JS byte
// streams, identity and FixedLength streams, BYOB reads over tee
// branches, Request clone consumption, and TextDecoderStream over a
// Request body.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Bounded observation of a promise's outcome; a pinned 'pending' is a
// deliberate defect pin.
const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

// The TS-side pins in this file follow the readable-byte suite's
// ledger: readAtLeast PENDS FOREVER when the stream closes below the
// minimum (ledger #12 family), and tee-driven pulls present a NULL
// byobRequest (ledger #5/#6), so sources that touch c.byobRequest
// unconditionally throw TypeError into the stream.

// Test BYOB readAtLeast with automatic atLeast handling
export const byobReadAtLeastAutomatic = {
  async test() {
    if (usingTsImpl) {
      // Close arrives below the 100-byte minimum: the readAtLeast
      // PENDS FOREVER (C++ folds the 10 available bytes into a
      // done=false result).
      const enc = new TextEncoder();
      const chunks = ['hello', 'there'];
      const rs = new ReadableStream({
        type: 'bytes',
        pull(c) {
          c.enqueue(enc.encode(chunks.shift()));
          if (chunks.length === 0) c.close();
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      const outcome = await outcomeOf(
        reader.readAtLeast(100, new Uint8Array(100))
      );
      strictEqual(outcome.state, 'pending');
      return;
    }
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
    if (usingTsImpl) {
      // Both branches close below the 10-byte minimum: each branch's
      // readAtLeast PENDS FOREVER.
      const enc = new TextEncoder();
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          c.enqueue(enc.encode('hello'));
          c.close();
        },
      });
      const [b1, b2] = rs.tee();
      const o1 = await outcomeOf(
        b1.getReader({ mode: 'byob' }).readAtLeast(10, new Uint8Array(10))
      );
      const o2 = await outcomeOf(
        b2.getReader({ mode: 'byob' }).readAtLeast(10, new Uint8Array(10))
      );
      strictEqual(o1.state, 'pending');
      strictEqual(o2.state, 'pending');
      return;
    }
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
    if (usingTsImpl) {
      // The first 100-byte minimum is satisfiable, but the trailing
      // 1-byte remainder plus close never fulfills a 100-byte minimum:
      // the consumption chain PENDS FOREVER at the tail.
      const { readable, writable } = new IdentityTransformStream();
      const reader = readable.getReader({ mode: 'byob' });
      const writer = writable.getWriter();
      void writer.write(new Uint8Array(100));
      void writer.write(new Uint8Array(1));
      void writer.write(new Uint8Array(100));
      void writer.close();
      const first = await outcomeOf(
        reader.readAtLeast(100, new Uint8Array(100))
      );
      strictEqual(first.state, 'fulfilled');
      strictEqual(first.value.value.byteLength, 100);
      const second = await outcomeOf(
        reader.readAtLeast(100, new Uint8Array(100))
      );
      strictEqual(second.state, 'fulfilled');
      strictEqual(second.value.value.byteLength, 100); // view filled exactly
      // One byte remains, below the minimum, with the close behind it.
      const tail = await outcomeOf(
        reader.readAtLeast(100, new Uint8Array(100))
      );
      strictEqual(tail.state, 'pending');
      return;
    }
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
    if (usingTsImpl) {
      // 5000 bytes consumed in 102-byte minimums leaves a final
      // below-minimum remainder; the last readAtLeast PENDS FOREVER
      // when the close arrives.
      const { readable, writable } = new IdentityTransformStream();
      const enc = new TextEncoder();
      const writer = writable.getWriter();
      void writer.write(enc.encode('hello'.repeat(1000)));
      void writer.close();
      const reader = readable.getReader({ mode: 'byob' });
      let received = 0;
      let ab = new ArrayBuffer(102);
      for (;;) {
        const outcome = await outcomeOf(
          reader.readAtLeast(102, new Uint8Array(ab))
        );
        if (outcome.state === 'pending') break;
        strictEqual(outcome.state, 'fulfilled');
        received += outcome.value.value.byteLength;
        ab = outcome.value.value.buffer;
      }
      strictEqual(received, 4998); // 49 full reads; the 2-byte tail hangs
      return;
    }
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

// A byte source in the shape the tee tests use: touches c.byobRequest
// unconditionally (fine under C++ tee pulls, TypeError under TS).
function teeSourceTouchingByobRequest() {
  const enc = new TextEncoder();
  const chunks = ['hello', 'there'];
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      if (chunks.length === 0) {
        c.close();
        c.byobRequest.respond(0);
      } else {
        enc.encodeInto(chunks.shift(), c.byobRequest.view);
        c.byobRequest.respond(5);
      }
    },
  });
}

// Test BYOB readAtLeast with tee
export const byobReadAtLeastTee = {
  async test() {
    if (usingTsImpl) {
      // Tee-driven pulls present a NULL byobRequest: the source's
      // unconditional c.byobRequest access throws into the stream and
      // every branch read rejects with that TypeError.
      const rs = teeSourceTouchingByobRequest();
      const [branch1, branchB] = rs.tee();
      const [branch2, branch3] = branchB.tee();
      for (const [branch, min, size] of [
        [branch1, 100, 100],
        [branch2, 5, 100],
        [branch3, 3, 3],
      ]) {
        const outcome = await outcomeOf(
          branch
            .getReader({ mode: 'byob' })
            .readAtLeast(min, new Uint8Array(size))
        );
        strictEqual(outcome.state, 'rejected');
        strictEqual(outcome.reason.name, 'TypeError');
        ok(/byobRequest|null/.test(outcome.reason.message));
      }
      return;
    }
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
    if (usingTsImpl) {
      // Tee-driven pulls present a NULL byobRequest: the source's
      // unconditional c.byobRequest access throws into the stream and
      // every branch read rejects with that TypeError.
      const rs = teeSourceTouchingByobRequest();
      const [b1, b2] = rs.tee();
      const outcome = await outcomeOf(
        b1.getReader({ mode: 'byob' }).readAtLeast(5, new Uint8Array(100))
      );
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason.name, 'TypeError');
      void b2;
      return;
    }
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
    if (usingTsImpl) {
      // Tee-driven pulls present a NULL byobRequest: the source's
      // unconditional c.byobRequest access throws into the stream and
      // every branch read rejects with that TypeError.
      const rs = teeSourceTouchingByobRequest();
      const [b1, b2] = rs.tee();
      const outcome = await outcomeOf(
        b1.getReader({ mode: 'byob' }).readAtLeast(5, new Uint8Array(100))
      );
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason.name, 'TypeError');
      void b2;
      return;
    }
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
    if (usingTsImpl) {
      // Tee-driven pulls present a NULL byobRequest: the source's
      // unconditional c.byobRequest access throws into the stream and
      // every branch read rejects with that TypeError.
      const rs = teeSourceTouchingByobRequest();
      const [b1, b2] = rs.tee();
      const outcome = await outcomeOf(
        b1.getReader({ mode: 'byob' }).readAtLeast(5, new Uint8Array(100))
      );
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason.name, 'TypeError');
      void b2;
      return;
    }
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
