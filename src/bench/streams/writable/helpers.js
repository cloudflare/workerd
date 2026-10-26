// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the writable benchmarks.

// Cases check that they did the work their names claim, so a stream that
// drops writes fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

// Sinks never copy their chunks, so one object serves as every chunk.
export const CHUNK = { value: 1 };

// A sink that counts the chunks it is given. `async` makes write() return a
// promise.
export function countingSink({ async = false } = {}) {
  const sink = { writes: 0 };
  sink.write = async
    ? async () => {
        sink.writes++;
      }
    : () => {
        sink.writes++;
      };
  return sink;
}
