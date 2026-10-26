// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the byte stream benchmarks.

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

// [count, size] of the chunks a stream carries: many small chunks, a common
// network read size, and large chunks.
export const SHAPES = [
  [256, 64],
  [64, 4096],
  [16, 65536],
];

export function shapeLabel(count, size) {
  return `${count} x ${sizeLabel(size)}`;
}

// enqueue() transfers its chunk's buffer, so every chunk is a new one.
export function enqueuedByteStream(count, size) {
  return new ReadableStream({
    type: 'bytes',
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(new Uint8Array(size));
      controller.close();
    },
  });
}

// A byte stream whose pull() enqueues a new `size`-byte chunk per call,
// `count` times. It enqueues even when a BYOB request is pending: for a
// default read without autoAllocateChunkSize, C++ allocates a request
// (readable-byte ledger #5) where TypeScript, like the spec, has none, and
// enqueue() answers either.
export function enqueueOnPull(count, size) {
  let remaining = count;
  return new ReadableStream({
    type: 'bytes',
    pull(controller) {
      controller.enqueue(new Uint8Array(size));
      if (--remaining === 0) controller.close();
    },
  });
}

// A byte stream whose pull() answers each BYOB request with `size` bytes,
// `count` times, for reads that have a request under both implementations:
// BYOB reads into views of at least `size` bytes, and default reads with an
// autoAllocateChunkSize of at least `size`.
export function respondOnPull(count, size, options = {}) {
  let remaining = count;
  return new ReadableStream({
    type: 'bytes',
    ...options,
    pull(controller) {
      controller.byobRequest.respond(size);
      if (--remaining === 0) controller.close();
    },
  });
}

// Reads `reader`, a default reader, to the end, returning the number of
// bytes read.
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
