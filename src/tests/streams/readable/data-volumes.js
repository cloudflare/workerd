// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Value-stream transfer volumes. Value chunks aren't bytes, so volume
// has two axes: chunk COUNT (thousands of chunks through the queue
// machinery) and chunk SIZE (large single strings). Each chunk encodes
// its own index so reordering, loss, or duplication is detected
// per-chunk.

import { strictEqual } from 'node:assert';

// Chunk i is the letter for (i % 26) repeated `size` times.
function chunkFor(i, size) {
  return String.fromCharCode(65 + (i % 26)).repeat(size);
}

function pullCountingSource(count, size) {
  let i = 0;
  return new ReadableStream({
    pull(c) {
      if (i < count) c.enqueue(chunkFor(i++, size));
      else c.close();
    },
  });
}

async function drainAndVerify(rs, count, size) {
  const reader = rs.getReader();
  let i = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    strictEqual(value, chunkFor(i, size), `chunk ${i} corrupted`);
    i++;
  }
  strictEqual(i, count);
}

// MEDIUM count: 4096 small chunks through the queue machinery.
export const mediumChunkCountTransfer = {
  async test() {
    await drainAndVerify(pullCountingSource(4096, 16), 4096, 16);
  },
};

// LARGE single chunk: one 1 MiB string.
export const largeSingleStringChunk = {
  async test() {
    const size = 1024 * 1024;
    await drainAndVerify(pullCountingSource(1, size), 1, size);
  },
};

// VERY LARGE aggregate: 8 MiB of string data in 128 × 64 KiB chunks.
export const veryLargeAggregateTransfer = {
  async test() {
    await drainAndVerify(pullCountingSource(128, 64 * 1024), 128, 64 * 1024);
  },
};

// LARGE through tee: both branches observe the full 64 × 16 KiB
// content independently.
export const largeTransferThroughTee = {
  async test() {
    const [a, b] = pullCountingSource(64, 16 * 1024).tee();
    await Promise.all([
      drainAndVerify(a, 64, 16 * 1024),
      drainAndVerify(b, 64, 16 * 1024),
    ]);
  },
};
