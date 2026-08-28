// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The transformer.cancel hook: which terminal operations invoke it, with
// what reason, how often, and how an error() from inside it fans out.
// Complements WPT transform-streams/cancel.any.js, whose three C++
// expectedFailures narrow to the fan-out divergence pinned in
// cancelHookErrorFanOut (probed; pedantic_wpt does not change it).

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// readable.cancel(reason): the cancel hook (not flush) runs with the
// reason; the writable side errors with the same reason (parity).
export const readableCancelRunsCancelHook = {
  async test() {
    let cancelArg = 'not-called';
    let flushCalled = false;
    const ts = new TransformStream({
      cancel(reason) {
        cancelArg = reason;
      },
      flush() {
        flushCalled = true;
      },
    });
    const writer = ts.writable.getWriter();
    strictEqual(await ts.readable.cancel('why'), undefined);
    await scheduler.wait(5);
    strictEqual(cancelArg, 'why');
    strictEqual(flushCalled, false);
    await rejects(writer.closed, (e) => e === 'why');
    await rejects(writer.write('x'), (e) => e === 'why');
  },
};

// writable.abort(reason): the cancel hook (not flush) runs with the
// reason; the readable side errors with the same reason (parity).
export const writableAbortRunsCancelHook = {
  async test() {
    let cancelArg = 'not-called';
    let flushCalled = false;
    const ts = new TransformStream({
      cancel(reason) {
        cancelArg = reason;
      },
      flush() {
        flushCalled = true;
      },
    });
    const reader = ts.readable.getReader();
    strictEqual(await ts.writable.abort('stop'), undefined);
    await scheduler.wait(5);
    strictEqual(cancelArg, 'stop');
    strictEqual(flushCalled, false);
    await rejects(reader.read(), (e) => e === 'stop');
  },
};

// DIVERGENCE (the WPT cancel.any family): when the cancel hook calls
// controller.error(err), the spec (TypeScript) rejects BOTH the pending
// readable.cancel() and the parallel writable.close() with err; C++
// fulfills the readable.cancel() and only rejects the close.
export const cancelHookErrorFanOut = {
  async test() {
    let ctrl;
    const ts = new TransformStream({
      start(c) {
        ctrl = c;
      },
      cancel() {
        ctrl.error(new Error('from-cancel'));
      },
    });
    const cancelP = ts.readable.cancel('why');
    const closeP = ts.writable.close();
    const rs = await Promise.allSettled([cancelP, closeP]);

    if (usingTsImpl) {
      strictEqual(rs[0].status, 'rejected');
      strictEqual(rs[0].reason.message, 'from-cancel');
    } else {
      strictEqual(rs[0].status, 'fulfilled');
    }
    strictEqual(rs[1].status, 'rejected');
    strictEqual(rs[1].reason.message, 'from-cancel');
  },
};

// A writable.abort() after readable.cancel() does not run the cancel
// hook a second time (parity; the WPT "should not call cancel() again"
// case).
export const cancelHookRunsOnce = {
  async test() {
    let calls = 0;
    const ts = new TransformStream({
      cancel() {
        calls++;
      },
    });
    await ts.readable.cancel('one');
    await ts.writable.abort('two').catch(() => {});
    await scheduler.wait(5);
    strictEqual(calls, 1);
  },
};
