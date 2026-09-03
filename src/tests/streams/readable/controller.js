// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDefaultController: enqueue/close/error and desiredSize
// accounting.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf } from 'helpers';

// desiredSize tracks hwm minus queued sizes during start() and recovers
// as reads drain the queue (migrated from streams-js-test.js
// readableGetDesiredSize, value-stream half).
export const desiredSizeAccounting = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
          strictEqual(c.desiredSize, 2);
          c.enqueue(1);
          strictEqual(c.desiredSize, 1);
          c.enqueue(2);
          strictEqual(c.desiredSize, 0);
          c.enqueue(3);
          strictEqual(c.desiredSize, -1);
        },
      },
      { highWaterMark: 2 }
    );
    await rs.getReader().read();
    strictEqual(controller.desiredSize, 0);
  },
};

// Enqueuing with a read already pending hands the chunk straight to the
// reader without touching the queue: desiredSize is unchanged (parity;
// migrated from streams-js-test.js).
export const enqueueWithPendingReadSkipsQueue = {
  async test() {
    let controller;
    const rs = new ReadableStream(
      {
        start(c) {
          controller = c;
        },
      },
      { highWaterMark: 2 }
    );
    const reader = rs.getReader();
    strictEqual(controller.desiredSize, 2);
    const read = reader.read();
    controller.enqueue(1);
    strictEqual(controller.desiredSize, 2);
    strictEqual((await read).value, 1);
  },
};

// desiredSize through terminal states: 0 after close, null after error,
// 0 after cancel (parity).
export const desiredSizeTerminalStates = {
  async test() {
    let c1;
    new ReadableStream({
      start(c) {
        c1 = c;
      },
    });
    strictEqual(c1.desiredSize, 1);
    c1.close();
    strictEqual(c1.desiredSize, 0);

    let c2;
    new ReadableStream({
      start(c) {
        c2 = c;
      },
    });
    c2.error(new Error('boom'));
    strictEqual(c2.desiredSize, null);

    let c3;
    const rs3 = new ReadableStream({
      start(c) {
        c3 = c;
      },
    });
    await rs3.cancel('why');
    strictEqual(c3.desiredSize, 0);
  },
};

// controller.error() rejects pending and subsequent reads with the very
// error object (parity; migrated from streams-js-test.js
// readableStreamControllerError).
export const controllerErrorRejectsReads = {
  async test() {
    const err = new Error('controller-error');
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    const pending = reader.read();
    controller.error(err);
    strictEqual(await rejectionOf(pending), err);
    strictEqual(await rejectionOf(reader.read()), err);
    strictEqual(await rejectionOf(reader.closed), err);
  },
};

// error() twice and error() after close are silent no-ops on both sides
// (the WPT bad-underlying-sources seeds fail on other grounds).
export const errorIdempotence = {
  test() {
    let c1;
    new ReadableStream({
      start(c) {
        c1 = c;
      },
    });
    c1.error(new Error('first'));
    c1.error(new Error('second')); // must not throw

    let c2;
    new ReadableStream({
      start(c) {
        c2 = c;
      },
    });
    c2.close();
    c2.error(new Error('after-close')); // must not throw
  },
};

// close() twice and enqueue() after close throw TypeError on both sides,
// with different messages.
export const closeTerminality = {
  test() {
    let controller;
    new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    controller.close();
    throws(() => controller.close(), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot close a stream that is already closed or closing'
        : 'This ReadableStream is closed.',
    });
    throws(() => controller.enqueue('x'), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot enqueue a chunk into a stream that is closed or closing'
        : 'Unable to enqueue',
    });
  },
};

// close() with chunks still queued lets them drain before done (parity).
export const closeDrainsQueue = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('a');
        c.enqueue('b');
        c.close();
      },
    });
    const reader = rs.getReader();
    strictEqual((await reader.read()).value, 'a');
    strictEqual((await reader.read()).value, 'b');
    strictEqual((await reader.read()).done, true);
  },
};

export const controllerType = {
  async test() {
    let c;
    new ReadableStream({
      start(ctrl) {
        c = ctrl;
      },
    });
    strictEqual(c instanceof ReadableStreamDefaultController, true);
  },
};
