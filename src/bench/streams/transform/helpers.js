// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the transform benchmarks.

// Cases check that they did the work their names claim, so a stream that
// ends early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

// Value streams never copy their chunks, so one object serves as every
// chunk.
export const CHUNK = { value: 1 };

// Reads `reader` to the end, returning the number of chunks read.
export async function drainReader(reader) {
  let count = 0;
  for (;;) {
    const { done } = await reader.read();
    if (done) return count;
    count++;
  }
}

// The transformers the cases compare: `make` returns a new one, and
// `output` is the number of chunks it produces from `count` written.
export const TRANSFORMERS = {
  'no transformer': { make: () => undefined, output: (count) => count },
  'transform() enqueues': {
    make: () => ({
      transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    }),
    output: (count) => count,
  },
  'async transform()': {
    make: () => ({
      async transform(chunk, controller) {
        controller.enqueue(chunk);
      },
    }),
    output: (count) => count,
  },
  'transform() enqueues twice': {
    make: () => ({
      transform(chunk, controller) {
        controller.enqueue(chunk);
        controller.enqueue(chunk);
      },
    }),
    output: (count) => count * 2,
  },
  'transform() drops every other chunk': {
    make: () => {
      let keep = false;
      return {
        transform(chunk, controller) {
          keep = !keep;
          if (keep) controller.enqueue(chunk);
        },
      };
    },
    output: (count) => Math.ceil(count / 2),
  },
  'flush() enqueues': {
    make: () => ({
      flush(controller) {
        controller.enqueue(CHUNK);
      },
    }),
    output: (count) => count + 1,
  },
};
