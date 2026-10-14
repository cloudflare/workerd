// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over JS-BACKED byte streams — the
// bulk-drain conduit the C++ bridge drives to consume TypeScript
// streams (conduit basics in the identity suite's draining-reader.js).
// Exists only under the TS implementation (readable-byte-ts.wd-test
// sets expose_draining_reader); the C++ cell asserts the global's
// absence.
//
// Byte-stream facts pinned here: a queued byte backlog plus the close
// sentinel is swept in one batched read with each enqueued chunk kept
// INTACT (no coalescing, no re-slicing); a read whose maxSize runs out
// at the last bytes before the close reports done: false and leaves the
// stream readable until the next read reaches the close (the readable
// suite pins the same for value streams); with autoAllocateChunkSize set
// the conduit's wait-read synthesizes the auto-allocate descriptor, so
// pull observes a byobRequest (without it, pull sees null — ledger #5);
// a byte stream that declares no expectedLength reads undefined, and a
// declared one passes through as the exact bigint, across the uint64 range.

/* global ReadableStreamDrainingReader */

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const drainingReaderSweepsByteBacklog = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        c.enqueue(new Uint8Array([1, 2, 3]));
        c.enqueue(new Uint8Array([4]));
        c.enqueue(new Uint8Array([5, 6]));
        c.close();
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    strictEqual(reader.expectedLength, undefined);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    deepStrictEqual(
      chunks.map((chunk) => Array.from(chunk)),
      [[1, 2, 3], [4], [5, 6]]
    );
  },
};

export const drainingReaderByteMaxSizeEndsAtClose = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // Three bytes, then close(): a read with maxSize 3 takes them and
    // stops before the close sentinel. The stream stays readable,
    // desiredSize at the high-water mark and closed pending, until the
    // next read reaches the close. (A high-water mark of 8 tells
    // readable from closed: at the byte default of 0, both read 0.)
    let controller;
    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(c) {
          controller = c;
          c.enqueue(new Uint8Array([1, 2, 3]));
          c.close();
        },
      },
      { highWaterMark: 8 }
    );
    const reader = new ReadableStreamDrainingReader(rs);
    let closed = false;
    reader.closed.then(() => {
      closed = true;
    });
    const first = await reader.read({ maxSize: 3 });
    deepStrictEqual(
      first.chunks.map((chunk) => Array.from(chunk)),
      [[1, 2, 3]]
    );
    strictEqual(first.done, false);
    strictEqual(controller.desiredSize, 8);
    await scheduler.wait(0);
    strictEqual(closed, false);
    const second = await reader.read({ maxSize: 3 });
    deepStrictEqual(second.chunks, []);
    strictEqual(second.done, true);
    strictEqual(controller.desiredSize, 0);
    strictEqual(closed, true);
  },
};

export const drainingReaderByteExpectedLengthPassThrough = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // Above 2^53 too, where a number would lose precision.
    for (const length of [9007199254740993n, 0xffff_ffff_ffff_ffffn]) {
      const rs = new ReadableStream({ type: 'bytes', expectedLength: length });
      const reader = new ReadableStreamDrainingReader(rs);
      strictEqual(reader.expectedLength, length);
      await reader.cancel();
    }
  },
};

export const drainingReaderDrivesBytePull = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // The conduit's demand drives pull() like a default reader's: with
    // autoAllocateChunkSize set, each wait-read synthesizes the
    // auto-allocate descriptor, so every pull observes a byobRequest.
    // The source may still enqueue() — the enqueued chunk fulfills the
    // read directly and the auto-allocated buffer is discarded.
    const byobRequests = [];
    let pulls = 0;
    const rs = new ReadableStream({
      type: 'bytes',
      autoAllocateChunkSize: 64,
      pull(c) {
        pulls++;
        byobRequests.push(c.byobRequest === null ? 'null' : 'present');
        if (pulls <= 2) c.enqueue(new Uint8Array([pulls]));
        else c.close();
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    const seen = [];
    for (;;) {
      const { chunks, done } = await reader.read();
      for (const chunk of chunks) seen.push(chunk[0]);
      if (done) break;
    }
    deepStrictEqual(seen, [1, 2]);
    deepStrictEqual(byobRequests, ['present', 'present', 'present']);
  },
};

export const drainingReaderByteErrorPropagation = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const err = new Error('byte-err');
    let controller;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        controller = c;
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    const readP = reader.read();
    controller.error(err);
    const reason = await readP.then(
      () => {
        throw new Error('expected rejection');
      },
      (e) => e
    );
    strictEqual(reason, err);
  },
};

export const drainingReaderByteCancelReachesSource = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    let cancelArg = 'not-called';
    const rs = new ReadableStream({
      type: 'bytes',
      cancel(reason) {
        cancelArg = reason;
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    const why = new Error('enough bytes');
    await reader.cancel(why);
    strictEqual(cancelArg, why);
  },
};
