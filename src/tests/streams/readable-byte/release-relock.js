// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The WPT general.any releaseLock()-with-pending-read(view) cluster:
// releasing a reader rejects its pending reads, but the pull-into
// descriptor machinery survives, so a later respond()/enqueue() routes
// the bytes to a SECOND reader's read. Parity throughout except the
// released-read message and the overflow shape at the end.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

function byteStream(source = {}) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
    ...source,
  });
  return { rs, controller: () => controller };
}

const RELEASED_MSG = () =>
  usingTsImpl
    ? 'This reader has been released'
    : 'This ReadableStream reader has been released.';

// respond() after releaseLock routes to the second BYOB reader's read;
// the released read rejects TypeError (message per implementation).
export const relockRespondRoutesToSecondReader = {
  async test() {
    const { rs, controller } = byteStream();
    const r1 = rs.getReader({ mode: 'byob' });
    const read1 = r1.read(new Uint8Array(4));
    await scheduler.wait(5);
    r1.releaseLock();
    const err = await rejectionOf(read1);
    strictEqual(err.name, 'TypeError');
    strictEqual(err.message, RELEASED_MSG());
    const r2 = rs.getReader({ mode: 'byob' });
    const read2 = r2.read(new Uint8Array(4));
    const req = controller().byobRequest;
    ok(req !== null);
    req.view[0] = 7;
    req.view[1] = 8;
    req.respond(2);
    const { value, done } = await read2;
    strictEqual(done, false);
    strictEqual(value.byteLength, 2);
    strictEqual(value[0], 7);
    strictEqual(value[1], 8);
  },
};

// A Uint16Array read on the second reader is assembled from two
// separate respond(1) calls (parity).
export const relockUint16RespondAcrossResponds = {
  async test() {
    const { rs, controller } = byteStream();
    const r1 = rs.getReader({ mode: 'byob' });
    const read1 = r1.read(new Uint16Array(1));
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = rs.getReader({ mode: 'byob' });
    const read2 = r2.read(new Uint16Array(1));
    controller().byobRequest.view[0] = 1;
    controller().byobRequest.respond(1);
    controller().byobRequest.view[0] = 2;
    controller().byobRequest.respond(1);
    const { value, done } = await read2;
    strictEqual(done, false);
    ok(value instanceof Uint16Array);
    // bytes 1,2 little-endian
    strictEqual(value[0], 513);
  },
};

// respondWithNewView() after relock also routes to the second reader
// (parity).
export const relockRespondWithNewView = {
  async test() {
    const { rs, controller } = byteStream();
    const r1 = rs.getReader({ mode: 'byob' });
    const read1 = r1.read(new Uint8Array(4));
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = rs.getReader({ mode: 'byob' });
    const read2 = r2.read(new Uint8Array(4));
    const req = controller().byobRequest;
    const nv = new Uint8Array(req.view.buffer, req.view.byteOffset, 2);
    nv[0] = 9;
    nv[1] = 10;
    req.respondWithNewView(nv);
    const { value, done } = await read2;
    strictEqual(done, false);
    strictEqual(value.byteLength, 2);
    strictEqual(value[0], 9);
    strictEqual(value[1], 10);
  },
};

// The autoAllocateChunkSize variants: relock a pending DEFAULT read,
// then respond() or enqueue() fulfills the second default reader
// (parity).
export const relockAutoAllocateRespond = {
  async test() {
    const { rs, controller } = byteStream({ autoAllocateChunkSize: 8 });
    const r1 = rs.getReader();
    const read1 = r1.read();
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = rs.getReader();
    const read2 = r2.read();
    const req = controller().byobRequest;
    ok(req !== null);
    req.view[0] = 5;
    req.respond(1);
    const { value, done } = await read2;
    strictEqual(done, false);
    strictEqual(value.byteLength, 1);
    strictEqual(value[0], 5);
  },
};

export const relockAutoAllocateEnqueue = {
  async test() {
    const { rs, controller } = byteStream({ autoAllocateChunkSize: 8 });
    const r1 = rs.getReader();
    const read1 = r1.read();
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = rs.getReader();
    const read2 = r2.read();
    controller().enqueue(new Uint8Array([1, 2, 3]));
    const { value, done } = await read2;
    strictEqual(done, false);
    strictEqual(value.byteLength, 3);
    strictEqual(value[2], 3);
  },
};

// DIVERGENCE: respond(3) when the released 4-byte descriptor heads the
// queue but the second reader's read wants only 2 bytes. C++ throws
// RangeError from respond() and the second read stays pending
// (bounded); TypeScript accepts the respond, committing the bytes to
// the released descriptor, and fulfills the second read with its
// 2-byte view UNTOUCHED (zeros).
export const relockRespondOverflowSecondView = {
  async test() {
    const { rs, controller } = byteStream();
    const r1 = rs.getReader({ mode: 'byob' });
    const read1 = r1.read(new Uint8Array(4));
    await scheduler.wait(5);
    r1.releaseLock();
    await rejectionOf(read1);
    const r2 = rs.getReader({ mode: 'byob' });
    const read2 = r2.read(new Uint8Array(2));
    const req = controller().byobRequest;
    if (usingTsImpl) {
      req.respond(3);
      const { value, done } = await read2;
      strictEqual(done, false);
      strictEqual(value.byteLength, 2);
      strictEqual(value[0], 0);
      strictEqual(value[1], 0);
    } else {
      let caught;
      try {
        req.respond(3);
      } catch (e) {
        caught = e;
      }
      strictEqual(caught.name, 'RangeError');
      strictEqual(
        caught.message,
        'Too many bytes [3] in response to a BYOB read request.'
      );
      // The second read stays pending (bounded observation).
      const outcome = await Promise.race([
        read2.then(() => 'settled'),
        scheduler.wait(50).then(() => 'pending'),
      ]);
      strictEqual(outcome, 'pending');
      await r2.cancel('cleanup');
    }
  },
};
