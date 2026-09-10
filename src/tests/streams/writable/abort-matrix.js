// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Abort matrix: reason identity across writer promises, sink-hook
// suppression, in-flight interactions with controller.error(), and the
// controller.signal surface. WPT writable-streams/aborting.any.js lists
// eleven C++ expectedFailures ("nitpickiness about the type of error");
// the scenarios here probe the same territory and pin what each
// implementation actually does — most of it is parity, with the
// signal-reason default and concurrent-abort identity as divergences.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl, pedanticWpt } from 'which-impl';

// Aborting before start() settles rejects ready and closed with the
// abort reason (by identity); the abort promise fulfills.
export const abortBeforeStartReasonIdentity = {
  async test() {
    const ws = new WritableStream({
      async start() {
        await scheduler.wait(5);
      },
    });
    const writer = ws.getWriter();
    const reason = new Error('r1');
    const abort = writer.abort(reason);

    await rejects(writer.ready, (e) => e === reason);
    await rejects(writer.closed, (e) => e === reason);
    strictEqual(await abort, undefined);
  },
};

// After an abort settles, writes reject with the very same reason object.
export const erroredStateReasonIdentity = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    const reason = { custom: true };
    await writer.abort(reason);
    await rejects(writer.write('x'), (e) => e === reason);
  },
};

// A stream errored by a throwing size() does not run the sink abort hook
// when abort() arrives later; the abort still fulfills (parity; cf. the
// WPT "sink abort() should not be called if stream was erroring due to
// bad strategy" case).
export const sinkAbortSkippedAfterBadStrategyError = {
  async test() {
    let abortCalled = false;
    const ws = new WritableStream(
      {
        abort() {
          abortCalled = true;
        },
      },
      {
        size() {
          throw new Error('bad size');
        },
        highWaterMark: 1,
      }
    );
    const writer = ws.getWriter();
    await rejects(writer.write('x'), { message: 'bad size' });
    strictEqual(await writer.abort('reason'), undefined);
    strictEqual(abortCalled, false);
  },
};

// writer.abort() while a write is in flight, with the sink write then
// REJECTING: the write surfaces its own failure, the abort fulfills, and
// closed carries the abort reason (parity).
export const inFlightWriteRejectionDuringAbort = {
  async test() {
    let rejectWrite;
    const ws = new WritableStream({
      write() {
        return new Promise((res, rej) => (rejectWrite = rej));
      },
    });
    const writer = ws.getWriter();
    await writer.ready;
    const write = writer.write('x');
    await scheduler.wait(1); // the sink write is now in flight on both sides
    const abortReason = new Error('abort-reason');
    const abort = writer.abort(abortReason);
    await scheduler.wait(1);
    rejectWrite(new Error('write-fail'));

    await rejects(write, { message: 'write-fail' });
    strictEqual(await abort, undefined);
    await rejects(writer.closed, (e) => e === abortReason);
  },
};

// writer.abort() then controller.error() while a write is in flight,
// with the write finishing cleanly: the abort request wins, both write
// and abort fulfill (parity).
export const abortThenControllerErrorInFlight = {
  async test() {
    let resolveWrite;
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        return new Promise((res) => (resolveWrite = res));
      },
    });
    const writer = ws.getWriter();
    await writer.ready;
    const write = writer.write('x');
    await scheduler.wait(1);
    const abort = writer.abort(new Error('abort-reason'));
    controller.error(new Error('ctrl-error'));
    await scheduler.wait(1);
    resolveWrite();

    strictEqual(await write, undefined);
    strictEqual(await abort, undefined);
  },
};

// writer.abort() then controller.error() while a write is in flight,
// with the write later rejecting. The abort request remains authoritative:
// closed carries its reason, and the sink abort hook waits for the in-flight
// write to settle on both implementations.
export const abortThenControllerErrorInFlightRejects = {
  async test() {
    const events = [];
    let rejectWrite;
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        events.push('sink-write');
        return new Promise((resolve, reject) => (rejectWrite = reject));
      },
      abort(reason) {
        events.push(`sink-abort:${reason}`);
      },
    });
    const writer = ws.getWriter();
    const closedExpectation = rejects(
      writer.closed,
      (e) => e === 'abort-reason'
    );
    const write = writer.write('chunk');
    const writeSettled = write.catch((e) =>
      events.push(`write-rejected:${e.message}`)
    );
    await scheduler.wait(1);
    const abortP = writer.abort('abort-reason');
    const abortSettled = abortP.then(
      () => events.push('abort-fulfilled'),
      (e) => events.push(`abort-rejected:${e.message}`)
    );
    controller.error(new Error('controller-error'));
    await scheduler.wait(1);
    strictEqual(events.join(' | '), 'sink-write');
    rejectWrite(new Error('write-failure'));
    await Promise.all([writeSettled, abortSettled, closedExpectation]);
    strictEqual(
      events.join(' | '),
      'sink-write | sink-abort:abort-reason | write-rejected:write-failure | abort-fulfilled'
    );
  },
};

// controller.error() then writer.abort() while a write is in flight: the
// stream is already erroring, so the abort rejects with the
// controller's error while the in-flight write still finishes and the
// sink abort hook remains suppressed (parity).
export const controllerErrorThenAbortInFlight = {
  async test() {
    let resolveWrite;
    let controller;
    let abortHookCalled = false;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      write() {
        return new Promise((res) => (resolveWrite = res));
      },
      abort() {
        abortHookCalled = true;
      },
    });
    const writer = ws.getWriter();
    await writer.ready;
    const write = writer.write('x');
    await scheduler.wait(1);
    controller.error(new Error('ctrl-error'));
    const abort = writer.abort(new Error('abort-reason'));
    await scheduler.wait(1);
    resolveWrite();

    strictEqual(await write, undefined);
    await rejects(abort, { message: 'ctrl-error' });
    strictEqual(abortHookCalled, false);
  },
};

// DIVERGENCE: the signal reason for a reasonless abort(). The spec (and
// TypeScript, and C++ under pedantic_wpt — standard.c++ WritableImpl::
// abort) synthesizes an AbortError DOMException; C++ otherwise leaves
// the reason undefined. An explicit reason is passed through verbatim
// everywhere.
export const abortSignalReason = {
  async test() {
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
      });
      ws.abort();
      await scheduler.wait(1);
      strictEqual(controller.signal.aborted, true);
      if (usingTsImpl || pedanticWpt) {
        ok(controller.signal.reason instanceof DOMException);
        strictEqual(controller.signal.reason.name, 'AbortError');
      } else {
        strictEqual(controller.signal.reason, undefined);
      }
    }
    {
      let controller;
      const ws = new WritableStream({
        start(c) {
          controller = c;
        },
      });
      ws.abort('why');
      await scheduler.wait(1);
      strictEqual(controller.signal.reason, 'why');
    }
  },
};

// DIVERGENCE: two aborts issued while the first is still pending. The
// spec returns the SAME promise for both (TypeScript); C++ mints a new
// promise per call (the WPT aborting.any "multiple writer.abort()s"
// case). Both fulfill with undefined.
export const concurrentAbortPromiseIdentity = {
  async test() {
    const ws = new WritableStream({
      async abort() {
        await scheduler.wait(5);
      },
    });
    const writer = ws.getWriter();
    const a1 = writer.abort('x');
    const a2 = writer.abort('y');
    strictEqual(a1 === a2, usingTsImpl);
    strictEqual(await a1, undefined);
    strictEqual(await a2, undefined);
  },
};
