// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pipes with native endpoints beyond the plain identity cases in
// pipe-matrix.js: cancel propagation through pipeThrough,
// FixedLengthStream, and pre-settled endpoint pairings. Identity↔
// identity piping (including the circular pipeThrough pin) lives in
// the identity suite's pipe-integration.js.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const outcomeOf = (p, ms = 250) =>
  Promise.race([
    p.then(
      (v) => ({ state: 'fulfilled', value: v }),
      (e) => ({ state: 'rejected', reason: e })
    ),
    scheduler.wait(ms).then(() => ({ state: 'pending' })),
  ]);

// Canceling the reader of a pipeThrough result propagates backward
// through the (native) IdentityTransformStream: all locks release and
// the JS source ends CLOSED, not errored (migrated from
// api/streams/streams-test.js testCancelPipethrough).
export const cancelPropagationThroughIdentity = {
  async test() {
    const enc = new TextEncoder();
    const transform = new IdentityTransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const readable = rs.pipeThrough(transform);
    const reader = readable.getReader();
    ok(rs.locked);
    ok(transform.writable.locked);
    reader.cancel(new Error('boom'));
    reader.releaseLock();
    await scheduler.wait(1);
    ok(!rs.locked);
    ok(!transform.readable.locked);
    ok(!transform.writable.locked);
    // Cancel propagates back and CLOSES the source stream.
    const result = await rs.getReader().read();
    ok(result.done);
    strictEqual(result.value, undefined);
  },
};

// The same through a JS-backed TransformStream (migrated from
// testCancelPipethrough2).
export const cancelPropagationThroughJsTransform = {
  async test() {
    const enc = new TextEncoder();
    const transform = new TransformStream();
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
      },
    });
    const readable = rs.pipeThrough(transform);
    const reader = readable.getReader();
    ok(rs.locked);
    ok(transform.writable.locked);
    reader.cancel(new Error('boom'));
    reader.releaseLock();
    await scheduler.wait(1);
    ok(!rs.locked);
    ok(!transform.readable.locked);
    ok(!transform.writable.locked);
    const result = await rs.getReader().read();
    ok(result.done);
    strictEqual(result.value, undefined);
  },
};

// Piping the exact byte count into a FixedLengthStream delivers all
// bytes and completes both ends.
export const fixedLengthStreamPipeExact = {
  async test() {
    const enc = new TextEncoder();
    const fls = new FixedLengthStream(5);
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hel'));
        c.enqueue(enc.encode('lo'));
        c.close();
      },
    });
    const pipeP = rs.pipeTo(fls.writable);
    const text = await new Response(fls.readable).text();
    strictEqual(text, 'hello');
    await pipeP;
  },
};

// Overflowing a FixedLengthStream: DIVERGENCE — TypeScript rejects the
// pipe with a RangeError; the C++ pipe never settles (bounded
// observation).
export const fixedLengthStreamPipeOverflow = {
  async test() {
    const enc = new TextEncoder();
    const fls = new FixedLengthStream(3);
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hello'));
        c.close();
      },
    });
    const outcome = await outcomeOf(rs.pipeTo(fls.writable));
    if (usingTsImpl) {
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason.name, 'RangeError');
    } else {
      strictEqual(outcome.state, 'pending');
    }
  },
};

// Closing short of the declared length: the pipe never settles on
// EITHER implementation (bounded observation; parity of
// nonconformance — the close step should surface the length error).
export const fixedLengthStreamPipeUnderflow = {
  async test() {
    const enc = new TextEncoder();
    const fls = new FixedLengthStream(5);
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('hi'));
        c.close();
      },
    });
    const outcome = await outcomeOf(rs.pipeTo(fls.writable));
    strictEqual(outcome.state, 'pending');
  },
};

// An already-closed source piped into an already-closed destination
// (the WPT multiple-propagation seed). DIVERGENCE: C++ rejects
// TypeError (per spec, the destination's closed state must reject the
// pipe); TypeScript treats it as a trivially complete pipe and
// FULFILLS.
export const closedSourceToClosedDest = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const ws = new WritableStream({});
    await ws.close();
    const outcome = await outcomeOf(rs.pipeTo(ws));
    if (usingTsImpl) {
      strictEqual(outcome.state, 'fulfilled');
    } else {
      strictEqual(outcome.state, 'rejected');
      strictEqual(outcome.reason.name, 'TypeError');
    }
  },
};

// A closed source into a live destination: the pipe fulfills and closes
// the destination (forward close propagation baseline).
export const closedSourceToLiveDest = {
  async test() {
    let closed = false;
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const ws = new WritableStream({
      close() {
        closed = true;
      },
    });
    await rs.pipeTo(ws);
    strictEqual(closed, true);
  },
};
