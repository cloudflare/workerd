// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How writer-side backpressure is signaled across tee() branches. The core
// semantics are shared by both implementations:
// - tee() by itself creates no demand: a write stays pending until some
//   branch actually reads.
// - A single branch's read is sufficient demand: it resolves the pending
//   write and restores the writer's desiredSize in full.
// - The other branch receives a buffered copy that is NOT counted against
//   the writer's backpressure budget.
// - Canceling one branch leaves the writer flowing to the survivor.
//
// Two aspects deliberately diverge and are asserted per implementation:
// - The cancel promise of a single branch: C++ resolves it immediately;
//   TypeScript follows the WHATWG tee semantics, where both branches share
//   one cancel promise that settles only once BOTH branches have canceled.
// - A write after BOTH branches have canceled: C++ never propagates the
//   composite cancellation back to the writable, so the write parks forever;
//   TypeScript rejects it with an AggregateError ("All readable stream tee
//   branches were canceled").

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Yields through the event loop (not just the microtask queue) so that any
// resolution in flight — including one arriving via the C++ event loop —
// has settled before we assert on pending-ness.
const tick = () => scheduler.wait(0);

export const teeCreatesNoDemand = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, _b] = its.readable.tee();
    let writeResolved = false;
    const writePromise = writer
      .write(new Uint8Array([1]))
      .then(() => (writeResolved = true));
    await tick();
    strictEqual(writeResolved, false, 'tee alone must not create demand');
    // A single branch read supplies the demand that completes the write.
    const { value, done } = await a.getReader().read();
    strictEqual(done, false);
    strictEqual(value[0], 1);
    await writePromise;
    strictEqual(writeResolved, true);
  },
};

export const singleBranchReadDrivesWriter = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const writePromise = writer.write(new Uint8Array([7]));
    const readerA = a.getReader();
    const readA = await readerA.read();
    strictEqual(readA.value[0], 7);
    // One branch's consumption is enough to complete the write...
    await writePromise;
    // ...and the sibling holds a buffered copy it can read later, with no
    // further writes.
    const readerB = b.getReader();
    const readB = await readerB.read();
    strictEqual(readB.value[0], 7);
    // Close drains through to both branches.
    const closePromise = writer.close();
    strictEqual((await readerA.read()).done, true);
    strictEqual((await readerB.read()).done, true);
    await closePromise;
  },
};

export const writerDesiredSizeAcrossTee = {
  async test() {
    const its = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = its.writable.getWriter();
    const [a, _b] = its.readable.tee();
    strictEqual(writer.desiredSize, 10);
    const writePromise = writer.write(new Uint8Array(4));
    strictEqual(writer.desiredSize, 6);
    // A single branch's read restores the budget in full: the sibling's
    // buffered copy is not counted against the writer.
    const readerA = a.getReader();
    await readerA.read();
    await writePromise;
    strictEqual(writer.desiredSize, 10);
  },
};

export const cancelOneBranchKeepsWriterFlowing = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();

    let cancelASettled = false;
    const cancelA = a.cancel(new Error('branch a done')).then(
      () => (cancelASettled = true),
      () => (cancelASettled = true)
    );
    await tick();
    if (usingTsImpl) {
      // WHATWG tee semantics: the branches share one cancel promise, which
      // settles only once both branches have canceled.
      strictEqual(cancelASettled, false);
    } else {
      strictEqual(cancelASettled, true);
    }

    // Writes keep flowing to the surviving branch.
    const writePromise = writer.write(new Uint8Array([42]));
    const readerB = b.getReader();
    const readB = await readerB.read();
    strictEqual(readB.done, false);
    strictEqual(readB.value[0], 42);
    await writePromise;

    // Once the sibling cancels too, the shared cancel promise settles in
    // both implementations.
    await readerB.cancel(new Error('branch b done'));
    await cancelA;
    strictEqual(cancelASettled, true);
  },
};

export const writeAfterBothBranchesCancel = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    await Promise.all([
      a.cancel(new Error('branch a done')),
      b.cancel(new Error('branch b done')),
    ]);
    const writePromise = writer.write(new Uint8Array([43]));
    if (usingTsImpl) {
      // The composite cancellation propagates to the writable side with an
      // aggregate of the branch reasons.
      await rejects(writePromise, (err) => {
        strictEqual(err.constructor, AggregateError);
        return /tee branches were canceled/.test(err.message);
      });
    } else {
      // C++ never propagates the composite cancellation back to the
      // writable: the write parks forever, neither delivering nor
      // rejecting. (The isolate tears the promise down at test end.)
      let settled = false;
      writePromise.then(
        () => (settled = true),
        () => (settled = true)
      );
      await tick();
      strictEqual(settled, false);
    }
  },
};
