// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared helpers for the encoding stream benchmarks.

// Cases check that they did the work their names claim, so a stream that
// ends early fails the case instead of reporting a fast time.
export function expectEqual(actual, expected, what) {
  if (actual !== expected) {
    throw new Error(`${what}: expected ${expected}, got ${actual}`);
  }
}

export function sizeLabel(units) {
  return units >= 1024 ? `${units / 1024} Ki` : `${units}`;
}

// [count, length] of the chunks written: many short chunks (a token
// stream), and longer ones.
export const SHAPES = [
  [256, 16],
  [64, 4096],
  [16, 65536],
];

// The text the cases encode and decode, by the number of UTF-8 bytes per
// character: 'a', 'ğ' and '中'.
export const TEXT = {
  ascii: { char: 'a', bytesPerChar: 1 },
  'two-byte': { char: '\u011f', bytesPerChar: 2 },
  'three-byte': { char: '\u4e2d', bytesPerChar: 3 },
};

// Writes `chunkAt(i)` to `stream` for each i below `count` and closes it,
// while the readable side is read to the end; returns the total `measure` of
// what was read.
export async function pump(stream, chunkAt, count, measure) {
  const writer = stream.writable.getWriter();
  const reader = stream.readable.getReader();
  const drained = (async () => {
    let total = 0;
    for (;;) {
      const { value, done } = await reader.read();
      if (done) return total;
      total += measure(value);
    }
  })();
  const writes = [];
  for (let i = 0; i < count; i++) writes.push(writer.write(chunkAt(i)));
  const [total] = await Promise.all([drained, ...writes, writer.close()]);
  return total;
}
