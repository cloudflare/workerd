// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// BYOB reads on tee() branches of identity streams. Under TypeScript
// (ledger #23) a branch's BYOB read spans the writes already made, like a
// direct reader's (ledger #22), and a write settles once the slowest branch
// has read its last byte — or once a branch is starved (see
// tee-backpressure.js), so an idle branch never stalls the writer. Under
// C++ a branch's read is capped by the sizes the first-reading branch
// requested, and a write settles once that branch has read it.

import { deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const tick = () => scheduler.wait(5);

function settleFlags(promises) {
  const flags = promises.map(() => false);
  promises.forEach((p, i) =>
    p.then(
      () => (flags[i] = true),
      () => (flags[i] = 'rejected')
    )
  );
  return flags;
}

// Reads with the given view size, or reports 'pending' after a macrotask.
async function readStep(reader, size) {
  const result = await Promise.race([
    size === undefined ? reader.read() : reader.read(new Uint8Array(size)),
    tick().then(() => 'pending'),
  ]);
  await tick();
  if (result === 'pending') return 'pending';
  return result.done ? 'done' : [...result.value].join('');
}

// Runs `steps` ([reader, viewSize or undefined for a default read, label])
// and logs each read's bytes with every write's settlement so far.
async function runSteps(steps, settled, extra = () => []) {
  const log = [];
  for (const [reader, size, label] of steps) {
    log.push([label, await readStep(reader, size), [...settled], ...extra()]);
  }
  return log;
}

export const teeByobBranchesReadDifferentSizes = {
  async test() {
    for (const make of [
      () => new IdentityTransformStream(),
      () => new FixedLengthStream(15),
    ]) {
      const stream = make();
      const writer = stream.writable.getWriter();
      const [a, b] = stream.readable.tee();
      const ra = a.getReader({ mode: 'byob' });
      const rb = b.getReader({ mode: 'byob' });
      const settled = settleFlags([
        writer.write(new Uint8Array(10).fill(1)),
        writer.write(new Uint8Array(5).fill(2)),
      ]);
      const log = await runSteps(
        [
          [ra, 3, 'A3'],
          [rb, 7, 'B7'],
          [ra, 12, 'A12'],
          [rb, 12, 'B12'],
        ],
        settled
      );
      deepStrictEqual(
        log,
        usingTsImpl
          ? [
              ['A3', '111', [false, false]],
              ['B7', '1111111', [false, false]],
              ['A12', '111111122222', [false, false]],
              ['B12', '11122222', [true, true]],
            ]
          : [
              ['A3', '111', [false, false]],
              ['B7', '111', [false, false]],
              ['A12', '1111111', [true, false]],
              ['B12', '1111111', [true, false]],
            ]
      );
      if (!usingTsImpl) {
        deepStrictEqual(
          await runSteps(
            [
              [ra, 12, 'A12'],
              [rb, 12, 'B12'],
            ],
            settled
          ),
          [
            ['A12', '22222', [true, true]],
            ['B12', '22222', [true, true]],
          ]
        );
      }
      writer.close();
      deepStrictEqual(
        [await readStep(ra, 4), await readStep(rb, 4)],
        ['done', 'done']
      );
    }
  },
};

// A lagging branch holds the writer's desiredSize: the bytes count until the
// slowest branch has read them (TypeScript), or until the first-reading
// branch has (C++).
export const teeLaggingBranchHoldsWriterDesiredSize = {
  async test() {
    const its = new IdentityTransformStream({ highWaterMark: 20 });
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const ra = a.getReader({ mode: 'byob' });
    const rb = b.getReader({ mode: 'byob' });
    const settled = settleFlags([
      writer.write(new Uint8Array(10).fill(1)),
      writer.write(new Uint8Array(5).fill(2)),
    ]);
    deepStrictEqual(writer.desiredSize, 5);
    const log = await runSteps(
      [
        [ra, 20, 'A20'],
        [rb, 4, 'B4'],
        [rb, 20, 'B20'],
      ],
      settled,
      () => [writer.desiredSize]
    );
    deepStrictEqual(
      log,
      usingTsImpl
        ? [
            ['A20', '1'.repeat(10) + '22222', [false, false], 5],
            ['B4', '1111', [false, false], 5],
            ['B20', '111111' + '22222', [true, true], 20],
          ]
        : [
            ['A20', '1'.repeat(10), [true, false], 15],
            ['B4', '1111', [true, false], 15],
            ['B20', '111111', [true, false], 15],
          ]
    );
    if (!usingTsImpl) {
      deepStrictEqual(await runSteps([[rb, 20, 'B20']], settled), [
        ['B20', '22222', [true, true]],
      ]);
      deepStrictEqual(writer.desiredSize, 20);
    }
  },
};

// Cancelling the slower branch settles what the survivor has read.
export const teeCancelSlowerBranchSettlesWrites = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const ra = a.getReader({ mode: 'byob' });
    const settled = settleFlags([writer.write(new Uint8Array([1, 2, 3]))]);
    deepStrictEqual(await runSteps([[ra, 8, 'A8']], settled), [
      ['A8', '123', [!usingTsImpl]],
    ]);
    // Not awaited: under TypeScript one branch's cancel promise settles
    // only once both branches have cancelled (tee-backpressure.js).
    b.cancel(new Error('b done'));
    await tick();
    deepStrictEqual(settled, [true]);
  },
};

// A BYOB branch spans writes while a default-reader branch still takes one
// write per read.
export const teeMixedByobAndDefaultReaders = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const ra = a.getReader({ mode: 'byob' });
    const rb = b.getReader();
    const settled = settleFlags([
      writer.write(new Uint8Array([1, 1, 1, 1])),
      writer.write(new Uint8Array([2, 2, 2, 2])),
    ]);
    const log = await runSteps(
      [
        [ra, 8, 'A8'],
        [rb, undefined, 'B'],
        [rb, undefined, 'B'],
      ],
      settled
    );
    deepStrictEqual(
      log,
      usingTsImpl
        ? [
            ['A8', '11112222', [false, false]],
            ['B', '1111', [true, false]],
            ['B', '2222', [true, true]],
          ]
        : [
            ['A8', '1111', [true, false]],
            ['B', '1111', [true, false]],
            ['B', '2222', [true, true]],
          ]
    );
  },
};

// Nested tees: every leaf reads the whole content with its own view size,
// and the writes and close settle once all leaves have read everything.
export const nestedTeeByobLeavesReadDifferentSizes = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const [a, b] = its.readable.tee();
    const [c, d] = a.tee();
    const writes = [
      writer.write(new TextEncoder().encode('nested ')),
      writer.write(new TextEncoder().encode('byob ')),
      writer.write(new TextEncoder().encode('tee')),
      writer.close(),
    ];
    const readAll = async (branch, size) => {
      const reader = branch.getReader({ mode: 'byob' });
      const dec = new TextDecoder();
      let text = '';
      for (;;) {
        const { value, done } = await reader.read(new Uint8Array(size));
        if (done) return text + dec.decode();
        text += dec.decode(value, { stream: true });
      }
    };
    deepStrictEqual(
      await Promise.all([readAll(b, 2), readAll(c, 5), readAll(d, 16)]),
      ['nested byob tee', 'nested byob tee', 'nested byob tee']
    );
    await Promise.all(writes);
  },
};
