// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The transformer.cancel hook: which terminal operations invoke it, with
// what reason, how often, and how an error() from inside it fans out.
// Complements WPT transform-streams/cancel.any.js, whose three C++
// expectedFailures narrow to the fan-out divergence pinned in
// cancelHookErrorFanOut (probed; pedantic_wpt does not change it).

import { strictEqual, ok, rejects } from 'node:assert';
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

// A cancel hook that calls controller.error(err) errors the writable, so a
// parallel writable.close() rejects with err (parity). Whether the pending
// readable.cancel() rejects too depends on whether the writable has errored
// by the time the hook's result settles (spec step 7.1.1): a sync hook's
// result settles first, while the writable is still erroring during start,
// and the cancel fulfills (parity); an async hook's promise settles two
// microtasks later, after the writable has errored, and the cancel rejects
// with err (the WPT cancel.any case). DIVERGENCE (ledger #3): C++ fulfills
// the cancel in both.
export const cancelHookErrorFanOut = {
  async test() {
    for (const isAsync of [false, true]) {
      let ctrl;
      const hook = () => {
        ctrl.error(new Error('from-cancel'));
      };
      const ts = new TransformStream({
        start(c) {
          ctrl = c;
        },
        cancel: isAsync ? async () => hook() : hook,
      });
      const cancelP = ts.readable.cancel('why');
      const closeP = ts.writable.close();
      const rs = await Promise.allSettled([cancelP, closeP]);

      if (usingTsImpl && isAsync) {
        strictEqual(rs[0].status, 'rejected');
        strictEqual(rs[0].reason.message, 'from-cancel');
      } else {
        strictEqual(rs[0].status, 'fulfilled');
      }
      strictEqual(rs[1].status, 'rejected');
      strictEqual(rs[1].reason.message, 'from-cancel');
    }
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

// An ASYNC cancel hook is awaited by both readable.cancel() and
// writable.abort(), and a throwing cancel hook rejects the abort with
// its error (migrated from streams-test.js tsCancel).
export const asyncCancelHookAwaitedAndThrowPropagates = {
  async test() {
    {
      let cancelCalled = false;
      const { readable } = new TransformStream({
        async cancel(reason) {
          strictEqual(reason, 'boom');
          await scheduler.wait(10);
          cancelCalled = true;
        },
      });
      ok(!cancelCalled);
      await readable.cancel('boom');
      ok(cancelCalled);
    }
    {
      let cancelCalled = false;
      const { writable } = new TransformStream({
        async cancel(reason) {
          strictEqual(reason, 'boom');
          await scheduler.wait(10);
          cancelCalled = true;
        },
      });
      ok(!cancelCalled);
      await writable.abort('boom');
      ok(cancelCalled);
    }
    {
      const { writable } = new TransformStream({
        async cancel() {
          throw new Error('boomy');
        },
      });
      await rejects(writable.abort('boom'), { message: 'boomy' });
    }
  },
};
