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
// INTACT (no coalescing, no re-slicing); the conduit drives pull like a
// default reader — under the TS implementation byobRequest is null even
// with autoAllocateChunkSize set (the suite's ledger #5/#6 shape); byte
// streams never declare an expectedLength.

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

export const drainingReaderDrivesBytePull = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // The conduit's demand drives pull() like a default reader's: the
    // TS implementation presents byobRequest null even with
    // autoAllocateChunkSize set, so the source must enqueue.
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
    deepStrictEqual(byobRequests, ['null', 'null', 'null']);
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
