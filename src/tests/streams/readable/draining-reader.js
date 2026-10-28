// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over JS-BACKED value streams — the
// bulk-drain conduit the C++ bridge drives to consume TypeScript
// streams (see the identity suite's draining-reader.js for the conduit
// basics). Exists only under the TS implementation (readable-ts.wd-test
// sets expose_draining_reader); the C++ cell asserts the global's
// absence.
//
// Value-stream facts pinned here: a JS stream's real queue (unlike the
// identity rendezvous) makes batching directly observable — a queued
// backlog plus the close sentinel is swept in ONE read; a read whose
// size limit runs out at the last chunk, its maxSize or the chunk sizes
// adding up to Infinity (the one limit a read without a maxSize, such
// as the C++ bridge's, can meet), reaches the close all the same: it
// reports done and closes the stream in that call, as a default read
// closes it as it takes the last chunk; value chunks pass through the
// conduit UNTOUCHED (object identity included); a JS stream's
// expectedLength is its source's declaration, undefined if it declares
// none, or once the stream (or, before the fork, the stream it was forked
// from) has been read from; cancel and error propagate through the conduit like a default
// reader's.

/* global ReadableStreamDrainingReader */

import { strictEqual, deepStrictEqual, ok, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const drainingReaderSweepsQueuedBacklog = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // Everything enqueued before the conduit attaches — including the
    // close sentinel — arrives in a single batched read.
    const marker = { object: 'identity' };
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('a');
        c.enqueue(42);
        c.enqueue(marker);
        c.close();
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    strictEqual(reader.expectedLength, undefined);
    strictEqual(rs.locked, true);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    strictEqual(chunks.length, 3);
    strictEqual(chunks[0], 'a');
    strictEqual(chunks[1], 42);
    strictEqual(chunks[2], marker); // the very instance
  },
};

export const drainingReaderMaxSizeEndsAtClose = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // One chunk of the default strategy's size 1, then close(): a read
    // with maxSize 1 takes the chunk, using up its maxSize, and reaches
    // the close sentinel all the same. It reports done, and the stream
    // closes in that call: closed resolves before the read does, and
    // desiredSize reads 0.
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
        c.enqueue('a');
        c.close();
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    let closed = false;
    reader.closed.then(() => {
      closed = true;
    });
    const first = await reader.read({ maxSize: 1 });
    deepStrictEqual(first.chunks, ['a']);
    strictEqual(first.done, true);
    strictEqual(controller.desiredSize, 0);
    strictEqual(closed, true);
    const second = await reader.read({ maxSize: 1 });
    deepStrictEqual(second.chunks, []);
    strictEqual(second.done, true);
  },
};

export const drainingReaderSizeOverflowEndsAtClose = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // A read without a maxSize, as the C++ bridge and body consumption
    // read, has its limit used up once the chunk sizes add up to
    // Infinity: here by the second of two chunks of size
    // Number.MAX_VALUE, the last before the close. It reaches the close
    // all the same: it reports done, and the stream closes in that call.
    const rs = new ReadableStream(
      {
        start(c) {
          c.enqueue('a');
          c.enqueue('b');
          c.close();
        },
      },
      { size: () => Number.MAX_VALUE }
    );
    const reader = new ReadableStreamDrainingReader(rs);
    let closed = false;
    reader.closed.then(() => {
      closed = true;
    });
    const first = await reader.read();
    deepStrictEqual(first.chunks, ['a', 'b']);
    strictEqual(first.done, true);
    strictEqual(closed, true);
    const second = await reader.read();
    deepStrictEqual(second.chunks, []);
    strictEqual(second.done, true);
  },
};

export const drainingReaderPullDriven = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // A pull-driven source with nothing buffered yields one chunk per
    // read via the always-makes-progress fallback, with EOF as a
    // separate empty batch.
    let pulls = 0;
    const rs = new ReadableStream(
      {
        pull(c) {
          pulls++;
          if (pulls <= 3) c.enqueue(`chunk${pulls}`);
          else c.close();
        },
      },
      { highWaterMark: 0 }
    );
    const reader = new ReadableStreamDrainingReader(rs);
    const seen = [];
    for (;;) {
      const { chunks, done } = await reader.read();
      seen.push(chunks.length);
      for (const chunk of chunks) seen.push(chunk);
      if (done) break;
    }
    deepStrictEqual(seen, [1, 'chunk1', 1, 'chunk2', 1, 'chunk3', 0]);
  },
};

export const drainingReaderErrorPropagation = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const err = new Error('source-err');
    let controller;
    const rs = new ReadableStream({
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

export const drainingReaderCancelReachesSource = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    let cancelArg = 'not-called';
    const rs = new ReadableStream({
      cancel(reason) {
        cancelArg = reason;
      },
    });
    const reader = new ReadableStreamDrainingReader(rs);
    const why = new Error('done with this');
    await reader.cancel(why);
    strictEqual(cancelArg, why);
  },
};

export const drainingReaderLockExclusivity = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const rs = new ReadableStream({});
    const reader = new ReadableStreamDrainingReader(rs);
    strictEqual(rs.locked, true);
    throws(() => rs.getReader(), TypeError);
    throws(() => new ReadableStreamDrainingReader(rs), TypeError);
    reader.releaseLock();
    strictEqual(rs.locked, false);
    ok(rs.getReader());
  },
};

export const drainingReaderReportsDeclaredLength = {
  test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // The source's declaration, as a bigint, until the reader is released.
    const rs = new ReadableStream({ expectedLength: 5 });
    const reader = new ReadableStreamDrainingReader(rs);
    strictEqual(reader.expectedLength, 5n);
    reader.releaseLock();
    strictEqual(reader.expectedLength, undefined);
  },
};

// The length the C++ bridge sees (#35 has the Content-Length): none once
// the stream has been read from, nor on a tee branch forked after that
// read, while a branch forked before it keeps the declaration.
export const drainingReaderReportsNoLengthAfterRead = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    function lengthOf(stream) {
      const reader = new ReadableStreamDrainingReader(stream);
      const length = reader.expectedLength;
      reader.releaseLock();
      return length;
    }
    const rs = new ReadableStream({
      expectedLength: 2,
      start(c) {
        c.enqueue(new Uint8Array(1));
        c.enqueue(new Uint8Array(1));
        c.close();
      },
    });
    const [early, read] = rs.tee();
    const reader = read.getReader();
    await reader.read();
    reader.releaseLock();
    strictEqual(lengthOf(read), undefined);
    const [late] = read.tee();
    strictEqual(lengthOf(early), 2n);
    strictEqual(lengthOf(late), undefined);
  },
};
