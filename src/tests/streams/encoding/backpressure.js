// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backpressure under the current defaults (encoder_stream_spec_compliant_
// backpressure pinned for C++): writable HWM 1, count-based; readable HWM
// 0, so no transform runs — and no write settles — until read demand
// exists.

import { strictEqual, notStrictEqual } from 'node:assert';

// Settlement, were it going to happen, is a synchronous microtask cascade
// off write(); a macrotask turn flushes it.
function macrotask() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

export const desiredSizeCountsQueuedChunks = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    strictEqual(writer.desiredSize, 1);
    const p1 = writer.write('a');
    strictEqual(writer.desiredSize, 0);
    const p2 = writer.write('b');
    strictEqual(writer.desiredSize, -1);

    const reader = tes.readable.getReader();
    await reader.read();
    await p1;
    strictEqual(writer.desiredSize, 0);
    await reader.read();
    await p2;
    strictEqual(writer.desiredSize, 1);
    await writer.close();
  },
};

export const writesParkWithoutReadDemand = {
  async test() {
    // Readable HWM 0: even the first write's transform waits for a pull.
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    let settled = 0;
    const p1 = writer.write('x').then(() => settled++);
    const p2 = writer.write('y').then(() => settled++);
    await macrotask();
    await macrotask();
    strictEqual(settled, 0);

    const reader = tes.readable.getReader();
    await reader.read();
    await reader.read();
    await Promise.all([p1, p2]);
    strictEqual(settled, 2);
    await writer.close();
  },
};

export const readyReflectsBackpressure = {
  async test() {
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const initialReady = writer.ready;
    await initialReady;

    const p1 = writer.write('a');
    const blocked = writer.ready;
    notStrictEqual(blocked, initialReady);
    let state = 'pending';
    blocked.then(() => (state = 'resolved'));
    await macrotask();
    strictEqual(state, 'pending');

    const reader = tes.readable.getReader();
    await reader.read();
    await p1;
    await blocked;
    // The same promise resolves on drain; it is not replaced again.
    strictEqual(writer.ready, blocked);
    await writer.close();
  },
};
