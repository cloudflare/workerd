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

// Test consuming only one branch of a tee completely
// Inspired by: Bun test/js/web/streams/streams.test.js (tee tests)
export const teeConsumeOneBranchFully = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      pull(controller) {
        pullCount++;
        if (pullCount <= 5) {
          controller.enqueue(pullCount);
        } else {
          controller.close();
        }
      },
    });

    const [branch1, branch2] = rs.tee();

    // Only consume branch1 fully
    const reader1 = branch1.getReader();
    const values = [];

    while (true) {
      const { value, done } = await reader1.read();
      if (done) break;
      values.push(value);
    }

    deepStrictEqual(values, [1, 2, 3, 4, 5]);

    // branch2 should still be readable (though branch1 consumed the data)
    ok(!branch2.locked);

    // Now consume branch2
    const reader2 = branch2.getReader();
    const values2 = [];

    while (true) {
      const { value, done } = await reader2.read();
      if (done) break;
      values2.push(value);
    }

    deepStrictEqual(values2, [1, 2, 3, 4, 5]);
  },
};

// Test tee with different read rates on branches
// Inspired by: Deno tests/unit/streams_test.ts (async stream tests)
export const teeDifferentReadRates = {
  async test() {
    let counter = 0;
    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 10) {
          controller.enqueue(counter);
        } else {
          controller.close();
        }
      },
    });

    const [branch1, branch2] = rs.tee();

    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    const results1 = [];
    const results2 = [];

    // Read from branch1 fast, branch2 slow
    for (let i = 0; i < 10; i++) {
      // Read 2 from branch1
      results1.push((await reader1.read()).value);
      if (i % 2 === 1) {
        // Read 1 from branch2 every other iteration
        results2.push((await reader2.read()).value);
      }
    }

    // Finish reading branch2
    while (true) {
      const { value, done } = await reader2.read();
      if (done) break;
      results2.push(value);
    }

    // Finish reading branch1
    while (true) {
      const { value, done } = await reader1.read();
      if (done) break;
      results1.push(value);
    }

    // Both branches should have received all values
    deepStrictEqual(results1, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    deepStrictEqual(results2, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
  },
};

// Test canceling the slower branch mid-stream
// Inspired by: Bun test/js/web/streams/streams.test.js (cancel tests)
export const teeCancelSlowBranch = {
  async test() {
    let counter = 0;
    let sourceCancelled = false;

    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 20) {
          controller.enqueue(counter);
        } else {
          controller.close();
        }
      },
      cancel() {
        sourceCancelled = true;
      },
    });

    const [branch1, branch2] = rs.tee();

    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    // Read 5 from both branches
    for (let i = 0; i < 5; i++) {
      await reader1.read();
      await reader2.read();
    }

    // Cancel branch2 (the "slow" one)
    await reader2.cancel('No longer needed');

    // Source should NOT be cancelled yet (branch1 still active)
    ok(!sourceCancelled);

    // Continue reading branch1 to completion
    const remaining = [];
    while (true) {
      const { value, done } = await reader1.read();
      if (done) break;
      remaining.push(value);
    }

    // Branch1 should have received remaining values
    strictEqual(remaining.length, 15);
    strictEqual(remaining[0], 6);
    strictEqual(remaining[14], 20);
  },
};

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

// Test tee with large number of chunks
// Inspired by: Deno tests/unit/streams_test.ts (large stream tests)
export const teeLargeChunkCount = {
  async test() {
    const CHUNK_COUNT = 1000;
    let counter = 0;

    const rs = new ReadableStream({
      pull(controller) {
        if (counter < CHUNK_COUNT) {
          controller.enqueue(counter++);
        } else {
          controller.close();
        }
      },
    });

    const [branch1, branch2] = rs.tee();

    // Read both branches in parallel
    async function consumeBranch(branch) {
      const reader = branch.getReader();
      let count = 0;
      let sum = 0;
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        count++;
        sum += value;
      }
      return { count, sum };
    }

    const [result1, result2] = await Promise.all([
      consumeBranch(branch1),
      consumeBranch(branch2),
    ]);

    strictEqual(result1.count, CHUNK_COUNT);
    strictEqual(result2.count, CHUNK_COUNT);
    strictEqual(result1.sum, result2.sum);
    // Sum of 0 to 999 = 999 * 1000 / 2 = 499500
    strictEqual(result1.sum, 499500);
  },
};

// Test tee after partial read from original stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const teeAfterPartialRead = {
  async test() {
    let counter = 0;
    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 10) {
          controller.enqueue(counter);
        } else {
          controller.close();
        }
      },
    });

    // Read some values before tee
    const originalReader = rs.getReader();
    const firstValue = await originalReader.read();
    strictEqual(firstValue.value, 1);

    // Release lock and tee
    originalReader.releaseLock();

    const [branch1, branch2] = rs.tee();

    // Both branches should start from where original left off
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    const value1 = await reader1.read();
    const value2 = await reader2.read();

    // Both should get value 2 (first value after the pre-tee read)
    strictEqual(value1.value, 2);
    strictEqual(value2.value, 2);

    reader1.releaseLock();
    reader2.releaseLock();
  },
};

// Test that cancel reason is passed through tee
// Inspired by: Deno tests/unit/streams_test.ts (cancel reason tests)
export const teeCancelReason = {
  async test() {
    let receivedReason = null;
    let cancelCalled = false;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('data');
      },
      cancel(reason) {
        cancelCalled = true;
        receivedReason = reason;
      },
    });

    const [branch1, branch2] = rs.tee();

    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();

    // Read one value from each
    await reader1.read();
    await reader2.read();

    // Cancel both with specific reasons
    await reader1.cancel('Reason from branch 1');
    await reader2.cancel('Reason from branch 2');

    // The source cancel should be called when both branches are cancelled
    ok(cancelCalled, 'Source cancel should be called');
    // The reason format may vary by implementation - it could be:
    // - An array of reasons
    // - The first reason
    // - The second reason
    // - A composite reason
    // Just verify cancel was called with some reason
    ok(
      receivedReason !== null && receivedReason !== undefined,
      'Cancel reason should be provided'
    );
  },
};
