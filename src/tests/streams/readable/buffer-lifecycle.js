// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Chunk ownership in a default (value) ReadableStream: the queue holds
// chunks BY REFERENCE — no copy, no transfer.

import { strictEqual } from 'node:assert';

// The reader receives the very object that was enqueued, including
// mutations made while it sat in the queue (parity).
export const chunkHeldByReference = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const buf = new Uint8Array([1, 2, 3]);
    controller.enqueue(buf);
    buf[0] = 9;
    const r = await rs.getReader().read();
    strictEqual(r.value, buf);
    strictEqual(r.value[0], 9);
  },
};

// A buffer detached while its view is queued is observed detached by
// the reader — same object, zero length (parity).
export const detachWhileQueuedObserved = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const buf = new ArrayBuffer(4);
    const view = new Uint8Array(buf);
    controller.enqueue(view);
    buf.transfer();
    const r = await rs.getReader().read();
    strictEqual(r.value, view);
    strictEqual(r.value.byteLength, 0);
  },
};
