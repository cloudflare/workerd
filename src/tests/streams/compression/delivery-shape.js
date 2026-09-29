// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How a write's output reaches the readable. The codec runs eagerly, so one
// write can produce megabytes, and a default read gets a bounded piece of it
// (ledger #16): under TypeScript the sink moves the output into the
// readable's queue as 64 KiB chunks before the write settles; under C++ it
// waits in the codec's buffer and each read takes the internal-stream read
// buffer's worth, 4 KiB or 16 KiB under the updated-auto-allocate-chunk-size
// autogate. A BYOB read fills its view in both. A write's output is never
// held whole in JS beside the codec's copy of it.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { pump } from 'round-trip';

const kTsPiece = 64 * 1024;

function checkPieceSize(n) {
  if (usingTsImpl) {
    strictEqual(n, kTsPiece);
  } else {
    ok(n === 4096 || n === 16 * 1024, `C++ read size ${n}`);
  }
}

// Period-251 pattern: a misplaced or repeated piece would be detected at
// every byte.
function pattern(size) {
  const data = new Uint8Array(size);
  for (let i = 0; i < size; i++) data[i] = i % 251;
  return data;
}

function checkPattern(chunk, offset) {
  for (let i = 0; i < chunk.length; i++) {
    if (chunk[i] !== (offset + i) % 251) {
      throw new Error(`byte ${offset + i} is ${chunk[i]}`);
    }
  }
}

// Reads to the end, checking every byte against the pattern; returns the
// chunk sizes.
async function readPatternPieces(readable, expectedTotal) {
  const sizes = [];
  let offset = 0;
  for await (const chunk of readable) {
    checkPattern(chunk, offset);
    offset += chunk.byteLength;
    sizes.push(chunk.byteLength);
  }
  strictEqual(offset, expectedTotal);
  return sizes;
}

export const largeOutputDeliveredInBoundedPieces = {
  async test() {
    const size = 4 * 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      pattern(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    // The whole output exists before the first read.
    await writer.write(compressed);
    await writer.close();
    const sizes = await readPatternPieces(ds.readable, size);
    for (const n of sizes) checkPieceSize(n);
    // Every piece is a full one: the output is an exact multiple of each
    // implementation's piece size.
    strictEqual(sizes.length * sizes[0], size);
  },
};

export const byobReadsFillTheView = {
  async test() {
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      pattern(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(compressed);
    await writer.close();
    const reader = ds.readable.getReader({ mode: 'byob' });
    const viewSize = 256 * 1024;
    let view = new Uint8Array(viewSize);
    for (let offset = 0; offset < size; offset += viewSize) {
      const { value, done } = await reader.read(view);
      strictEqual(done, false);
      strictEqual(value.byteLength, viewSize);
      checkPattern(value, offset);
      view = new Uint8Array(value.buffer);
    }
    const tail = await reader.read(view);
    strictEqual(tail.done, true);
  },
};

export const concurrentReadsTakeConsecutivePieces = {
  async test() {
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      pattern(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const reader = ds.readable.getReader();
    const first = reader.read();
    const second = reader.read();
    second.catch(() => {});
    const writer = ds.writable.getWriter();
    await writer.write(compressed);
    const a = await first;
    checkPieceSize(a.value.byteLength);
    checkPattern(a.value, 0);
    if (usingTsImpl) {
      const b = await second;
      strictEqual(b.value.byteLength, kTsPiece);
      checkPattern(b.value, kTsPiece);
    } else {
      // Ledger #14: a second concurrent default read rejects under C++.
      await rejects(second, TypeError);
    }
  },
};

export const teeBranchesReceiveBoundedPieces = {
  async test() {
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      pattern(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(compressed);
    await writer.close();
    const [a, b] = ds.readable.tee();
    const [sizesA, sizesB] = await Promise.all([
      readPatternPieces(a, size),
      readPatternPieces(b, size),
    ]);
    for (const n of sizesA) checkPieceSize(n);
    for (const n of sizesB) checkPieceSize(n);
  },
};

export const trailingJunkAfterLargeOutput = {
  async test() {
    // Ledger #17: output produced before a trailing-junk error reaches the
    // waiting reads under TypeScript, one piece each, before the error;
    // C++ rejects the waiting read (and the second concurrent read, #14).
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      pattern(size),
    ]);
    const padded = new Uint8Array(compressed.byteLength + 1);
    padded.set(compressed);
    padded[compressed.byteLength] = 0xff;
    const ds = new DecompressionStream('gzip');
    const reader = ds.readable.getReader();
    const first = reader.read();
    const second = reader.read();
    second.catch(() => {});
    const writer = ds.writable.getWriter();
    await rejects(writer.write(padded), {
      name: 'TypeError',
      message: 'Trailing bytes after end of compressed data',
    });
    if (usingTsImpl) {
      const a = await first;
      strictEqual(a.value.byteLength, kTsPiece);
      checkPattern(a.value, 0);
      const b = await second;
      strictEqual(b.value.byteLength, kTsPiece);
      checkPattern(b.value, kTsPiece);
    } else {
      await rejects(first, TypeError);
      await rejects(second, TypeError);
    }
    await rejects(reader.read(), TypeError);
  },
};
