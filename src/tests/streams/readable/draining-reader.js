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
// backlog plus the close sentinel is swept in ONE read; value chunks
// pass through the conduit UNTOUCHED (object identity included); a JS
// stream never declares an expectedLength; cancel and error propagate
// through the conduit like a default reader's.

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
