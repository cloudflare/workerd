// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What body consumption (text/arrayBuffer/bytes/json) does when it fails
// on the consumer's side rather than the source's: the stream is cancelled
// with the failure, so the source's cancel() runs and pulls stop, and the
// stream stays locked (PARITY). DIVERGENCE (ledger #21): a TransformStream
// readable declaring `expectedLength` and delivering more is such a
// failure in TypeScript; C++ consumption ignores the declaration.

import { strictEqual, deepStrictEqual, ok, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const kNotBytes = 'This ReadableStream did not return bytes.';

// A source whose second pull enqueues a non-byte chunk, recording pulls and
// the cancel reason.
function nonByteSource(log) {
  let controller;
  const rs = new ReadableStream({
    start(c) {
      controller = c;
      c.enqueue(enc.encode('ok'));
    },
    pull(c) {
      log.pulls++;
      c.enqueue(log.pulls === 1 ? 'not bytes' : enc.encode('late'));
    },
    cancel(reason) {
      log.cancel = reason;
    },
  });
  return { rs, controller };
}

// A non-byte chunk rejects the consumer and cancels the source with the
// same TypeError; nothing is pulled afterwards (the count at rejection
// differs: TypeScript pulls on reading the last queued chunk, ledger #5),
// the stream stays locked (the consumer keeps its reader), and the
// controller is spent.
export const nonBytesChunkCancelsSource = {
  async test() {
    const log = { pulls: 0, cancel: undefined };
    const { rs, controller } = nonByteSource(log);
    await rejects(new Response(rs).text(), {
      name: 'TypeError',
      message: kNotBytes,
    });
    const pullsAtRejection = log.pulls;
    await scheduler.wait(10);
    ok(log.cancel instanceof TypeError);
    strictEqual(log.cancel.message, kNotBytes);
    strictEqual(log.pulls, pullsAtRejection);
    strictEqual(rs.locked, true);
    throws(() => controller.enqueue(enc.encode('x')), TypeError);
  },
};

// Every consumer takes the same path.
export const everyConsumerCancelsOnNonBytes = {
  async test() {
    const consumers = ['arrayBuffer', 'bytes', 'json', 'text'];
    const reasons = [];
    for (const method of consumers) {
      const rs = new ReadableStream({
        start(c) {
          c.enqueue(123);
        },
        cancel(reason) {
          reasons.push(reason.message);
        },
      });
      await rejects(new Response(rs)[method](), {
        name: 'TypeError',
        message: kNotBytes,
      });
    }
    deepStrictEqual(
      reasons,
      consumers.map(() => kNotBytes)
    );
  },
};

// A cancel() that rejects replaces the consumption failure.
export const nonBytesCancelRejectionReplacesFailure = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('not bytes');
      },
      cancel() {
        throw new Error('cancel failed');
      },
    });
    await rejects(new Response(rs).text(), {
      name: 'Error',
      message: 'cancel failed',
    });
  },
};

// A non-byte chunk in the batch that closes the stream: nothing is left to
// cancel, and the consumer rejects all the same.
export const nonBytesChunkInClosingBatch = {
  async test() {
    let cancelled = false;
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('a'));
        c.enqueue('bad');
        c.close();
      },
      cancel() {
        cancelled = true;
      },
    });
    await rejects(new Response(rs).text(), {
      name: 'TypeError',
      message: kNotBytes,
    });
    strictEqual(cancelled, false);
  },
};

// Ledger #21. A TransformStream readable that delivers more than its
// declared expectedLength: TypeScript rejects the consumer with a
// RangeError naming the declaration and cancels the readable, which
// errors the writable side with the same reason (the write itself has
// already been transformed and fulfilled; the queued close rejects); C++
// returns everything.
export const transformExpectedLengthOverflow = {
  async test() {
    let cancelReason;
    const ts = new TransformStream({
      expectedLength: 3,
      transform(chunk, c) {
        c.enqueue(chunk);
      },
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const writer = ts.writable.getWriter();
    const consumed = new Response(ts.readable).text();
    const written = writer.write(enc.encode('hello'));
    const closed = writer.close();
    if (usingTsImpl) {
      const expected = {
        name: 'RangeError',
        message: 'stream delivered more bytes than its declared expectedLength',
      };
      await rejects(consumed, expected);
      await written;
      await rejects(closed, expected);
      await rejects(writer.closed, expected);
      ok(cancelReason instanceof RangeError);
      strictEqual(cancelReason.message, expected.message);
    } else {
      strictEqual(await consumed, 'hello');
      await written;
      await closed;
      strictEqual(cancelReason, undefined);
    }
  },
};

// Exactly the declared length is fine everywhere.
export const transformExpectedLengthMet = {
  async test() {
    const ts = new TransformStream({
      expectedLength: 5,
      transform(chunk, c) {
        c.enqueue(chunk);
      },
    });
    const writer = ts.writable.getWriter();
    const consumed = new Response(ts.readable).text();
    await writer.write(enc.encode('hello'));
    await writer.close();
    strictEqual(await consumed, 'hello');
  },
};
