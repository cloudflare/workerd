// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the queuing strategy benchmarks.

// Cases check that they did the work their names claim, so a stream that
// ends early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

// Value streams never copy their chunks, so one chunk serves as every
// chunk. It has a byteLength, for ByteLengthQueuingStrategy.
export const CHUNK = new Uint8Array(64);
