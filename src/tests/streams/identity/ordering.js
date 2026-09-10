// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk ordering and 1:1 write/read correspondence, in both interleavings:
// all writes queued before any read, and each read pending before its write.

import { strictEqual, deepStrictEqual } from 'node:assert';

const COUNT = 10;

export const writesBeforeReads = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromises = [];
    for (let i = 0; i < COUNT; i++) {
      writePromises.push(writer.write(new Uint8Array([i])));
    }
    const chunks = [];
    for (let i = 0; i < COUNT; i++) {
      chunks.push(await reader.read());
    }
    await Promise.all(writePromises);
    for (let i = 0; i < COUNT; i++) {
      strictEqual(chunks[i].done, false);
      deepStrictEqual([...chunks[i].value], [i]);
    }
    const closePromise = writer.close();
    const tail = await reader.read();
    strictEqual(tail.done, true);
    await closePromise;
    await writer.closed;
    await reader.closed;
  },
};

export const readsBeforeWrites = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const chunks = [];
    for (let i = 0; i < COUNT; i++) {
      const readPromise = reader.read();
      await writer.write(new Uint8Array([i]));
      chunks.push(await readPromise);
    }
    for (let i = 0; i < COUNT; i++) {
      strictEqual(chunks[i].done, false);
      deepStrictEqual([...chunks[i].value], [i]);
    }
    const readPromise = reader.read();
    await writer.close();
    const tail = await readPromise;
    strictEqual(tail.done, true);
    await writer.closed;
    await reader.closed;
  },
};

export const aggregateContentPreserved = {
  async test() {
    // Total content is preserved across many chunk boundaries regardless of
    // how reads slice it.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const N = 4;
    const M = 1000;
    const writePromise = (async () => {
      for (let i = 0; i < N; i++) {
        const chunk = new Uint8Array(M);
        chunk.fill(i + 1);
        await writer.write(chunk);
      }
      await writer.close();
    })();
    const chunks = [];
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    const total = chunks.reduce((sum, c) => sum + c.byteLength, 0);
    strictEqual(total, N * M);
    const body = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      body.set(chunk, offset);
      offset += chunk.byteLength;
    }
    for (let i = 0; i < body.length; i++) {
      strictEqual(body[i], Math.floor(i / M) + 1);
    }
    await writePromise;
  },
};
