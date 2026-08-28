// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// abort() semantics: reason propagation to pending writes/promises, the
// controller signal, sink-hook sequencing relative to in-flight
// start/write/close, and terminal-state interactions. Migrated from
// streams-js-test.js and streams-error-edge-cases-test.js.
//
// DIVERGENCE (startedness): C++ invokes sink algorithms synchronously
// from writer.write()/close(), so a write issued immediately before
// abort() is already in flight and runs to completion (its promise
// fulfills). TypeScript follows the spec's [[started]] gating: sink
// hooks only run after the start promise settles in a microtask, so a
// synchronous write→abort sequence finds the write still queued and
// rejects it with the abort reason. Several tests below pin both sides.

import { strictEqual, ok, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Aborting rejects pending writes with the reason, fires the controller
// signal, and leaves the stream persistently errored.
export const writableStreamWriteAbort = {
  async test() {
    let aborted = false;
    const ws = new WritableStream({
      start(c) {
        c.signal.addEventListener(
          'abort',
          () => {
            strictEqual(c.signal.reason.message, 'boom');
            aborted = true;
          },
          { once: true }
        );
      },
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();
    const write1 = writer.write(1);
    const write2 = writer.write(2);

    writer.abort(new Error('boom'));

    const [result1, result2] = await Promise.allSettled([write1, write2]);

    // Both writes fail
    strictEqual(result1.status, 'rejected');
    strictEqual(result2.status, 'rejected');
    strictEqual(result1.reason.message, 'boom');
    strictEqual(result2.reason.message, 'boom');

    ok(aborted);

    // Aborting puts the stream into a persistent errored state
    writer.releaseLock();
    const writer2 = ws.getWriter();

    await rejects(writer2.write('should not be allowed'), { message: 'boom' });
  },
};

// Aborting replaces the ready promise with a rejected one.
export const writableStreamAbortReadyRejected = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    const ready = writer.ready;

    await ready;

    writer.abort('boom');

    ok(ready !== writer.ready);

    const results = await Promise.allSettled([writer.ready]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, 'boom');
  },
};

// abort() with no argument defaults the reason to undefined.
export const writableStreamAbortOptional = {
  async test() {
    const ws = new WritableStream();
    const writer = ws.getWriter();
    await writer.abort();

    const results = await Promise.allSettled([writer.closed]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, undefined);
  },
};

// Aborting while start() is pending rejects the ready promise.
export const writableStreamAbortWhileStarting = {
  async test() {
    const ws = new WritableStream({
      async start() {},
    });

    const writer = ws.getWriter();
    writer.abort('boom');

    const results = await Promise.allSettled([writer.ready]);
    strictEqual(results[0].status, 'rejected');
    strictEqual(results[0].reason, 'boom');
  },
};

// An abort() hook error while a write is pending rejects the abort
// promise; the abort hook is not re-run. The pending write diverges per
// the startedness model: in flight under C++ (fulfills), still queued
// under TypeScript (rejects with the abort reason, undefined).
export const writableStreamAbortWhileWriting = {
  async test() {
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },

      async abort() {
        throw new Error('boom');
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('test');

    await rejects(writer.abort(), { message: 'boom' });

    if (usingTsImpl) {
      await rejects(write, (err) => err === undefined);
    } else {
      await write;
    }

    // The write should reject with undefined because the writer.abort() above
    // specified undefined. The abort algorithm should not be called again.
    await rejects(writer.write('should fail'), (err) => err === undefined);
  },
};

// releaseLock() while an abort is pending rejects the closed promise with
// a TypeError.
export const writableStreamReleaseLockWhileAborting = {
  async test() {
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();
    writer.write('test');

    writer.abort();
    const closed = writer.closed;

    writer.releaseLock();

    await rejects(closed, TypeError);
  },
};

// The sink abort() hook waits for in-flight start/write/close.
export const writableStreamAbortTiming = {
  async test() {
    // Abort waits for start
    {
      let started = false;
      const ws = new WritableStream({
        async start() {
          await scheduler.wait(10);
          started = true;
        },
        abort() {
          ok(started, 'The stream should have started first');
        },
      });

      await ws.abort();
    }

    // Abort waits for write
    {
      let writeCompleted = false;
      const ws = new WritableStream({
        async write() {
          await scheduler.wait(10);
          writeCompleted = true;
        },
        abort() {
          ok(writeCompleted, 'The write should have completed');
        },
      });

      const writer = ws.getWriter();
      const write = writer.write('hello');
      const abort = writer.abort();

      await Promise.allSettled([write, abort]);
    }

    // Abort waits for close
    {
      let closeCompleted = false;
      const ws = new WritableStream({
        async close() {
          await scheduler.wait(10);
          closeCompleted = true;
        },
        abort() {
          ok(closeCompleted, 'The close should have completed');
        },
      });

      const writer = ws.getWriter();
      const close = writer.close();
      const abort = writer.abort();

      await Promise.allSettled([close, abort]);
    }
  },
};

// Aborting with a write and close pending: the close is aborted, the
// abort succeeds, and the sink abort hook runs in both implementations.
// The write diverges per the startedness model: in flight under C++
// (finishes), still queued under TypeScript (rejected with the abort
// reason).
export const writableStreamAbortWriteClosePending = {
  async test() {
    let abortCalled = false;
    const ws = new WritableStream({
      async write() {
        await scheduler.wait(10);
      },
      abort() {
        abortCalled = true;
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('hello');
    const close = writer.close();
    const abort = writer.abort();

    const res = await Promise.allSettled([write, close, abort]);
    ok(abortCalled);

    if (usingTsImpl) {
      strictEqual(res[0].status, 'rejected'); // Queued write is aborted
      strictEqual(res[0].reason, undefined);
    } else {
      strictEqual(res[0].status, 'fulfilled'); // In-flight write finishes
    }
    strictEqual(res[1].status, 'rejected'); // Pending close is aborted
    strictEqual(res[2].status, 'fulfilled'); // Abort finishes
  },
};

// controller.error() during an in-flight write rejects ready without
// waiting for the write to finish.
export const writableStreamErrorDuringInFlightWrite = {
  async test() {
    let controller;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      async write() {
        await scheduler.wait(10);
      },
    });

    const writer = ws.getWriter();

    const write = writer.write('hello').catch(() => {});

    controller.error('boom');

    await Promise.all([write, writer.ready.catch(() => {})]);
  },
};

// start() erroring after an abort leaves the stream errored; close rejects.
export const writableStreamStartErrorAfterAbort = {
  async test() {
    const ws = new WritableStream({
      async start() {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });

    ws.abort();

    // close() should return a rejected promise. The abort was called
    // before start finished, so the stream enters an errored state.
    await ws.close().catch(() => {});

    // Verify the stream is errored
    strictEqual(ws.locked, false);
  },
};

// A rejected sink write does not prevent the sink abort hook from running.
export const writableStreamRejectedWriteNoPreventAbort = {
  async test() {
    let abortCalled = false;
    const ws = new WritableStream({
      write() {
        throw new Error('boom');
      },
      abort() {
        abortCalled = true;
      },
    });

    const writer = ws.getWriter();
    const write = writer.write('hello');
    const abort = writer.abort();

    const res = await Promise.allSettled([write, abort]);
    ok(abortCalled);
    strictEqual(res[0].status, 'rejected');
    strictEqual(res[1].status, 'fulfilled');
  },
};

// Aborting twice: both promises fulfill.
export const writableStreamAbortedTwice = {
  async test() {
    const ws = new WritableStream();
    const abort1 = ws.abort();
    const abort2 = ws.abort();

    await Promise.all([abort1, abort2]);
  },
};

// Aborting an already-errored stream rejects with the stored error.
export const writableStreamAbortOnErroredResolves = {
  async test() {
    const ws = new WritableStream({
      start(c) {
        c.error(new Error('boom'));
      },
    });

    // When aborting an already-errored stream, abort() rejects with the stored error
    await rejects(ws.abort(), Error);
  },
};

// The sink abort hook is not called if the stream errored before abort().
export const writableStreamSinkAlgNoCallErrorBeforeAbort = {
  test() {
    let controller;
    let abortCalled = false;
    const ws = new WritableStream({
      start(c) {
        controller = c;
      },
      abort() {
        abortCalled = true;
      },
    });

    controller.error(new Error('boom'));
    ws.abort(new Error('bang'));

    ok(!abortCalled);
  },
};

// A writer acquired after a pending abort has a rejected ready promise.
export const writableStreamWriterWithPendingAbort = {
  async test() {
    const ws = new WritableStream();
    ws.abort(new Error('boom'));
    const writer = ws.getWriter();

    await rejects(writer.ready, Error);
  },
};

// writer.abort() during a slow in-flight write: the write settles rather
// than hanging. Migrated from streams-error-edge-cases-test.js.
export const errorRaceWithCloseWritable = {
  async test() {
    let writeStarted = false;

    const ws = new WritableStream({
      write() {
        writeStarted = true;
        // Simulate a slow write that can be aborted
        return scheduler.wait(100);
      },
    });

    const writer = ws.getWriter();

    const writePromise = writer.write('data').catch((e) => e);

    await scheduler.wait(5);
    ok(writeStarted, 'Write should have started');

    await writer.abort(new Error('Abort wins'));

    const writeResult = await writePromise;
    ok(
      writeResult === undefined || writeResult instanceof Error,
      'Write should complete or error'
    );
  },
};
