// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for https://github.com/cloudflare/workerd/issues/8901.
//
// WritableStreamJsController::pipeLoop() used to start each iteration by returning the next
// iteration's promise from the previous iteration's continuation. Every iteration's promise was
// then resolved with the next one's, so a pipe kept a few promises alive per chunk until it ended.
//
// The chain isn't reachable from JS, but its length is observable: once the last iteration
// settles, the result has to travel back through every link, one microtask per link, before
// pipeTo() settles. So this test counts the microtasks between the destination's close() and
// pipeTo() settling, and checks that the count doesn't depend on how many chunks were piped.

import { deepStrictEqual, rejects, strictEqual } from 'node:assert';

// Bounds the count, so a pipe that never settles fails the test instead of hanging it.
const maxTicks = 100_000;

async function ticksFromCloseToSettle(chunkCount) {
  let sent = 0;
  const source = new ReadableStream({
    pull(controller) {
      if (sent < chunkCount) {
        controller.enqueue(sent++);
      } else {
        controller.close();
      }
    },
  });

  let settled = false;
  let ticks = 0;
  const tick = () => {
    if (settled || ticks === maxTicks) return;
    ticks++;
    queueMicrotask(tick);
  };
  const destination = new WritableStream({
    write() {},
    close() {
      queueMicrotask(tick);
    },
  });

  try {
    await source.pipeTo(destination);
  } finally {
    settled = true;
  }
  return ticks;
}

export const pipeToSettlesInTicksIndependentOfChunkCount = {
  async test() {
    const oneChunk = await ticksFromCloseToSettle(1);
    const manyChunks = await ticksFromCloseToSettle(1000);
    strictEqual(manyChunks, oneChunk);
  },
};

// A pipe that ends on its first iteration, here because the destination errored in start(), used
// to hand its rejected outcome straight to the handlers that settle pipeTo(). Resolving another
// promise with it instead attaches those handlers a microtask later, and with
// unhandled_rejection_after_microtask_checkpoint off (as at this test's default compatibility
// date), the rejection is reported as unhandled before then, though the caller handles pipeTo().
export const pipeToErroredDestinationReportsNoUnhandledRejection = {
  async test() {
    const reasons = [];
    const handler = (event) => {
      reasons.push(String(event.reason));
      event.preventDefault();
    };
    addEventListener('unhandledrejection', handler);
    try {
      const source = new ReadableStream({
        pull(controller) {
          controller.enqueue(0);
        },
      });
      const destination = new WritableStream({
        start(controller) {
          controller.error(new Error('early'));
        },
      });
      await rejects(source.pipeTo(destination), { message: 'early' });
      await scheduler.wait(0);
      deepStrictEqual(reasons, []);
    } finally {
      removeEventListener('unhandledrejection', handler);
    }
  },
};
