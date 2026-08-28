// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// FixedLengthStream happy paths: writing exactly expectedLength bytes, and
// the interaction between expectedLength and the highWaterMark option.

import { strictEqual } from 'node:assert';

export const exactLengthSingleWrite = {
  async test() {
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    const readPromise = reader.read();
    await writer.write(new TextEncoder().encode('hello'));
    await writer.close();
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'hello');
    const tail = await reader.read();
    strictEqual(tail.done, true);
  },
};

export const exactLengthTwoWrites = {
  async test() {
    const fls = new FixedLengthStream(6);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    const w1 = writer.write(new Uint8Array([1, 2, 3]));
    const r1 = await reader.read();
    await w1;
    strictEqual(r1.value.byteLength, 3);
    const w2 = writer.write(new Uint8Array([4, 5, 6]));
    const r2 = await reader.read();
    await w2;
    strictEqual(r2.value.byteLength, 3);
    await writer.close();
    const tail = await reader.read();
    strictEqual(tail.done, true);
  },
};

export const zeroLengthStreamClosesCleanly = {
  async test() {
    // FixedLengthStream(0) means "closes without delivering any bytes".
    const fls = new FixedLengthStream(0);
    const writer = fls.writable.getWriter();
    await writer.close();
    const reader = fls.readable.getReader();
    const { done } = await reader.read();
    strictEqual(done, true);
  },
};

export const highWaterMarkCappedAtExpectedLength = {
  test() {
    const fls = new FixedLengthStream(10, { highWaterMark: 100 });
    strictEqual(fls.writable.getWriter().desiredSize, 10);
  },
};

export const highWaterMarkKeptWhenSmaller = {
  test() {
    const fls = new FixedLengthStream(100, { highWaterMark: 5 });
    strictEqual(fls.writable.getWriter().desiredSize, 5);
  },
};

export const highWaterMarkCappedWithBigintLength = {
  test() {
    const fls = new FixedLengthStream(10n, { highWaterMark: 100 });
    strictEqual(fls.writable.getWriter().desiredSize, 10);
  },
};

export const cappedHighWaterMarkWithDataFlow = {
  async test() {
    const data = 'hello world, padding'; // exactly 20 bytes of ASCII
    const fls = new FixedLengthStream(data.length, { highWaterMark: 50 });
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    strictEqual(writer.desiredSize, data.length);
    const readPromise = reader.read();
    await writer.write(data);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), data);
    await writer.close();
  },
};
