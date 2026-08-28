// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Byte-transfer volumes: small (64 B), medium (64 KiB), large (1 MiB),
// very large (8 MiB) through JS-backed byte streams, drained by default
// and BYOB readers. Chunks carry a byte pattern continuous across chunk
// boundaries (prime modulus, so it never falls into step with
// power-of-two sizes); verification is byte-exact and names the first
// offending index.

import { strictEqual, ok } from 'node:assert';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

function assertPatternedBytes(bytes, total) {
  strictEqual(bytes.byteLength, total, 'wrong total byte length');
  for (let i = 0; i < total; i++) {
    if (bytes[i] !== i % PATTERN_MODULUS) {
      strictEqual(bytes[i], i % PATTERN_MODULUS, `pattern break at byte ${i}`);
    }
  }
}

// A byte ReadableStream producing `total` patterned bytes in
// `chunkLength`-sized enqueues.
// Closes in the SAME pull as the final enqueue: a close() with a
// pending unfilled BYOB read pends forever under the TypeScript
// implementation (pinned in controller.js
// closeWithPendingUnfilledByobRead), so a drain loop must never park a
// fresh read against an empty, about-to-close source.
function patternedByteSource(total, chunkLength) {
  let offset = 0;
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      const length = Math.min(chunkLength, total - offset);
      c.enqueue(patternChunk(offset, length));
      offset += length;
      if (offset >= total) c.close();
    },
  });
}

async function drainDefault(rs) {
  const reader = rs.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
    total += value.byteLength;
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

export const smallByteTransfer = {
  async test() {
    assertPatternedBytes(await drainDefault(patternedByteSource(64, 64)), 64);
  },
};

export const mediumByteTransfer = {
  async test() {
    const total = 64 * 1024;
    assertPatternedBytes(
      await drainDefault(patternedByteSource(total, total)),
      total
    );
  },
};

export const largeByteTransferDefaultReader = {
  async test() {
    const total = 1024 * 1024;
    assertPatternedBytes(
      await drainDefault(patternedByteSource(total, 16 * 1024)),
      total
    );
  },
};

export const largeByteTransferByobReader = {
  async test() {
    const total = 1024 * 1024;
    const rs = patternedByteSource(total, 16 * 1024);
    const reader = rs.getReader({ mode: 'byob' });
    const out = new Uint8Array(total);
    let offset = 0;
    let view = new Uint8Array(64 * 1024);
    for (;;) {
      const { value, done } = await reader.read(view);
      if (value !== undefined && value.byteLength > 0) {
        out.set(value, offset);
        offset += value.byteLength;
      }
      if (done) break;
      view = new Uint8Array(value.buffer);
    }
    strictEqual(offset, total);
    assertPatternedBytes(out, total);
  },
};

export const veryLargeByteTransfer = {
  async test() {
    const total = 8 * 1024 * 1024;
    assertPatternedBytes(
      await drainDefault(patternedByteSource(total, 64 * 1024)),
      total
    );
  },
};

// Very large drain with MISMATCHED read granularity: BYOB views half the
// enqueue size, exercising chunk splitting across many fills.
export const veryLargeByteTransferMismatchedViews = {
  async test() {
    const total = 4 * 1024 * 1024;
    const rs = patternedByteSource(total, 64 * 1024);
    const reader = rs.getReader({ mode: 'byob' });
    let offset = 0;
    let view = new Uint8Array(32 * 1024);
    for (;;) {
      const { value, done } = await reader.read(view);
      if (value !== undefined && value.byteLength > 0) {
        // Verify in place, streaming — no full-body buffer.
        for (let i = 0; i < value.byteLength; i++) {
          if (value[i] !== (offset + i) % PATTERN_MODULUS) {
            strictEqual(
              value[i],
              (offset + i) % PATTERN_MODULUS,
              `pattern break at byte ${offset + i}`
            );
          }
        }
        offset += value.byteLength;
      }
      if (done) break;
      view = new Uint8Array(value.buffer);
    }
    strictEqual(offset, total);
    ok(true);
  },
};
