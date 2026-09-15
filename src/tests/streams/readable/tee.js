// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream.tee(): branch independence, cancellation composition,
// and error propagation. The value-stream halves of
// streams-tee-edge-cases-test.js live here (the byte-stream tee tests
// belong to the readable-byte suite); the WPT tee.any C++
// expectedFailures narrow mostly to the cancel-reason composite and
// pull-timing bookkeeping.

import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { drainToArray } from 'helpers';

// Consuming one branch fully does not disturb the other; both see every
// chunk (parity; migrated).
export const teeConsumeOneBranchFully = {
  async test() {
    let pullCount = 0;
    const rs = new ReadableStream({
      pull(controller) {
        pullCount++;
        if (pullCount <= 5) controller.enqueue(pullCount);
        else controller.close();
      },
    });
    const [branch1, branch2] = rs.tee();
    deepStrictEqual(await drainToArray(branch1), [1, 2, 3, 4, 5]);
    ok(!branch2.locked);
    deepStrictEqual(await drainToArray(branch2), [1, 2, 3, 4, 5]);
  },
};

// Asymmetric read rates: both branches still see every chunk in order
// (parity; migrated).
export const teeDifferentReadRates = {
  async test() {
    let counter = 0;
    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 10) controller.enqueue(counter);
        else controller.close();
      },
    });
    const [branch1, branch2] = rs.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();
    const results1 = [];
    const results2 = [];
    for (let i = 0; i < 10; i++) {
      results1.push((await reader1.read()).value);
      if (i % 2 === 1) results2.push((await reader2.read()).value);
    }
    for (;;) {
      const { value, done } = await reader2.read();
      if (done) break;
      results2.push(value);
    }
    for (;;) {
      const { value, done } = await reader1.read();
      if (done) break;
      results1.push(value);
    }
    deepStrictEqual(results1, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    deepStrictEqual(results2, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
  },
};

// Canceling one branch keeps the source alive for the other (parity;
// migrated).
export const teeCancelSlowBranch = {
  async test() {
    let counter = 0;
    let sourceCancelled = false;
    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 20) controller.enqueue(counter);
        else controller.close();
      },
      cancel() {
        sourceCancelled = true;
      },
    });
    const [branch1, branch2] = rs.tee();
    const reader1 = branch1.getReader();
    const reader2 = branch2.getReader();
    for (let i = 0; i < 5; i++) {
      await reader1.read();
      await reader2.read();
    }
    // A lone branch's cancel promise stays pending until the OTHER branch
    // cancels under TypeScript (spec; pinned in teeCancelReasonComposite),
    // so it must not be awaited here.
    const cancel2 = reader2.cancel('No longer needed');
    if (!usingTsImpl) await cancel2;
    await scheduler.wait(10);
    ok(!sourceCancelled);
    const remaining = [];
    for (;;) {
      const { value, done } = await reader1.read();
      if (done) break;
      remaining.push(value);
    }
    strictEqual(remaining.length, 15);
    strictEqual(remaining[0], 6);
    strictEqual(remaining[14], 20);
  },
};

// High chunk counts flow through both branches intact (parity;
// migrated).
export const teeLargeChunkCount = {
  async test() {
    const CHUNK_COUNT = 1000;
    let counter = 0;
    const rs = new ReadableStream({
      pull(controller) {
        if (counter < CHUNK_COUNT) controller.enqueue(counter++);
        else controller.close();
      },
    });
    const [branch1, branch2] = rs.tee();
    async function consumeBranch(branch) {
      let count = 0;
      let sum = 0;
      for (const value of await drainToArray(branch)) {
        count++;
        sum += value;
      }
      return { count, sum };
    }
    const [result1, result2] = await Promise.all([
      consumeBranch(branch1),
      consumeBranch(branch2),
    ]);
    strictEqual(result1.count, CHUNK_COUNT);
    strictEqual(result2.count, CHUNK_COUNT);
    strictEqual(result1.sum, 499500);
    strictEqual(result2.sum, 499500);
  },
};

// tee() after a partial read resumes from the current position on both
// branches (parity; migrated).
export const teeAfterPartialRead = {
  async test() {
    let counter = 0;
    const rs = new ReadableStream({
      pull(controller) {
        counter++;
        if (counter <= 10) controller.enqueue(counter);
        else controller.close();
      },
    });
    const originalReader = rs.getReader();
    strictEqual((await originalReader.read()).value, 1);
    originalReader.releaseLock();
    const [branch1, branch2] = rs.tee();
    strictEqual((await branch1.getReader().read()).value, 2);
    strictEqual((await branch2.getReader().read()).value, 2);
  },
};

// Erroring the source propagates the very error object to pending reads
// on BOTH branches (parity; the WPT tee.any seed fails on recording
// bookkeeping, not the propagation itself).
export const teeErrorPropagatesBothBranches = {
  async test() {
    const err = new Error('src-err');
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const [b1, b2] = rs.tee();
    const read1 = b1.getReader().read();
    const read2 = b2.getReader().read();
    controller.error(err);
    let e1, e2;
    await read1.catch((e) => (e1 = e));
    await read2.catch((e) => (e2 = e));
    strictEqual(e1, err);
    strictEqual(e2, err);
  },
};

// DIVERGENCE (the WPT tee.any cancel-aggregation seeds, including the
// two tests that HANG the TypeScript WPT run): when both branches
// cancel, the spec passes the source an ARRAY [reason1, reason2].
// TypeScript intentionally passes an AggregateError carrying both
// reasons (in branch order, by identity), and the FIRST branch's cancel
// promise stays pending until the second branch cancels. C++ runs the
// source cancel with only the reason of whichever branch completed the
// pair, and the first branch's cancel promise fulfills immediately.
export const teeCancelReasonComposite = {
  async test() {
    const r1 = { branch: 1 };
    const r2 = { branch: 2 };
    let cancelReason = 'not-called';
    const rs = new ReadableStream({
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const [b1, b2] = rs.tee();

    let c1State = 'pending';
    const c1 = b1.cancel(r1).then(() => (c1State = 'fulfilled'));
    await scheduler.wait(10);
    strictEqual(cancelReason, 'not-called');
    strictEqual(c1State, usingTsImpl ? 'pending' : 'fulfilled');

    await b2.cancel(r2);
    await c1;
    strictEqual(c1State, 'fulfilled');
    if (usingTsImpl) {
      ok(cancelReason instanceof AggregateError);
      strictEqual(cancelReason.errors.length, 2);
      strictEqual(cancelReason.errors[0], r1);
      strictEqual(cancelReason.errors[1], r2);
    } else {
      strictEqual(cancelReason, r2);
    }
  },
};

// The C++ single-reason behavior is order-dependent: whichever branch
// completes the pair supplies the reason (reversed order pinned here;
// TypeScript aggregates in branch order regardless).
export const teeCancelReverseOrder = {
  async test() {
    const r1 = { branch: 1 };
    const r2 = { branch: 2 };
    let cancelReason = 'not-called';
    const rs = new ReadableStream({
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const [b1, b2] = rs.tee();
    b2.cancel(r2);
    await b1.cancel(r1);
    await scheduler.wait(10);
    if (usingTsImpl) {
      ok(cancelReason instanceof AggregateError);
      strictEqual(cancelReason.errors[0], r1);
      strictEqual(cancelReason.errors[1], r2);
    } else {
      strictEqual(cancelReason, r1);
    }
  },
};

// Reading one branch drives exactly one pull per read while the other
// branch's queue buffers (parity on this shape).
export const teePullPerRead = {
  async test() {
    let pulls = 0;
    const rs = new ReadableStream(
      {
        pull(c) {
          pulls++;
          c.enqueue(pulls);
        },
      },
      { highWaterMark: 0 }
    );
    const [b1] = rs.tee();
    await scheduler.wait(10);
    strictEqual(pulls, 0);
    const reader1 = b1.getReader();
    for (let i = 1; i <= 3; i++) {
      strictEqual((await reader1.read()).value, i);
    }
    await scheduler.wait(10);
    strictEqual(pulls, 3);
  },
};
