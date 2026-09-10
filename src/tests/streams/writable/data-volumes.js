// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Write-side transfer volumes: many small writes, large single writes,
// and multi-MiB chunked writes with the writer's backpressure honored.
// The sink verifies the continuous byte pattern (prime modulus) as
// chunks ARRIVE, so memory stays bounded and the first corrupt byte is
// named.

import { strictEqual } from 'node:assert';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

// A sink that verifies arriving bytes continue the pattern.
function verifyingSink() {
  const state = { received: 0, closed: false };
  const ws = new WritableStream({
    write(chunk) {
      for (let i = 0; i < chunk.byteLength; i++) {
        if (chunk[i] !== (state.received + i) % PATTERN_MODULUS) {
          strictEqual(
            chunk[i],
            (state.received + i) % PATTERN_MODULUS,
            `pattern break at byte ${state.received + i}`
          );
        }
      }
      state.received += chunk.byteLength;
    },
    close() {
      state.closed = true;
    },
  });
  return { ws, state };
}

async function writePattern(ws, total, chunkLength) {
  const writer = ws.getWriter();
  for (let offset = 0; offset < total; offset += chunkLength) {
    const length = Math.min(chunkLength, total - offset);
    await writer.ready;
    await writer.write(patternChunk(offset, length));
  }
  await writer.close();
}

// MEDIUM count: 4096 × 16 B writes.
export const manySmallWrites = {
  async test() {
    const { ws, state } = verifyingSink();
    await writePattern(ws, 4096 * 16, 16);
    strictEqual(state.received, 4096 * 16);
    strictEqual(state.closed, true);
  },
};

// LARGE single write: one 1 MiB chunk.
export const largeSingleWrite = {
  async test() {
    const total = 1024 * 1024;
    const { ws, state } = verifyingSink();
    await writePattern(ws, total, total);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};

// LARGE chunked: 1 MiB in 16 KiB writes.
export const largeChunkedWrites = {
  async test() {
    const total = 1024 * 1024;
    const { ws, state } = verifyingSink();
    await writePattern(ws, total, 16 * 1024);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};

// VERY LARGE chunked: 8 MiB in 64 KiB writes.
export const veryLargeChunkedWrites = {
  async test() {
    const total = 8 * 1024 * 1024;
    const { ws, state } = verifyingSink();
    await writePattern(ws, total, 64 * 1024);
    strictEqual(state.received, total);
    strictEqual(state.closed, true);
  },
};
