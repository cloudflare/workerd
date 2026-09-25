// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The queues a default stream keeps: the buffered chunks, read back from
// a pre-buffered backlog (alone, and through both tee branches), and the
// pending reads, submitted unawaited and satisfied by enqueues. An array
// dequeued with shift() grew 40-400x on these shapes past ~20k entries,
// once V8 stopped left-trimming it. Every chunk is index-checked, so the
// buffers cannot lose or reorder data as they grow and wrap.

import { strictEqual } from 'node:assert';
import { assertLinearScaling } from 'helpers';

function bufferedSource(n) {
  return new ReadableStream({
    start(c) {
      for (let i = 0; i < n; i++) c.enqueue(i);
      c.close();
    },
  });
}

async function readAllInOrder(rs, n) {
  const reader = rs.getReader();
  let i = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    strictEqual(value, i, `chunk ${i} out of order`);
    i++;
  }
  strictEqual(i, n);
}

// A pre-buffered backlog read back one chunk at a time reclaims one entry
// per read.
export const backlogReadScalesLinearly = {
  async test() {
    await assertLinearScaling(
      (n) => readAllInOrder(bufferedSource(n), n),
      20_000
    );
  },
};

// The same backlog behind a tee: the first branch drains while the second
// still holds every entry, then the second drains alone.
export const teeBacklogReadScalesLinearly = {
  async test() {
    await assertLinearScaling(async (n) => {
      const [a, b] = bufferedSource(n).tee();
      await readAllInOrder(a, n);
      await readAllInOrder(b, n);
    }, 10_000);
  },
};

// n reads submitted before any data, each satisfied by an enqueue in
// order.
export const pendingReadsScaleLinearly = {
  async test() {
    await assertLinearScaling(async (n) => {
      let controller;
      const rs = new ReadableStream({
        start(c) {
          controller = c;
        },
      });
      const reader = rs.getReader();
      const reads = [];
      for (let i = 0; i < n; i++) reads.push(reader.read());
      for (let i = 0; i < n; i++) controller.enqueue(i);
      const results = await Promise.all(reads);
      for (let i = 0; i < n; i++) {
        strictEqual(results[i].value, i, `read ${i} got the wrong chunk`);
      }
    }, 20_000);
  },
};
