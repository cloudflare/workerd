// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// write() semantics: chunk identity, queueing of multiple pending writes,
// and the ordering guarantees of the writer's promises. Migrated from
// streams-js-test.js.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Multiple writes may be pending at once and complete in order.
export const writableStreamMultiplePendingWrites = {
  async test() {
    const expectedWrites = ['hello', 'there'];
    const ws = new WritableStream({
      async write(value) {
        await scheduler.wait(10);
        strictEqual(value, expectedWrites.shift());
      },
    });

    const writer = ws.getWriter();

    await Promise.all([writer.write('hello'), writer.write('there')]);
  },
};

// A TypedArray subarray is passed through to the sink by reference.
export const writableStreamWriteSubarray = {
  async test() {
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const sub = u8.subarray(1, 3);

    const ws = new WritableStream({
      write(value) {
        strictEqual(value, sub);
      },
    });

    const writer = ws.getWriter();

    await writer.write(sub);
  },
};

// Any JavaScript value can be written; the sink sees it by identity.
export const writableStreamWriteAny = {
  async test() {
    // Make a copy since we'll shift() from it
    const expectedWrites = [
      'hello',
      true,
      1,
      1.1,
      undefined,
      NaN,
      Infinity,
      new Uint8Array(1),
      {},
      [],
    ];
    // Keep original for writing
    const valuesToWrite = [...expectedWrites];

    const ws = new WritableStream({
      async write(value) {
        await scheduler.wait(1);
        const expected = expectedWrites.shift();
        // Use Object.is for NaN and -0/+0 handling (same as testharness same_value)
        ok(Object.is(value, expected), `expected ${expected} but got ${value}`);
      },
    });

    const writer = ws.getWriter();

    await Promise.all(valuesToWrite.map((i) => writer.write(i)));
  },
};

// Writer promises settle in issue order, including under abort.
export const writableStreamPromisesResolvedInOrder = {
  async test() {
    // Write before close
    {
      let closeFinished = false;
      const ws = new WritableStream();

      const writer = ws.getWriter();

      const write = writer.write('hello').then(() => ok(!closeFinished));
      const close = writer.close();
      const closed = writer.closed.then(() => (closeFinished = true));

      // Closed promise should not resolve before fulfilled write.
      await Promise.allSettled([write, close, closed]);
    }

    // Rejected write before close
    {
      let closeFinished = false;
      let writeFailed = false;
      const ws = new WritableStream({
        write() {
          throw new Error('boom');
        },
      });

      const writer = ws.getWriter();

      const write = writer.write('hello').catch(() => {
        ok(!closeFinished);
        writeFailed = true;
      });
      const close = writer.close();
      const closed = writer.closed.then(() => (closeFinished = true));

      // Closed promise should not resolve before rejected write.
      await Promise.allSettled([write, close, closed]);
      ok(writeFailed);
    }

    // Writes settled in order when aborting. The first write diverges per
    // the startedness model (see abort-semantics.js): in flight under C++
    // so it fulfills; still queued under TypeScript so it rejects with
    // the abort reason. Either way, settlement order is 1, 2, 3.
    {
      const order = [];
      const ws = new WritableStream({
        async write() {
          await scheduler.wait(10);
        },
      });

      const writer = ws.getWriter();

      const write1 = writer.write('hello').then(
        () => order.push(usingTsImpl ? -1 : 1),
        () => order.push(usingTsImpl ? 1 : -1)
      );
      const write2 = writer.write('hello').catch(() => order.push(2));
      const write3 = writer.write('hello').catch(() => order.push(3));
      const abort = writer.abort();

      await Promise.allSettled([write1, write2, write3, abort]);

      strictEqual(order[0], 1);
      strictEqual(order[1], 2);
      strictEqual(order[2], 3);
    }
  },
};

// releaseLock() with a write QUEUED behind an in-flight one.
// DIVERGENCE: C++ rejects the queued write with the released-writer
// error; under TypeScript the queued write stays PENDING FOREVER
// (bounded observation) — the release orphans it. The release itself
// succeeds and the stream is re-lockable in both (migrated from
// streams-test.js).
export const cancelWriteOnReleaseLock = {
  async test() {
    const ws = new WritableStream({
      write() {
        return new Promise(() => {});
      },
    });
    const writer = ws.getWriter();
    // The first write is in flight forever; the second is queued.
    writer.write('ignored').catch(() => {});
    const queued = writer.write('hello');
    writer.releaseLock();
    const outcome = await Promise.race([
      queued.then(
        () => ({ state: 'fulfilled' }),
        (e) => ({ state: 'rejected', message: e.message })
      ),
      scheduler.wait(250).then(() => ({ state: 'pending' })),
    ]);
    if (usingTsImpl) {
      strictEqual(outcome.state, 'pending');
    } else {
      strictEqual(outcome.state, 'rejected');
      strictEqual(
        outcome.message,
        'This WritableStream writer has been released.'
      );
    }
    ws.getWriter(); // re-lockable
  },
};
