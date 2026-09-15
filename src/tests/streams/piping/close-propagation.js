// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Backward propagation while a pipe is active — the territory of the
// WPT close-propagation-backward and error-propagation-backward files,
// which are DISABLED for the C++ implementation because several cases
// hang. Every await here is bounded; a pinned 'pending' outcome is a
// deliberate defect pin, not a hole.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

// Calling close() on the destination while the pipe holds its lock
// rejects (locked), and the pipe itself is unaffected.
export const externalCloseOnPipedDestRejects = {
  async test() {
    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
    });
    const ws = new WritableStream({});
    const pipeP = rs.pipeTo(ws);
    await scheduler.wait(1);
    strictEqual(ws.locked, true);
    await rejects(ws.close(), { name: 'TypeError' });
    // The pipe still completes normally once the source closes.
    rc.close();
    await pipeP;
  },
};

// Same for abort(): a locked destination cannot be aborted externally.
export const externalAbortOnPipedDestRejects = {
  async test() {
    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
    });
    const ws = new WritableStream({});
    const pipeP = rs.pipeTo(ws);
    await scheduler.wait(1);
    await rejects(ws.abort('nope'), { name: 'TypeError' });
    rc.close();
    await pipeP;
  },
};

// The destination's write() hook throws on the second chunk: the pipe
// rejects with that error and the source's cancel hook receives it
// (error propagated BACKWARD).
export const destWriteThrowsMidPipe = {
  async test() {
    const werr = new Error('write-err');
    let cancelArg = 'not-called';
    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel(r) {
        cancelArg = r;
      },
    });
    const ws = new WritableStream({
      write(chunk) {
        if (chunk === 'boom') throw werr;
      },
    });
    const pipeP = rs.pipeTo(ws);
    rc.enqueue('ok');
    rc.enqueue('boom');
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, werr);
    strictEqual(cancelArg, werr);
    strictEqual(rs.locked, false);
  },
};

// Same with preventCancel: the pipe rejects, the source is NOT
// canceled, and its lock is released so the remaining chunk is
// readable.
export const destWriteThrowsMidPipePreventCancel = {
  async test() {
    const werr = new Error('write-err');
    let cancelCalled = false;
    let rc;
    const rs = new ReadableStream({
      start(c) {
        rc = c;
      },
      cancel() {
        cancelCalled = true;
      },
    });
    const ws = new WritableStream({
      write(chunk) {
        if (chunk === 'boom') throw werr;
      },
    });
    const pipeP = rs.pipeTo(ws, { preventCancel: true });
    rc.enqueue('ok');
    rc.enqueue('boom');
    rc.enqueue('after');
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, werr);
    strictEqual(cancelCalled, false);
    strictEqual(rs.locked, false);
    // DIVERGENCE: C++ leaves the not-yet-written chunk in the source's
    // queue, readable after the pipe. TypeScript's read-ahead already
    // consumed it before the failing write settled, so a fresh read
    // pends (bounded observation).
    const read = await outcomeOf(rs.getReader().read());
    if (usingTsImpl) {
      strictEqual(read.state, 'pending');
    } else {
      strictEqual(read.state, 'fulfilled');
      strictEqual(read.value.value, 'after');
    }
  },
};

// The destination's controller errors OUTSIDE a write while the pipe
// waits on a read: backward error propagation with nothing in flight.
// DIVERGENCE: TypeScript rejects the pipe with the error and cancels
// the source with it (spec); C++ half-propagates — it cancels the
// source with the error but FULFILLS the pipe promise.
export const destControllerErrorsMidPipe = {
  async test() {
    const derr = new Error('dest-err');
    let cancelArg = 'not-called';
    let wc;
    const rs = new ReadableStream({
      pull() {
        // never produces; the pipe waits on a read
      },
      cancel(r) {
        cancelArg = r;
      },
    });
    const ws = new WritableStream({
      start(c) {
        wc = c;
      },
    });
    const pipeP = rs.pipeTo(ws);
    await scheduler.wait(1);
    wc.error(derr);
    const outcome = await outcomeOf(pipeP);
    if (usingTsImpl) {
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason, derr);
      strictEqual(cancelArg, derr);
    } else {
      strictEqual(outcome.state, 'fulfilled');
      strictEqual(cancelArg, derr);
    }
  },
};
