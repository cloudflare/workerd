// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Compress/decompress round trips across the three supported formats.
// Writes settle without read demand (eager codec push), so the sequential
// write-then-read form is safe.

import { strictEqual, deepStrictEqual, ok } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

export async function readAll(readable) {
  const chunks = [];
  for await (const chunk of readable) {
    chunks.push(chunk);
  }
  return chunks;
}

export function concat(chunks) {
  const total = chunks.reduce((sum, c) => sum + c.byteLength, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return out;
}

// Writes the chunks, closes, drains the readable; returns the bytes.
export async function pump(pair, chunks) {
  const writer = pair.writable.getWriter();
  for (const chunk of chunks) {
    await writer.write(chunk);
  }
  await writer.close();
  return concat(await readAll(pair.readable));
}

export const allFormatsRoundTrip = {
  async test() {
    const payload = 'The quick brown fox jumps over the lazy dog. '.repeat(50);
    for (const format of ['gzip', 'deflate', 'deflate-raw']) {
      const data = enc.encode(payload);
      const compressed = await pump(new CompressionStream(format), [data]);
      ok(
        compressed.byteLength < data.byteLength,
        `${format} should compress the repetitive payload`
      );
      const restored = await pump(new DecompressionStream(format), [
        compressed,
      ]);
      strictEqual(dec.decode(restored), payload, `${format} round trip`);
    }
  },
};

export const pendingReadServedOnWrite = {
  async test() {
    // A read parked before any write is served as soon as the eager codec
    // push produces output — for deflate that is the 2-byte zlib header
    // from the first write; the rest arrives with the close-time flush.
    // The complete deflate bytes of 'hello' are pinned; the decompression
    // leg parks its read the same way.
    const testData = 'hello';
    const check = new Uint8Array([
      0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02,
      0x15,
    ]);
    {
      const { writable, readable } = new CompressionStream('deflate');
      const writer = writable.getWriter();
      const reader = readable.getReader();
      const readPromise = reader.read();
      await writer.write(enc.encode(testData));
      await writer.close();
      const first = await readPromise;
      deepStrictEqual([...first.value], [0x78, 0x9c]);
      const rest = [];
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        rest.push(...value);
      }
      deepStrictEqual([...first.value, ...rest], [...check]);
    }
    {
      const { writable, readable } = new DecompressionStream('deflate');
      const writer = writable.getWriter();
      const reader = readable.getReader();
      const readPromise = reader.read();
      await writer.write(check);
      await writer.close();
      strictEqual(dec.decode((await readPromise).value), testData);
    }
  },
};
