// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the body benchmarks.

// Cases check that they did the work their names claim, so a body that
// ends early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

export function sizeLabel(bytes) {
  if (bytes >= 1048576) return `${bytes / 1048576} MiB`;
  return bytes >= 1024 ? `${bytes / 1024} KiB` : `${bytes} B`;
}

// [count, size] of the chunks a JavaScript stream carries: many small
// chunks, a common network read size, and large chunks.
export const SHAPES = [
  [256, 64],
  [64, 4096],
  [16, 65536],
];

export function shapeLabel(count, size) {
  return `${count} x ${sizeLabel(size)}`;
}

// Bodies and writes copy their bytes, so one buffer of each size serves as
// every body and every value stream chunk.
const buffers = new Map();
export function bytesOf(size) {
  let bytes = buffers.get(size);
  if (bytes === undefined) {
    bytes = new Uint8Array(size).fill(0x61);
    buffers.set(size, bytes);
  }
  return bytes;
}

// A value stream of `count` chunks of `size` bytes.
export function valueStream(count, size) {
  const chunk = bytesOf(size);
  return new ReadableStream({
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(chunk);
      controller.close();
    },
  });
}

// A value stream of `bytes`, in chunks of `size` bytes. Value streams don't
// transfer their chunks, so the chunks are views of `bytes`.
export function chunkedValueStream(bytes, size) {
  return new ReadableStream({
    start(controller) {
      for (let offset = 0; offset < bytes.length; offset += size) {
        controller.enqueue(bytes.subarray(offset, offset + size));
      }
      controller.close();
    },
  });
}

// A byte stream of `count` chunks of `size` bytes. enqueue() transfers its
// chunk's buffer, so every chunk is a new one.
export function byteStream(count, size) {
  return new ReadableStream({
    type: 'bytes',
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(new Uint8Array(size));
      controller.close();
    },
  });
}

// Reads `reader`, a default reader, to the end, returning the number of
// bytes read. The implementations read bodies in different chunk sizes, so
// cases count bytes rather than reads.
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
