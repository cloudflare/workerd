// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Buffer lifecycle through the transform: chunks are never copied,
// validated, or detached — the reader receives the writer's chunk by
// identity, and a buffer detached while its view sits in the readable
// queue is observed detached by the reader (parity; the queue holds a
// reference, not a snapshot).

import { strictEqual, ok } from 'node:assert';

// The reader receives the exact object the writer wrote.
export const chunkIdentityThroughTransform = {
  async test() {
    const obj = { marker: 1 };
    const ts = new TransformStream({
      transform(chunk, c) {
        c.enqueue(chunk);
      },
    });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();
    const [, r] = await Promise.all([writer.write(obj), reader.read()]);
    strictEqual(r.value, obj);
  },
};

// Detaching a queued chunk's buffer before the read: the reader sees the
// same (now zero-length) view in both implementations.
export const detachWhileQueuedObservedByReader = {
  async test() {
    let ctrl;
    const ts = new TransformStream(
      {
        start(c) {
          ctrl = c;
        },
      },
      undefined,
      { highWaterMark: 2 }
    );
    const u8 = new Uint8Array([9]);
    ctrl.enqueue(u8);
    structuredClone(u8.buffer, { transfer: [u8.buffer] }); // detach while queued
    const reader = ts.readable.getReader();
    const r = await reader.read();
    ok(!r.done);
    strictEqual(r.value, u8);
    strictEqual(r.value.byteLength, 0);
  },
};
