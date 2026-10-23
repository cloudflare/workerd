// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the identity stream benchmarks.

// Cases check that they did the work their names claim, so a stream that
// ends early fails the case instead of reporting a fast time.
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

// Writes to identity streams copy their bytes, so one buffer of each size
// serves as every chunk.
const chunks = new Map();
export function chunkOf(size) {
  let chunk = chunks.get(size);
  if (chunk === undefined) {
    chunk = new Uint8Array(size);
    chunks.set(size, chunk);
  }
  return chunk;
}

// Reads `reader`, a default reader, to the end, returning the number of
// bytes read. The implementations split and merge writes into reads
// differently, so cases count bytes rather than reads.
export async function drainBytes(reader) {
  let bytes = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return bytes;
    bytes += value.byteLength;
  }
}

// Reads `reader`, a BYOB reader, to the end into views of `size` bytes,
// reusing the buffer each read returns, and returns the number of bytes
// read.
export async function drainByob(reader, size) {
  let buffer = new ArrayBuffer(size);
  let bytes = 0;
  for (;;) {
    const { value, done } = await reader.read(new Uint8Array(buffer));
    if (done) return bytes;
    bytes += value.byteLength;
    buffer = value.buffer;
  }
}

// Writes `count` chunks of `size` bytes to `writable` and closes it, while
// `read` reads the readable side to the end; returns what `read` returns.
export async function pump(stream, count, size, read) {
  const writer = stream.writable.getWriter();
  const drained = read(stream.readable);
  const chunk = chunkOf(size);
  const writes = [];
  for (let i = 0; i < count; i++) writes.push(writer.write(chunk));
  const [result] = await Promise.all([drained, ...writes, writer.close()]);
  return result;
}
