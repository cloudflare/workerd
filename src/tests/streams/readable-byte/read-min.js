// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The read-minimum machinery: the standard read(view, {min}) option and
// the workerd readAtLeast(min, view) extension (implemented by BOTH
// sides). The WPT read-min.any suite is disabled for hangs; the
// close-below-min divergence pinned here is the underlying reason.

import { strictEqual, ok, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

function byteStream(source = {}) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
    ...source,
  });
  return { rs, controller: () => controller };
}

// read(view, {min}) stays pending until min bytes accumulate across
// enqueues, then fulfills with everything available (parity; migrated
// byobMin shape from streams-test.js).
export const byobMin = {
  async test() {
    const { rs, controller } = byteStream();
    async function handleRead() {
      const reader = rs.getReader({ mode: 'byob' });
      const result = await reader.read(new Uint8Array(10), { min: 10 });
      strictEqual(result.done, false);
      strictEqual(result.value.byteLength, 10);
    }
    async function handlePush() {
      for (let n = 0; n < 10; n++) {
        controller().enqueue(new Uint8Array(1));
        await scheduler.wait(10);
      }
    }
    const results = await Promise.allSettled([handleRead(), handlePush()]);
    strictEqual(results[0].status, 'fulfilled');
    strictEqual(results[1].status, 'fulfilled');
  },
};

// Below-min bytes leave the read pending; it fulfills as soon as the
// enqueue that crosses min lands, delivering ALL buffered bytes
// (parity).
export const readMinStagedFulfillment = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(10), { min: 3 });
    await scheduler.wait(5);
    controller().enqueue(new Uint8Array([1, 2]));
    await scheduler.wait(5);
    const early = await Promise.race([
      read.then(() => 'settled'),
      scheduler.wait(50).then(() => 'pending'),
    ]);
    strictEqual(early, 'pending');
    controller().enqueue(new Uint8Array([3, 4]));
    const { value, done } = await read;
    strictEqual(done, false);
    strictEqual(value.byteLength, 4);
    strictEqual(value[3], 4);
  },
};

// min validation: zero rejects TypeError on both sides; min larger than
// the view rejects TypeError under C++ but RangeError under TypeScript
// (messages pinned).
export const readMinValidation = {
  async test() {
    const { rs } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    await rejects(reader.read(new Uint8Array(4), { min: 0 }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'options.min must be greater than 0'
        : 'Requested invalid minimum number of bytes to read (0).',
    });
    reader.releaseLock();
    const reader2 = rs.getReader({ mode: 'byob' });
    await rejects(reader2.read(new Uint8Array(4), { min: 5 }), {
      name: usingTsImpl ? 'RangeError' : 'TypeError',
      message: usingTsImpl
        ? 'options.min must not exceed the length of the view'
        : 'Minimum bytes to read (5) exceeds size of buffer (4).',
    });
    reader2.releaseLock();
    const reader3 = rs.getReader({ mode: 'byob' });
    await rejects(reader3.readAtLeast(0, new Uint8Array(4)), {
      name: 'TypeError',
    });
    reader3.releaseLock();
  },
};

// DIVERGENCE (the WPT read-min disable root): close() while a min-read
// holds SOME bytes (2 of 3). C++ fulfills the read with the partial
// bytes and done=false; TypeScript leaves the read PENDING FOREVER
// while reader.closed fulfills (bounded observation). The spec calls
// for a TypeError — neither side conforms.
export const closeBelowMin = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(10), { min: 3 });
    await scheduler.wait(5);
    controller().enqueue(new Uint8Array([1, 2]));
    await scheduler.wait(5);
    controller().close();
    strictEqual(await reader.closed, undefined);
    if (usingTsImpl) {
      const outcome = await Promise.race([
        read.then(() => 'settled'),
        scheduler.wait(100).then(() => 'pending'),
      ]);
      strictEqual(outcome, 'pending');
    } else {
      const { value, done } = await read;
      strictEqual(done, false);
      strictEqual(value.byteLength, 2);
      strictEqual(value[0], 1);
      strictEqual(value[1], 2);
    }
  },
};

// A read whose min is met fulfills immediately without waiting for the
// view to fill; close() afterwards succeeds (parity).
export const minMetThenClose = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(10), { min: 2 });
    await scheduler.wait(5);
    controller().enqueue(new Uint8Array([1, 2]));
    const { value, done } = await read;
    strictEqual(done, false);
    strictEqual(value.byteLength, 2);
    controller().close();
  },
};

// A default reader ignores a {min}-shaped argument (parity), and its
// readAtLeast() throws TypeError (migrated from streams-test.js).
export const readAtLeastDefaultReaderThrows = {
  async test() {
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });
    const reader = rs.getReader();
    throws(() => reader.readAtLeast(1), TypeError);
    const first = await reader.read({ min: 3 });
    strictEqual(first.done, false);
    strictEqual(first.value.byteLength, 5);
    reader.releaseLock();
    // Consume the rest to clean up.
    for await (const _ of rs) {
      // intentionally empty
    }
  },
};

// BYOB reader constraint validation against a native (Response) body
// (migrated from streams-test.js).
export const byobReaderConstraints = {
  async test() {
    const response = new Response('foo bar');
    const reader = response.body.getReader({ mode: 'byob' });
    reader.read(new Uint8Array(32)).catch(() => {});
    // Cannot BYOB with a zero-length buffer.
    await rejects(async () => reader.read(new Uint8Array(0)), TypeError);
    // Cannot BYOB an ArrayBuffer, only an ArrayBufferView.
    await rejects(async () => reader.read(new ArrayBuffer(32)), TypeError);
    // Cannot use a BYOB reader as a non-BYOB reader.
    await rejects(async () => reader.read(), TypeError);
  },
};

// readAtLeast() across a real fetch response (the SELF /chunked
// endpoint; migrated from streams-test.js): waits across chunk
// boundaries, returns early at stream end, and resolves done with an
// empty view (the pinned internal_stream_byob_return_view behavior).
export const readAtLeastByobReader = {
  async test(ctrl, env) {
    const response = await env.SELF.fetch('http://test/chunked');
    const reader = response.body.getReader({ mode: 'byob' });
    // Server chunks: foo, bar, b, a, z.
    let result = await reader.readAtLeast(4, new Uint8Array(20));
    let value = new TextDecoder().decode(result.value);
    strictEqual(result.done, false);
    strictEqual(value, 'foobar');
    result = await reader.read(new Uint8Array(20));
    value = new TextDecoder().decode(result.value);
    strictEqual(value, 'b');
    strictEqual(result.done, false);
    result = await reader.readAtLeast(4, new Uint8Array(20));
    value = new TextDecoder().decode(result.value);
    strictEqual(value, 'az');
    // DIVERGENCE in the end-of-stream shape: TypeScript reports done
    // together with the final below-min bytes; C++ returns them
    // done=false and requires one more read, which resolves done with
    // an empty view (the pinned internal_stream_byob_return_view
    // behavior).
    strictEqual(result.done, usingTsImpl);
    if (!usingTsImpl) {
      result = await reader.readAtLeast(4, new Uint8Array(20));
      strictEqual(result.done, true);
      ok(result.value instanceof Uint8Array);
      strictEqual(result.value.byteLength, 0);
    }
  },
};
