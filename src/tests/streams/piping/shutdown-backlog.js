// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A pipe that shuts down while its source still has chunks buffered. The
// pipe reads only what the destination desires, so the sink gets nothing
// beyond the writes made before the shutdown, and the chunks never read
// stay in the source. Every await is bounded.
//
// DIVERGENCES: with a destination highWaterMark above 1, TypeScript fills
// the destination queue before the shutdown and waits for those writes
// (spec); C++ reads and writes one chunk at a time (ledger #11). After a
// non-fatal write rejection C++ has already read, and drops, the next
// chunk (ledger #12).

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

const backlogSource = (n) => {
  let controller;
  const stream = new ReadableStream({
    start(c) {
      controller = c;
      for (let i = 0; i < n; i++) c.enqueue(i);
    },
  });
  return { stream, controller };
};

// A sink whose first write stays in flight until released.
const stallingSink = (strategy) => {
  const wrote = [];
  const aborts = [];
  let releaseWrite;
  const stalled = new Promise((resolve) => (releaseWrite = resolve));
  const stream = new WritableStream(
    {
      write(chunk) {
        wrote.push(chunk);
        if (wrote.length === 1) return stalled;
        return undefined;
      },
      abort(reason) {
        aborts.push(reason);
      },
    },
    strategy
  );
  return { stream, wrote, aborts, release: () => releaseWrite() };
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

// Aborting through the signal writes nothing more, and with preventCancel
// every chunk not written stays readable, in order.
export const abortWithBacklogKeepsUnwrittenChunks = {
  async test() {
    const { stream: rs } = backlogSource(8);
    const sink = stallingSink();
    const ac = new AbortController();
    const reason = new Error('stop');
    const pipeP = rs.pipeTo(sink.stream, {
      signal: ac.signal,
      preventCancel: true,
    });
    await scheduler.wait(10);
    ac.abort(reason);
    sink.release();
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, reason);
    deepStrictEqual(sink.wrote, [0]);
    deepStrictEqual(sink.aborts, [reason]);
    deepStrictEqual(await readValues(rs, 7), [1, 2, 3, 4, 5, 6, 7]);
  },
};

// With a destination highWaterMark of 3 the abort waits for the writes
// already queued (ledger #11).
export const abortWithBacklogHighWaterMark = {
  async test() {
    const { stream: rs } = backlogSource(8);
    const sink = stallingSink({ highWaterMark: 3 });
    const ac = new AbortController();
    const reason = new Error('stop');
    const pipeP = rs.pipeTo(sink.stream, {
      signal: ac.signal,
      preventCancel: true,
    });
    await scheduler.wait(10);
    deepStrictEqual(sink.wrote, [0]);
    ac.abort(reason);
    sink.release();
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, reason);
    deepStrictEqual(sink.aborts, [reason]);
    if (usingTsImpl) {
      deepStrictEqual(sink.wrote, [0, 1, 2]);
      deepStrictEqual(await readValues(rs, 5), [3, 4, 5, 6, 7]);
    } else {
      deepStrictEqual(sink.wrote, [0]);
      deepStrictEqual(await readValues(rs, 7), [1, 2, 3, 4, 5, 6, 7]);
    }
  },
};

// A source error aborts the destination once the in-flight write settles;
// the backlog the error discarded is never written.
export const sourceErrorWithBacklogWritesNoMore = {
  async test() {
    const { stream: rs, controller } = backlogSource(8);
    const sink = stallingSink();
    const pipeP = rs.pipeTo(sink.stream);
    await scheduler.wait(10);
    const err = new Error('src-err');
    controller.error(err);
    sink.release();
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason, err);
    deepStrictEqual(sink.wrote, [0]);
    deepStrictEqual(sink.aborts, [err]);
  },
};

// An invalid chunk's non-fatal write rejection ends the pipe: nothing after
// it reaches the identity stream's readable, and with preventCancel the
// chunks after it stay in the source (ledger #12).
export const invalidChunkWithBacklogEndsPipe = {
  async test() {
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('before'));
        c.enqueue(42);
        c.enqueue('x1');
        c.enqueue('x2');
        c.enqueue('x3');
      },
    });
    const { readable, writable } = new IdentityTransformStream();
    const pipeP = rs.pipeTo(writable, { preventCancel: true });
    const reader = readable.getReader();
    strictEqual(dec.decode((await reader.read()).value), 'before');
    await rejects(reader.read(), { name: 'TypeError' });
    const outcome = await outcomeOf(pipeP);
    strictEqual(outcome.state, 'rejected');
    strictEqual(outcome.reason.name, 'TypeError');
    const remaining = usingTsImpl ? ['x1', 'x2', 'x3'] : ['x2', 'x3'];
    deepStrictEqual(await readValues(rs, remaining.length), remaining);
  },
};
