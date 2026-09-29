// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A pipe aborted while it waits on a read, and the chunk that reaches the
// source afterwards. The spec writes a chunk read before the shutdown's
// action and waits for its write; once the pipe has released the source,
// later chunks stay readable. Every await is bounded.
//
// DIVERGENCES: a chunk that arrives while the abort waits for an in-flight
// write is written on TypeScript (spec); C++ has not read it, and it stays
// in the source (ledger #14). With no write in flight TypeScript settles
// the pipe and a later chunk stays readable (spec); C++ keeps the pipe and
// its read pending until a chunk arrives, and drops that chunk (ledger #15).
//
// PARITY OF NONCONFORMANCE: a chunk enqueued in the abort's turn, with no
// write in flight, is lost from both ends on both implementations. The spec
// writes it: its read steps run synchronously, before the shutdown finishes
// waiting. TypeScript delivers the read result asynchronously, after the
// wait has already ended.

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

const pushSource = (initial = []) => {
  let controller;
  const stream = new ReadableStream({
    start(c) {
      controller = c;
      for (const chunk of initial) c.enqueue(chunk);
    },
  });
  return { stream, enqueue: (chunk) => controller.enqueue(chunk) };
};

// A sink whose writes each stay in flight until released, in order.
const gatedSink = (strategy) => {
  const wrote = [];
  const gates = [];
  const stream = new WritableStream(
    {
      write(chunk) {
        wrote.push(chunk);
        return new Promise((resolve) => gates.push(resolve));
      },
    },
    strategy
  );
  return { stream, wrote, releaseNext: () => gates.shift()() };
};

// Reads up to `count` values, stopping at a read that stays pending.
const readValues = async (stream, count) => {
  const reader = stream.getReader();
  const values = [];
  for (let i = 0; i < count; i++) {
    const read = await outcomeOf(reader.read());
    if (read.state !== 'fulfilled' || read.value.done) break;
    values.push(read.value.value);
  }
  reader.releaseLock();
  return values;
};

const pipeOptions = (signal) => ({
  signal,
  preventAbort: true,
  preventCancel: true,
});

// With a destination highWaterMark of 2 the pipe reads again while chunk
// 0's write is in flight. On TypeScript a chunk reaching that read during
// the abort's wait is written, and the pipe settles only once that write
// has too (ledger #14).
export const lateChunkDuringShutdownWait = {
  async test() {
    const source = pushSource([0]);
    const sink = gatedSink({ highWaterMark: 2 });
    const ac = new AbortController();
    const reason = new Error('stop');
    const pipeP = source.stream.pipeTo(sink.stream, pipeOptions(ac.signal));
    await scheduler.wait(10);
    deepStrictEqual(sink.wrote, [0]);
    ac.abort(reason);
    await scheduler.wait(10);
    source.enqueue('late');
    await scheduler.wait(10);
    sink.releaseNext();
    await scheduler.wait(10);
    if (usingTsImpl) {
      deepStrictEqual(sink.wrote, [0, 'late']);
      strictEqual((await outcomeOf(pipeP, 10)).state, 'pending');
      sink.releaseNext();
    } else {
      deepStrictEqual(sink.wrote, [0]);
    }
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, reason);
    const remaining = usingTsImpl ? [] : ['late'];
    deepStrictEqual(await readValues(source.stream, 1), remaining);
  },
};

// No write in flight: the abort's wait ends before a chunk enqueued in the
// abort's turn reaches the pipe, so the chunk is neither written nor left
// in the source.
export const lateChunkInAbortTurnIsLost = {
  async test() {
    const source = pushSource();
    const sink = gatedSink();
    const ac = new AbortController();
    const reason = new Error('stop');
    const pipeP = source.stream.pipeTo(sink.stream, pipeOptions(ac.signal));
    await scheduler.wait(10);
    ac.abort(reason);
    source.enqueue('late');
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, reason);
    deepStrictEqual(sink.wrote, []);
    deepStrictEqual(await readValues(source.stream, 1), []);
  },
};

// No write in flight, and the chunk arrives a turn after the abort. On
// TypeScript the pipe has settled and released the source by then, so the
// chunk stays readable; C++'s pipe is still reading (ledger #15).
export const lateChunkAfterIdleAbort = {
  async test() {
    const source = pushSource();
    const sink = gatedSink();
    const ac = new AbortController();
    const reason = new Error('stop');
    const pipeP = source.stream.pipeTo(sink.stream, pipeOptions(ac.signal));
    await scheduler.wait(10);
    ac.abort(reason);
    await scheduler.wait(10);
    strictEqual(
      (await outcomeOf(pipeP, 10)).state,
      usingTsImpl ? 'rejected' : 'pending'
    );
    strictEqual(source.stream.locked, !usingTsImpl);
    source.enqueue('late');
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, reason);
    deepStrictEqual(sink.wrote, []);
    const remaining = usingTsImpl ? ['late'] : [];
    deepStrictEqual(await readValues(source.stream, 1), remaining);
  },
};
