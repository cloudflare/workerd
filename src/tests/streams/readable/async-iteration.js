// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Async iteration over ReadableStream: for-await, the iterator protocol
// (return/next interleavings), preventCancel, and the iterator's shape.
// The seven protocol tests are migrated from
// streams-async-iterator-test.js; the interleavings and the
// prototype-shape divergence are pinned from probes. Plain no-await
// interleavings are parity. Ledger #22: WebIDL clears the iterator's
// ongoing promise whenever a next() settles, so a continuation that was
// registered on that next() before a later call runs with no ongoing
// promise, and its next() reads ahead of the queued one; a next() after
// a rejected one reports done (one of the WPT async-iterator.any C++
// expectedFailures; the read-ahead shapes are derived from the WebIDL
// algorithm, not WPT cases). TS follows the spec; C++ serializes every
// call.

import { strictEqual, ok, rejects, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Test that breaking out of for-await-of cancels the stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorBreakCancels = {
  async test() {
    let cancelled = false;
    let cancelReason = null;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel(reason) {
        cancelled = true;
        cancelReason = reason;
      },
    });

    const values = [];
    for await (const chunk of rs) {
      values.push(chunk);
      if (values.length === 3) {
        break;
      }
    }

    strictEqual(values.length, 3);
    ok(cancelled, 'stream should be cancelled after break');
    strictEqual(cancelReason, undefined);
  },
};

// Test calling return() explicitly on async iterator
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorReturnMethod = {
  async test() {
    let cancelled = false;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel() {
        cancelled = true;
      },
    });

    const iterator = rs[Symbol.asyncIterator]();

    const first = await iterator.next();
    strictEqual(first.value, 'chunk');
    strictEqual(first.done, false);

    const returnResult = await iterator.return('finished');
    strictEqual(returnResult.done, true);

    ok(cancelled, 'stream should be cancelled after return()');
  },
};

// Test that return() followed by next() returns done
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorReturnThenNext = {
  async test() {
    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
    });

    const iterator = rs[Symbol.asyncIterator]();

    await iterator.next();
    await iterator.return();

    const result = await iterator.next();
    strictEqual(result.done, true);
    strictEqual(result.value, undefined);
  },
};

// Test values() with preventCancel: true
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorPreventCancel = {
  async test() {
    let cancelled = false;

    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
      cancel() {
        cancelled = true;
      },
    });

    const values = [];
    for await (const chunk of rs.values({ preventCancel: true })) {
      values.push(chunk);
      if (values.length === 3) {
        break;
      }
    }

    strictEqual(values.length, 3);
    ok(!cancelled, 'stream should NOT be cancelled with preventCancel: true');
    ok(!rs.locked, 'stream should be unlocked');

    const reader = rs.getReader();
    const { value } = await reader.read();
    strictEqual(value, 'chunk');
    reader.releaseLock();
  },
};

// Test iterating over an already-closed stream
// Inspired by: Deno tests/unit/streams_test.ts
export const asyncIteratorOnClosedStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue('only-chunk');
        controller.close();
      },
    });

    const values = [];
    for await (const chunk of rs) {
      values.push(chunk);
    }

    deepStrictEqual(values, ['only-chunk']);
  },
};

// Test iterating over an already-errored stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorOnErroredStream = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.error(new Error('Stream error'));
      },
    });

    const values = [];
    const iterate = async () => {
      for await (const chunk of rs) {
        values.push(chunk);
      }
    };

    await rejects(iterate, { message: 'Stream error' });
    strictEqual(values.length, 0);
  },
};

// Test that getting an async iterator locks the stream
// Inspired by: Bun test/js/web/streams/streams.test.js
export const asyncIteratorLocksStream = {
  async test() {
    const rs = new ReadableStream({
      pull(controller) {
        controller.enqueue('chunk');
      },
    });

    ok(!rs.locked, 'stream should not be locked initially');

    const iterator = rs[Symbol.asyncIterator]();

    ok(rs.locked, 'stream should be locked after getting iterator');

    throws(() => rs[Symbol.asyncIterator](), TypeError);

    await iterator.return();
  },
};

// return(); next() without awaiting: the return settles first with its
// argument, the next reports done (parity).
export const returnThenNextNoAwait = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue('x');
      },
    });
    const it = rs[Symbol.asyncIterator]();
    const retP = it.return('done-arg');
    const nextP = it.next();
    const ret = await retP;
    strictEqual(ret.done, true);
    strictEqual(ret.value, 'done-arg');
    const nxt = await nextP;
    strictEqual(nxt.done, true);
    strictEqual(nxt.value, undefined);
  },
};

// next(); return() without awaiting: the read wins, the return still
// cancels the stream (parity).
export const nextThenReturnNoAwait = {
  async test() {
    let cancelCalled = false;
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue('x');
      },
      cancel() {
        cancelCalled = true;
      },
    });
    const it = rs[Symbol.asyncIterator]();
    const nextP = it.next();
    const retP = it.return('ret');
    const nxt = await nextP;
    strictEqual(nxt.done, false);
    strictEqual(nxt.value, 'x');
    const ret = await retP;
    strictEqual(ret.done, true);
    strictEqual(ret.value, 'ret');
    strictEqual(cancelCalled, true);
  },
};

// DIVERGENCE (ledger #13; the WPT async-iterator.any properties seed):
// the iterator's prototype exposes next and return on both sides, but C++
// also exposes a constructor property. The class string is WebIDL's
// "ReadableStream AsyncIterator" (non-writable) in TS, and
// "ReadableStreamAsyncIterator" (writable) in C++.
export const iteratorPrototypeShape = {
  async test() {
    const rs = new ReadableStream();
    const it = rs[Symbol.asyncIterator]();
    deepStrictEqual(Object.getOwnPropertyNames(it), []);
    const proto = Object.getPrototypeOf(it);
    deepStrictEqual(
      Object.getOwnPropertyNames(proto).sort(),
      (usingTsImpl
        ? ['next', 'return']
        : ['constructor', 'next', 'return']
      ).sort()
    );
    const tag = usingTsImpl
      ? 'ReadableStream AsyncIterator'
      : 'ReadableStreamAsyncIterator';
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag),
      {
        value: tag,
        writable: !usingTsImpl,
        enumerable: false,
        configurable: true,
      }
    );
    strictEqual(Object.prototype.toString.call(it), `[object ${tag}]`);
    await it.return();
  },
};

// DIVERGENCE (ledger #25): next() and return() called on anything but a
// stream's iterator return a promise rejected with a TypeError (WebIDL);
// C++ throws it synchronously. A different stream's iterator is a valid
// receiver: the call acts on that iterator.
export const iteratorMethodsRejectForeignThis = {
  async test() {
    const it = new ReadableStream().values();
    const other = new ReadableStream({
      start(c) {
        c.close();
      },
    }).values();
    const proto = Object.getPrototypeOf(it);
    for (const method of ['next', 'return']) {
      for (const receiver of [undefined, {}, proto, new ReadableStream()]) {
        if (usingTsImpl) {
          const result = it[method].call(receiver);
          ok(result instanceof Promise, method);
          await rejects(result, TypeError);
        } else {
          throws(() => it[method].call(receiver), TypeError);
        }
      }
      const result = await it[method].call(other);
      strictEqual(result.done, true);
      strictEqual(result.value, undefined);
    }
    await it.return();
  },
};

// Parity: the first next() reads at once (WebIDL runs its steps with no
// ongoing promise), so a started stream with an empty queue pulls
// synchronously, as reader.read() does.
export const firstNextPullsSynchronously = {
  async test() {
    let pulls = 0;
    const rs = new ReadableStream(
      {
        pull(controller) {
          controller.enqueue(++pulls);
        },
      },
      { highWaterMark: 0 }
    );
    await scheduler.wait(0);
    strictEqual(pulls, 0);
    const it = rs.values();
    const next = it.next();
    strictEqual(pulls, 1);
    deepStrictEqual(await next, { value: 1, done: false });
    await it.return();
  },
};

// Ledger #22: n1; a continuation on n1 calls next() (n3); then n2. Per
// spec, n3 reads before n2: n2 gets 'c', n3 gets 'b'. C++ keeps call
// order.
export const nextFromEarlierContinuationReadsAhead = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const it = rs.values();
    const n1 = it.next();
    let n3;
    const continued = n1.then(() => {
      n3 = it.next();
    });
    const n2 = it.next();
    await scheduler.wait(5);
    controller.enqueue('a');
    await continued;
    await scheduler.wait(5);
    controller.enqueue('b');
    controller.enqueue('c');
    strictEqual((await n1).value, 'a');
    deepStrictEqual(
      [(await n2).value, (await n3).value],
      usingTsImpl ? ['c', 'b'] : ['b', 'c']
    );
    await it.return();
  },
};

// Ledger #22: n1; a continuation on n1 calls next() (n2); then return().
// Per spec, n2 runs before the return and reads queued data; the return
// still cancels. C++ reports n2 done.
export const nextFromEarlierContinuationBeatsReturn = {
  async test() {
    let controller;
    let cancelReason;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const it = rs.values();
    const n1 = it.next();
    let n2;
    const continued = n1.then(() => {
      n2 = it.next();
    });
    await scheduler.wait(5);
    const ret = it.return('bye');
    controller.enqueue('a');
    // 'b' must be queued before n2 is issued: n2's read is then served
    // synchronously, so the reader has no pending read requests when the
    // return steps run, as the streams spec's async iterator return
    // asserts. A single enqueue would leave n2's read request pending
    // there, a state the spec treats as unreachable.
    controller.enqueue('b');
    strictEqual((await n1).value, 'a');
    await continued;
    const r2 = await n2;
    strictEqual(r2.done, !usingTsImpl);
    strictEqual(r2.value, usingTsImpl ? 'b' : undefined);
    deepStrictEqual(await ret, { value: 'bye', done: true });
    strictEqual(cancelReason, 'bye');
  },
};

// Ledger #22: two next() calls pending when the stream errors. Per spec,
// the first rejects and the iterator is finished, so later ones report
// done. C++ rejects them all.
export const nextAfterRejectedNextIsDone = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const it = rs.values();
    const n1 = it.next();
    const n2 = it.next();
    await scheduler.wait(5);
    const boom = new Error('boom');
    controller.error(boom);
    const n3 = it.next();
    await rejects(n1, (e) => e === boom);
    for (const p of [n2, n3]) {
      if (usingTsImpl) {
        deepStrictEqual(await p, { value: undefined, done: true });
      } else {
        await rejects(p, (e) => e === boom);
      }
    }
  },
};
