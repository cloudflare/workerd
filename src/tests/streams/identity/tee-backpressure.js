// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// How writer-side backpressure is signaled across tee() branches. Shared by
// both implementations:
// - tee() by itself creates no demand: a write stays pending until some
//   branch actually reads.
// - Canceling one branch leaves the writer flowing to the survivor.
//
// When a write settles diverges (ledger #23):
// - C++: once one branch has read it; the sibling's buffered copy is not
//   counted against the writer's budget.
// - TypeScript: once the slowest branch has read it, so its bytes stay
//   counted in the writer's desiredSize until then — or once a branch is
//   STARVED (it has a pending read and has taken everything delivered). As
//   in the queue's pull rule, a pending read overrides backpressure, so an
//   idle branch buffers rather than stalling the writer.
//
// Two further aspects deliberately diverge:
// - The cancel promise of a single branch: C++ resolves it immediately;
//   TypeScript follows the WHATWG tee semantics, where both branches share
//   one cancel promise that settles only once BOTH branches have canceled.
// - A write after BOTH branches have canceled: C++ never propagates the
//   composite cancellation back to the writable, so the write parks forever;
//   TypeScript rejects it with an AggregateError ("All readable stream tee
//   branches were canceled").

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
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
    // A single branch read supplies the demand. Under TypeScript the write
    // then waits for the sibling (the slower branch) or a starved branch.
    const readerA = a.getReader();
    const { value, done } = await readerA.read();
    strictEqual(done, false);
    strictEqual(value[0], 1);
    await tick();
    strictEqual(writeResolved, !usingTsImpl);
    if (usingTsImpl) {
      readerA.read();
      await writePromise;
    }
    strictEqual(writeResolved, true);
  },
};

export const singleBranchReadDrivesWriter = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    let writeResolved = false;
    const writePromise = writer
      .write(new Uint8Array([7]))
      .then(() => (writeResolved = true));
    const readerA = a.getReader();
    const readA = await readerA.read();
    strictEqual(readA.value[0], 7);
    await tick();
    // C++: one branch's consumption completes the write. TypeScript: the
    // write waits for the unread sibling until branch A is starved — its
    // next read finds nothing left, which releases the write.
    strictEqual(writeResolved, !usingTsImpl);
    const nextA = readerA.read();
    await writePromise;
    // The sibling holds a buffered copy it can read later, with no further
    // writes.
    const readerB = b.getReader();
    const readB = await readerB.read();
    strictEqual(readB.value[0], 7);
    // Close drains through to both branches.
    const closePromise = writer.close();
    strictEqual((await nextA).done, true);
    strictEqual((await readerB.read()).done, true);
    await closePromise;
  },
};

export const writerDesiredSizeAcrossTee = {
  async test() {
    const its = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    strictEqual(writer.desiredSize, 10);
    const writePromise = writer.write(new Uint8Array(4));
    strictEqual(writer.desiredSize, 6);
    // C++: a single branch's read restores the budget in full; the
    // sibling's buffered copy is not counted. TypeScript: the bytes stay
    // counted until the sibling, the slower branch, has read them.
    const readerA = a.getReader();
    await readerA.read();
    await tick();
    strictEqual(writer.desiredSize, usingTsImpl ? 6 : 10);
    if (usingTsImpl) {
      await b.getReader().read();
    }
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

export const inFlightWriteRejectsWhenBranchesLeaveAfterRead = {
  async test() {
    // One branch reads the write; then every branch cancels in the same
    // turn. Under TypeScript each of the first two cancels advances the
    // slowest consumer, and the last takes the source down before that
    // progress is checked: the write must reject with the composite
    // cancellation (ledger #14), not settle as read. C++ settles the write
    // once one branch has read it.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, rest] = its.readable.tee();
    const [b, c] = rest.tee();
    const ra = a.getReader();
    const rb = b.getReader();
    const rc = c.getReader();
    const writePromise = writer.write(new Uint8Array([1, 2, 3]));
    const { value } = await ra.read();
    deepStrictEqual([...value], [1, 2, 3]);
    ra.cancel();
    rb.cancel();
    rc.cancel(new Error('c'));
    if (usingTsImpl) {
      await rejects(writePromise, (err) => {
        strictEqual(err.constructor, AggregateError);
        return /tee branches were canceled/.test(err.message);
      });
    } else {
      await writePromise;
    }
  },
};

export const abortBeforeStarvedReadRejectsInFlightWrite = {
  async test() {
    // Under TypeScript the write waits for the idle sibling. abort() and
    // then a read that finds the reading branch starved, in the same turn:
    // the writable is already erroring, so the write rejects with the
    // abort reason rather than being released by the starved read. C++
    // settles the write once one branch has read it.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, _b] = its.readable.tee();
    const ra = a.getReader();
    const writePromise = writer.write(new Uint8Array([5]));
    strictEqual((await ra.read()).value[0], 5);
    const reason = new Error('abort');
    const abortPromise = writer.abort(reason);
    ra.read().catch(() => {});
    if (usingTsImpl) {
      await rejects(writePromise, (err) => err === reason);
    } else {
      await writePromise;
    }
    await abortPromise;
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
