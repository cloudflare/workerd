// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the piping benchmarks.

// Cases check that they did the work their names claim, so a pipe that ends
// early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

// Value streams never copy their chunks, so one object serves as every
// chunk.
export const CHUNK = { value: 1 };

export function sizeLabel(bytes) {
  return bytes >= 1024 ? `${bytes / 1024} KiB` : `${bytes} B`;
}

// [count, size] of the chunks a byte stream carries: many small chunks, a
// common network read size, and large chunks.
export const SHAPES = [
  [256, 64],
  [64, 4096],
  [16, 65536],
];

export function shapeLabel(count, size) {
  return `${count} x ${sizeLabel(size)}`;
}

// Reads `reader` to the end, returning the number of chunks read.
export async function drainReader(reader) {
  let count = 0;
  for (;;) {
    const { done } = await reader.read();
    if (done) return count;
    count++;
  }
}

// A stream whose start() enqueues `count` chunks and closes.
export function enqueuedStream(count) {
  return new ReadableStream({
    start(controller) {
      for (let i = 0; i < count; i++) controller.enqueue(CHUNK);
      controller.close();
    },
  });
}

// A stream whose pull() enqueues one chunk per call, closing with the last
// one.
export function pullStream(count) {
  let remaining = count;
  return new ReadableStream({
    pull(controller) {
      controller.enqueue(CHUNK);
      if (--remaining === 0) controller.close();
    },
  });
}

// A writable stream that counts the chunks its sink is given, in `.writes`.
// `async` makes write() return a promise.
export function countingWritable({ async = false, highWaterMark = 1 } = {}) {
  const counter = { writes: 0 };
  const write = async
    ? async () => {
        counter.writes++;
      }
    : () => {
        counter.writes++;
      };
  counter.stream = new WritableStream({ write }, { highWaterMark });
  return counter;
}
