// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-18:
// Concurrent BYOB readAtLeast() calls with partial enqueues must
// not trigger an internal ByteQueue invariant failure. The bug was
// that handlePush() in queue.c++ re-buffered a partially consumed
// entry from offset 0 instead of from the unread tail
// (entryOffset), causing duplicated bytes that violated the
// queueTotalSize < atLeast assertion on the next enqueue.

import { strictEqual, deepStrictEqual } from 'node:assert';

export const concurrentByobReadAtLeastPartialEnqueue = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(controller) {
        ctrl = controller;
      },
    });

    const reader = rs.getReader({ mode: 'byob' });

    // Issue two concurrent readAtLeast(5) calls with 5-byte views.
    // Both are pending since no data has been enqueued yet.
    const p1 = reader.readAtLeast(5, new Uint8Array(5));
    const p2 = reader.readAtLeast(5, new Uint8Array(5));

    // Enqueue 7 bytes. handlePush processes pending reads:
    //   Read #1 (atLeast=5, view=5): copies 5 bytes,
    //     amountAvailable=2, entryOffset=5
    //   Read #2 (atLeast=5): amountAvailable(2) < atLeast(5),
    //     so buffer the remainder.
    //     BUG: bufferData(0) → queueTotalSize = 7 (wrong!)
    //     FIX: bufferData(5) → queueTotalSize = 2 (correct)
    ctrl.enqueue(new Uint8Array([1, 2, 3, 4, 5, 6, 7]));

    // Enqueue 4 more bytes. With the bug, queueTotalSize=7 and
    // the KJ_REQUIRE (state.queueTotalSize < atLeast → 7 < 5)
    // fails. With the fix, queueTotalSize=2,
    // amountAvailable=2+4=6 >= 5, so read #2 is fulfilled.
    ctrl.enqueue(new Uint8Array([8, 9, 10, 11]));

    ctrl.close();

    const r1 = await p1;
    const r2 = await p2;

    strictEqual(r1.done, false);
    strictEqual(r2.done, false);

    // Read #1: first 5 bytes from the 7-byte enqueue.
    const r1Bytes = new Uint8Array(
      r1.value.buffer,
      r1.value.byteOffset,
      r1.value.byteLength
    );
    deepStrictEqual(
      Array.from(r1Bytes),
      [1, 2, 3, 4, 5],
      'read #1 should get bytes [1..5]'
    );

    // Read #2: remaining 2 from first enqueue + 3 from second.
    const r2Bytes = new Uint8Array(
      r2.value.buffer,
      r2.value.byteOffset,
      r2.value.byteLength
    );
    deepStrictEqual(
      Array.from(r2Bytes),
      [6, 7, 8, 9, 10],
      'read #2 should get bytes [6..10]'
    );
  },
};
