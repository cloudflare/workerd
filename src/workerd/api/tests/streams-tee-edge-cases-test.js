// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for tee() edge cases with asymmetric consumption patterns.
// These tests focus on scenarios where tee branches are consumed at
// different rates or only partially consumed.
//
// Test inspirations:
// - Bun: test/js/web/streams/streams.test.js (tee for default and direct streams)
// - Deno: tests/unit/streams_test.ts (tee tests)

import { strictEqual, ok, deepStrictEqual } from 'node:assert';

// Test tee with byte stream using default readers
// Inspired by: workerd streams-js-test.js (byte stream tee tests)
export const teeByteStreamDefaultReaders = {
  async test() {
    const data = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    let offset = 0;

    const rs = new ReadableStream({
      type: 'bytes',
      pull(controller) {
        if (offset < data.length) {
          const chunk = data.slice(offset, offset + 2);
          offset += 2;
          controller.enqueue(chunk);
        } else {
          controller.close();
        }
      },
    });

    const [branch1, branch2] = rs.tee();

    // Use default readers (not BYOB)
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    const bytes1 = [];
    const bytes2 = [];

    // Read all from both branches, collecting individual bytes
    while (true) {
      const { value, done } = await reader1.read();
      if (done) break;
      for (const b of value) bytes1.push(b);
    }

    while (true) {
      const { value, done } = await reader2.read();
      if (done) break;
      for (const b of value) bytes2.push(b);
    }

    // Both branches should have received all 8 bytes with same values
    deepStrictEqual(bytes1, [1, 2, 3, 4, 5, 6, 7, 8]);
    deepStrictEqual(bytes2, [1, 2, 3, 4, 5, 6, 7, 8]);
  },
};

// Test tee with byte stream using mixed reader types
// Inspired by: workerd streams-js-test.js (BYOB tee tests)
export const teeByteStreamMixedReaders = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });

    const [branch1, branch2] = rs.tee();

    // Use BYOB reader on branch1, default reader on branch2
    const reader1 = branch1.getReader({ mode: 'byob' });
    const reader2 = branch2.getReader();

    // Start reads
    const read1Promise = reader1.read(new Uint8Array(5));
    const read2Promise = reader2.read();

    // Enqueue data
    controller.enqueue(enc.encode('hello'));
    controller.close();

    const [result1, result2] = await Promise.all([read1Promise, read2Promise]);

    // Both should receive the data
    strictEqual(dec.decode(result1.value), 'hello');
    strictEqual(dec.decode(result2.value), 'hello');
  },
};
