// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Transfer volumes through JS-backed TransformStreams, with a
// concurrent producer and consumer so writer backpressure and reader
// demand interleave realistically. The XOR transform proves the
// transformer touched every byte of a multi-MiB body; the byte pattern
// (prime modulus) is verified streaming at the consumer.

import { strictEqual } from 'node:assert';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

async function producePattern(writable, total, chunkLength) {
  const writer = writable.getWriter();
  for (let offset = 0; offset < total; offset += chunkLength) {
    const length = Math.min(chunkLength, total - offset);
    await writer.write(patternChunk(offset, length));
  }
  await writer.close();
}

// Consumes the readable, verifying each byte against
// `transform(pattern)`.
async function consumeVerifying(readable, total, mapByte) {
  const reader = readable.getReader();
  let received = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    for (let i = 0; i < value.byteLength; i++) {
      const expected = mapByte((received + i) % PATTERN_MODULUS);
      if (value[i] !== expected) {
        strictEqual(
          value[i],
          expected,
          `pattern break at byte ${received + i}`
        );
      }
    }
    received += value.byteLength;
  }
  strictEqual(received, total);
}

// LARGE passthrough: 1 MiB in 16 KiB chunks.
export const largeBytePassthrough = {
  async test() {
    const total = 1024 * 1024;
    const ts = new TransformStream();
    await Promise.all([
      producePattern(ts.writable, total, 16 * 1024),
      consumeVerifying(ts.readable, total, (b) => b),
    ]);
  },
};

// VERY LARGE with a real per-byte transform: 8 MiB XOR'd chunk by
// chunk — the output proves every byte passed through the transformer.
export const veryLargeXorTransform = {
  async test() {
    const total = 8 * 1024 * 1024;
    const ts = new TransformStream({
      transform(chunk, controller) {
        const out = new Uint8Array(chunk.byteLength);
        for (let i = 0; i < chunk.byteLength; i++) out[i] = chunk[i] ^ 0x5a;
        controller.enqueue(out);
      },
    });
    await Promise.all([
      producePattern(ts.writable, total, 64 * 1024),
      consumeVerifying(ts.readable, total, (b) => b ^ 0x5a),
    ]);
  },
};

// MEDIUM chunk count: 4096 value chunks through a mapping transform.
export const mediumChunkCountThroughTransform = {
  async test() {
    const COUNT = 4096;
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk * 2 + 1);
      },
    });
    const producer = (async () => {
      const writer = ts.writable.getWriter();
      for (let i = 0; i < COUNT; i++) await writer.write(i);
      await writer.close();
    })();
    const consumer = (async () => {
      const reader = ts.readable.getReader();
      let i = 0;
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        strictEqual(value, i * 2 + 1, `chunk ${i} corrupted`);
        i++;
      }
      strictEqual(i, COUNT);
    })();
    await Promise.all([producer, consumer]);
  },
};
