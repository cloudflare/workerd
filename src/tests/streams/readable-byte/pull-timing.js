// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Automatic pull() scheduling for byte sources (the WPT general.any
// "Automatic pull()" seeds) and pull error handling.

import { strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

// DIVERGENCE (same shape as the readable suite's ledger #4): both sides
// pull once after start; TypeScript pulls after every read (1,2,3), C++
// serves the first read from the queue and batches the deferred pulls
// (1,1,3).
export const pullCountShape = {
  async test() {
    let pulls = 0;
    let c;
    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(ctrl) {
          c = ctrl;
        },
        pull() {
          pulls++;
          c.enqueue(new Uint8Array([pulls]));
        },
      },
      { highWaterMark: 1 }
    );
    await scheduler.wait(10);
    strictEqual(pulls, 1);
    const reader = rs.getReader();
    strictEqual((await reader.read()).value[0], 1);
    strictEqual(pulls, usingTsImpl ? 2 : 1);
    strictEqual((await reader.read()).value[0], 2);
    strictEqual(pulls, 3);
  },
};

// A throwing pull() errors the stream with the very error (parity).
export const pullThrowErrorsStream = {
  async test() {
    const err = new Error('pull-throw');
    const rs = new ReadableStream({
      type: 'bytes',
      pull() {
        throw err;
      },
    });
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// A pull() that errors the stream and THEN throws keeps the error()
// reason: the throw is ignored (parity; the WPT general.any seed).
export const pullThrowIgnoredIfErrored = {
  async test() {
    const err = new Error('stream-errored');
    const rs = new ReadableStream({
      type: 'bytes',
      pull(c) {
        c.error(err);
        throw new Error('ignored');
      },
    });
    strictEqual(await rejectionOf(rs.getReader().read()), err);
  },
};

// Byte-stream highWaterMark is measured in BYTES: a hwm of 10 with 3-byte
// enqueues pulls until desiredSize crosses zero (4 pulls, 12 bytes,
// desiredSize -2). Delivery of the buffered bytes then diverges per the
// suite's coalescing finding (ledger #17): C++ hands all 12 bytes to the
// first read, TypeScript delivers the first 3-byte chunk (migrated from
// streams-backpressure-test.js, strengthened to exact counts).
export const backpressureByteStreamHwm = {
  async test() {
    let controller;
    let pullCount = 0;
    const rs = new ReadableStream(
      {
        type: 'bytes',
        start(c) {
          controller = c;
          strictEqual(c.desiredSize, 10);
        },
        pull(c) {
          pullCount++;
          c.enqueue(new Uint8Array([1, 2, 3]));
        },
      },
      { highWaterMark: 10 }
    );
    await scheduler.wait(20);
    strictEqual(pullCount, 4);
    strictEqual(controller.desiredSize, -2);
    const reader = rs.getReader();
    const { value } = await reader.read();
    strictEqual(value.byteLength, usingTsImpl ? 3 : 12);
    reader.releaseLock();
  },
};

// DIVERGENCE (ledger #33; the readable suite's ledger #23 for a byte
// source): the controller settles a new promise with start()'s result, so
// a returned promise starts the stream later than adopting it would:
// the first pull runs after the second marker chained on it (spec; Node
// agrees). C++ adopts the promise and pulls before the first marker.
export const startPromiseSettledInNewPromise = {
  async test() {
    const expected = usingTsImpl ? '1,2,pull,3,4' : 'pull,1,2,3,4';
    // start() returning a fulfilled promise, and one fulfilled later.
    for (const pending of [false, true]) {
      const log = [];
      const { promise, resolve } = Promise.withResolvers();
      if (!pending) resolve();
      new ReadableStream(
        {
          type: 'bytes',
          start: () => promise,
          pull() {
            log.push('pull');
          },
        },
        { highWaterMark: 1 }
      );
      let p = promise;
      for (let i = 1; i <= 4; i++) {
        p = p.then(() => log.push(i));
      }
      if (pending) {
        await null;
        resolve();
      }
      await scheduler.wait(1);
      strictEqual(log.join(','), expected, `pending: ${pending}`);
    }
  },
};
