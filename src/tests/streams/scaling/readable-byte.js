// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The queues a byte stream keeps: the buffered chunks, consumed by BYOB
// fills that each take many small entries, and the pending pull-into
// descriptors, submitted unawaited and filled by one enqueue. Counting
// the bytes ahead of the cursor by rescanning the backlog, or dequeuing
// descriptors with shift(), grew 40-100x on these shapes past ~20k
// entries. Every byte is pattern-checked.

import { strictEqual } from 'node:assert';
import { assertLinearScaling } from 'helpers';

// n one-byte chunks pre-buffered, read into 64-byte views, so every fill
// takes 64 entries off a backlog of up to n.
export const byobFillsOverBacklogScaleLinearly = {
  async test() {
    await assertLinearScaling(async (n) => {
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          for (let i = 0; i < n; i++) c.enqueue(new Uint8Array([i & 0xff]));
          c.close();
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      let view = new Uint8Array(64);
      let total = 0;
      for (;;) {
        const { value, done } = await reader.read(view);
        if (done) break;
        for (let j = 0; j < value.byteLength; j++) {
          strictEqual(value[j], (total + j) & 0xff, `byte ${total + j}`);
        }
        total += value.byteLength;
        view = new Uint8Array(value.buffer);
      }
      strictEqual(total, n);
    }, 16_000);
  },
};

// n one-byte BYOB reads submitted before any data, all filled by a single
// n-byte enqueue.
export const pendingByobReadsScaleLinearly = {
  async test() {
    await assertLinearScaling(async (n) => {
      let controller;
      const rs = new ReadableStream({
        type: 'bytes',
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader({ mode: 'byob' });
      const reads = [];
      for (let i = 0; i < n; i++) reads.push(reader.read(new Uint8Array(1)));
      const chunk = new Uint8Array(n);
      for (let i = 0; i < n; i++) chunk[i] = i & 0xff;
      controller.enqueue(chunk);
      const results = await Promise.all(reads);
      for (let i = 0; i < n; i++) {
        strictEqual(results[i].value[0], i & 0xff, `read ${i}`);
      }
    }, 10_000);
  },
};
