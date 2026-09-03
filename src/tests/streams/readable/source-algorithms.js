// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Underlying source hook invocation: start/pull/cancel timing, error
// handling, and the pull-count shapes behind several WPT general.any
// C++ expectedFailures.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

// DIVERGENCE (WPT general.any 'should pull after start, and after every
// read'): both sides pull once after start; TypeScript then pulls after
// EVERY read (spec: counts 1,2,3), while C++ serves the first read from
// the queue without pulling and issues the deferred pulls together after
// the second read (counts 1,1,3).
export const pullCountShape = {
  async test() {
    const pulls = [];
    let c;
    const rs = new ReadableStream(
      {
        start(ctrl) {
          c = ctrl;
        },
        pull() {
          pulls.push('pull');
          c.enqueue(pulls.length);
        },
      },
      { highWaterMark: 1 }
    );
    await scheduler.wait(10);
    strictEqual(pulls.length, 1);
    const reader = rs.getReader();
    await reader.read();
    strictEqual(pulls.length, usingTsImpl ? 2 : 1);
    await reader.read();
    strictEqual(pulls.length, 3);
  },
};

// DIVERGENCE (WPT general.any 'should call pull in reaction to read()ing
// the last chunk'): with a chunk enqueued during start(), neither side
// pulls while the queue is non-empty; after the read drains the queue,
// TypeScript pulls (spec), C++ does not.
export const pullOnLastChunkRead = {
  async test() {
    let pulls = 0;
    const rs = new ReadableStream(
      {
        start(c) {
          c.enqueue('a');
        },
        pull(c) {
          pulls++;
          c.enqueue('p' + pulls);
        },
      },
      { highWaterMark: 1 }
    );
    await scheduler.wait(10);
    strictEqual(pulls, 0);
    const reader = rs.getReader();
    const r1 = await reader.read();
    strictEqual(r1.value, 'a');
    await scheduler.wait(10);
    strictEqual(pulls, usingTsImpl ? 1 : 0);
  },
};

// pull() is never re-entered: no second call until the previous pull's
// promise fulfills (parity; WPT lists it as a C++ failure but the
// observable serialization holds — the WPT assertion fails on the
// pull-count shapes above instead).
export const pullSerialized = {
  async test() {
    let pulls = 0;
    let releasePull;
    const rs = new ReadableStream(
      {
        pull(c) {
          pulls++;
          return new Promise((resolve) => {
            releasePull = () => {
              c.enqueue('x');
              resolve();
            };
          });
        },
      },
      { highWaterMark: 1 }
    );
    await scheduler.wait(10);
    strictEqual(pulls, 1);
    const reader = rs.getReader();
    const readP = reader.read();
    await scheduler.wait(10);
    strictEqual(pulls, 1);
    releasePull();
    await readP;
    await scheduler.wait(10);
    strictEqual(pulls, 2);
  },
};

// A rejected pull() errors the stream: reads reject with the SAME error
// object (parity, identity preserved).
export const pullRejectionErrorsStream = {
  async test() {
    const err = new Error('pull-reject');
    const rs = new ReadableStream({
      pull() {
        return Promise.reject(err);
      },
    });
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// A pull() that throws on its second invocation errors the stream after
// the first chunk is consumed (parity, identity).
export const pullThrowSecondCall = {
  async test() {
    const err = new Error('second-pull');
    let pulls = 0;
    const rs = new ReadableStream(
      {
        pull(c) {
          if (++pulls === 1) c.enqueue('a');
          else throw err;
        },
      },
      { highWaterMark: 1 }
    );
    const reader = rs.getReader();
    const r1 = await reader.read();
    strictEqual(r1.value, 'a');
    strictEqual(await rejectionOf(reader.read()), err);
    strictEqual(pulls, 2);
  },
};

// DIVERGENCE (the transform suite's ledger #1 mirrored): a synchronously
// throwing start() escapes the constructor under TypeScript (spec) but
// is captured by C++ — construction succeeds and the stream is errored.
export const syncStartThrow = {
  async test() {
    const err = new Error('start-throw');
    if (usingTsImpl) {
      let caught;
      try {
        new ReadableStream({
          start() {
            throw err;
          },
        });
      } catch (e) {
        caught = e;
      }
      strictEqual(caught, err);
    } else {
      const rs = new ReadableStream({
        start() {
          throw err;
        },
      });
      strictEqual(await rejectionOf(rs.getReader().read()), err);
    }
  },
};

// An async start() rejection errors the stream on both sides: reads
// reject with the same error object.
export const asyncStartRejectionErrorsStream = {
  async test() {
    const err = new Error('start-reject');
    const rs = new ReadableStream({
      async start() {
        throw err;
      },
    });
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// cancel() runs even with a pull pending, receives exactly the cancel
// reason, and the cancel promise fulfills (parity; the WPT cancel.any
// seed).
export const cancelWithPendingPull = {
  async test() {
    const reason = new Error('cancel-reason');
    let pullStarted = false;
    let cancelArgs;
    const rs = new ReadableStream(
      {
        pull() {
          pullStarted = true;
          return new Promise(() => {});
        },
        cancel(...args) {
          cancelArgs = args;
        },
      },
      { highWaterMark: 1 }
    );
    await scheduler.wait(10);
    strictEqual(pullStarted, true);
    strictEqual(await rs.cancel(reason), undefined);
    strictEqual(cancelArgs.length, 1);
    strictEqual(cancelArgs[0], reason);
  },
};
