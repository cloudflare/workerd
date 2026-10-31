// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the DigestStream benchmarks.

// Cases check that they did the work their names claim, so a stream that
// drops writes fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

export function sizeLabel(bytes) {
  return bytes >= 1024 ? `${bytes / 1024} KiB` : `${bytes} B`;
}

// [count, size] of the chunks written: many small chunks, a common network
// read size, and large chunks.
export const SHAPES = [
  [256, 64],
  [64, 4096],
  [16, 65536],
];

export function shapeLabel(count, size) {
  return `${count} x ${sizeLabel(size)}`;
}

// Writes don't modify their chunks, so one buffer of each size serves as
// every chunk.
const chunks = new Map();
export function chunkOf(size) {
  let chunk = chunks.get(size);
  if (chunk === undefined) {
    chunk = new Uint8Array(size).fill(0x61);
    chunks.set(size, chunk);
  }
  return chunk;
}

// Checks that `stream`, a closed DigestStream, hashed `bytes` bytes into a
// digest of `digestSize` bytes.
export async function expectDigest(stream, bytes, digestSize) {
  expectEqual((await stream.digest).byteLength, digestSize, 'digest size');
  expectEqual(stream.bytesWritten, BigInt(bytes), 'bytes hashed');
}
