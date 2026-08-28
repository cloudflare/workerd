// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Error propagation through pipeTo (the WPT error-propagation-forward
// seed cluster): errored endpoints at pipe start and mid-pipe, the
// prevent* options including truthy coercion, and hook argument
// identity.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const rejectionOf = (p) =>
  p.then(
    () => {
      throw new Error('expected rejection');
    },
    (e) => e
  );

// A source that starts errored: the pipe rejects with the very error
// and the destination's abort hook receives it.
export const sourceStartsErrored = {
  async test() {
    const err = new Error('src-err');
    let abortArg;
    const rs = new ReadableStream({
      start(c) {
        c.error(err);
      },
    });
    const ws = new WritableStream({
      abort(r) {
        abortArg = r;
      },
    });
    strictEqual(await rejectionOf(rs.pipeTo(ws)), err);
    strictEqual(abortArg, err);
    strictEqual(ws.locked, false);
  },
};

// preventAbort (boolean and TRUTHY non-boolean): the pipe still rejects
// with the source error, the destination abort hook never runs, and the
// destination stays usable.
export const sourceStartsErroredPreventAbort = {
  async test() {
    for (const preventAbort of [true, 'yes']) {
      const err = new Error('src-err');
      let abortCalled = false;
      const rs = new ReadableStream({
        start(c) {
          c.error(err);
        },
      });
      const ws = new WritableStream({
        abort() {
          abortCalled = true;
        },
      });
      strictEqual(await rejectionOf(rs.pipeTo(ws, { preventAbort })), err);
      strictEqual(abortCalled, false);
      ws.getWriter(); // still usable
    }
  },
};

// A source erroring after a chunk while the destination never desires
// (hwm 0): the pipe rejects with the error and the abort hook receives
// it. DIVERGENCE (the WPT 'dest never desires chunks' seeds): C++
// writes the chunk anyway — its pipe loop ignores the destination's
// desiredSize — while TypeScript respects backpressure and never
// writes.
export const sourceErroredAfterChunkHwmZero = {
  async test() {
    const err = new Error('src-err');
    let abortArg;
    const wrote = [];
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const ws = new WritableStream(
      {
        write(chunk) {
          wrote.push(chunk);
        },
        abort(r) {
          abortArg = r;
        },
      },
      new CountQueuingStrategy({ highWaterMark: 0 })
    );
    const pipeP = rs.pipeTo(ws);
    await scheduler.wait(5);
    controller.enqueue('a');
    await scheduler.wait(5);
    controller.error(err);
    strictEqual(await rejectionOf(pipeP), err);
    if (usingTsImpl) {
      strictEqual(wrote.length, 0);
    } else {
      strictEqual(wrote.length, 1);
      strictEqual(wrote[0], 'a');
    }
    strictEqual(abortArg, err);
  },
};

// A destination that starts errored: the pipe rejects with the dest
// error and the source's cancel hook receives it; preventCancel
// suppresses the cancel and releases the lock.
export const destStartsErrored = {
  async test() {
    const derr = new Error('dest-err');
    let cancelArg;
    const rs = new ReadableStream({
      cancel(r) {
        cancelArg = r;
      },
    });
    const ws = new WritableStream({
      start(c) {
        c.error(derr);
      },
    });
    strictEqual(await rejectionOf(rs.pipeTo(ws)), derr);
    strictEqual(cancelArg, derr);
  },
};

export const destStartsErroredPreventCancel = {
  async test() {
    const derr = new Error('dest-err');
    let cancelCalled = false;
    const rs = new ReadableStream({
      cancel() {
        cancelCalled = true;
      },
    });
    const ws = new WritableStream({
      start(c) {
        c.error(derr);
      },
    });
    strictEqual(
      await rejectionOf(rs.pipeTo(ws, { preventCancel: true })),
      derr
    );
    strictEqual(cancelCalled, false);
    strictEqual(rs.locked, false);
  },
};
