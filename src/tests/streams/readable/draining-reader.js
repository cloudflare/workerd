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
// backlog plus the close sentinel is swept in ONE read; a read that
// takes the last chunk but stops before the close sentinel, for its
// maxSize or because the chunk sizes add up to Infinity (the one stop
// a read without a maxSize, such as the C++ bridge's, can meet),
// reports done: false and leaves the stream readable until the next
// read reaches the close, where a default read closes the stream as it
// takes the last chunk; value chunks pass through the conduit
// UNTOUCHED (object identity included); a JS stream never declares an
// expectedLength; cancel and error propagate through the conduit like
// a default reader's.

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
    // with maxSize 1 takes the chunk and stops before the close sentinel.
    // The stream stays readable, desiredSize at the high-water mark and
    // closed pending, until the next read reaches the close.
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
    strictEqual(first.done, false);
    strictEqual(controller.desiredSize, 1);
    await scheduler.wait(0);
    strictEqual(closed, false);
    const second = await reader.read({ maxSize: 1 });
    deepStrictEqual(second.chunks, []);
    strictEqual(second.done, true);
    strictEqual(controller.desiredSize, 0);
    strictEqual(closed, true);
  },
};

export const drainingReaderSizeOverflowEndsAtClose = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // A read without a maxSize, as the C++ bridge and body consumption
    // read, stops where the chunk sizes add up to Infinity: here at the
    // second of two chunks of size Number.MAX_VALUE, before the close.
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
    strictEqual(first.done, false);
    await scheduler.wait(0);
    strictEqual(closed, false);
    const second = await reader.read();
    deepStrictEqual(second.chunks, []);
    strictEqual(second.done, true);
    strictEqual(closed, true);
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
