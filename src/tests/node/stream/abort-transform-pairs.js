// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// stream.addAbortSignal() on one half of a transform pair. The hook errors
// the half it is attached to without running the sink's abort or the
// source's cancel, so the pair itself errors its other half with the same
// reason, as TransformStreamDefaultController.error() would
// (TransformStreamError): every write, read and closed promise on both
// halves rejects with the AbortError, finished() on the other half reports
// it, and a pipeThrough() source upstream of the pair is cancelled with it.
// In Node the other half never learns of it and its pending operations stay
// pending. The C++ streams carry no hook and refuse the registration up
// front (ledger #5).

import { addAbortSignal, finished } from 'node:stream';
import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const unsupported = {
  name: 'TypeError',
  code: 'ERR_WEB_STREAM_INTEROP_UNSUPPORTED',
  message:
    'addAbortSignal() is not supported for web streams by the streams implementation in use',
};

const noop = () => {};

// A promise's settlement within a short grace period, so a regression fails
// an assertion instead of hanging the cell.
function outcome(promise, ms = 50) {
  return Promise.race([
    promise.then(
      (value) => ({ status: 'fulfilled', value }),
      (reason) => ({ status: 'rejected', reason })
    ),
    scheduler.wait(ms).then(() => ({ status: 'pending' })),
  ]);
}

function finishedOf(stream) {
  return new Promise((resolve) => finished(stream, resolve));
}

function assertAbortError(err, cause) {
  strictEqual(err.name, 'AbortError');
  strictEqual(err.code, 'ABORT_ERR');
  strictEqual(err.cause, cause);
}

function assertRejectsWithAbortError(settled, cause) {
  strictEqual(settled.status, 'rejected');
  assertAbortError(settled.reason, cause);
}

let transformerCancelCalls = 0;

// Every transform pair the runtime provides, with a chunk its writable
// accepts. The identity, encoding and default transforms park a write until
// a read arrives; the codec pairs settle it at once (`parks`).
const gzipHeader = new Uint8Array([
  0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
]);
const pairs = {
  IdentityTransformStream: {
    make: () => new IdentityTransformStream(),
    chunk: new Uint8Array([1, 2, 3]),
    parks: true,
  },
  FixedLengthStream: {
    make: () => new FixedLengthStream(3),
    chunk: new Uint8Array([1, 2, 3]),
    parks: true,
  },
  CompressionStream: {
    make: () => new CompressionStream('gzip'),
    chunk: new Uint8Array([1, 2, 3]),
    parks: false,
  },
  DecompressionStream: {
    make: () => new DecompressionStream('gzip'),
    chunk: gzipHeader,
    parks: false,
  },
  TextEncoderStream: {
    make: () => new TextEncoderStream(),
    chunk: 'abc',
    parks: true,
  },
  TextDecoderStream: {
    make: () => new TextDecoderStream(),
    chunk: new Uint8Array([0x61, 0x62, 0x63]),
    parks: true,
  },
  TransformStream: {
    make: () =>
      new TransformStream({
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
        cancel() {
          transformerCancelCalls++;
        },
      }),
    chunk: 'a',
    parks: true,
  },
  'TransformStream (no transformer)': {
    make: () => new TransformStream(),
    chunk: 'a',
    parks: true,
  },
};

// The hook on the readable half: the writable half errors too. A parked
// write, writer.closed, later writes, and finished() on the writable all
// report the AbortError; the transformer's cancel is not called.
export const addAbortSignalOnPairReadableErrorsWritable = {
  async test() {
    transformerCancelCalls = 0;
    for (const [name, { make, chunk, parks }] of Object.entries(pairs)) {
      const pair = make();
      const ac = new AbortController();
      if (!usingTsImpl) {
        throws(() => addAbortSignal(ac.signal, pair.readable), unsupported);
        continue;
      }
      addAbortSignal(ac.signal, pair.readable);
      const finishedWritable = finishedOf(pair.writable);
      const writer = pair.writable.getWriter();
      const write = writer.write(chunk);
      write.catch(noop);
      writer.closed.catch(noop);
      strictEqual(
        (await outcome(write, 5)).status,
        parks ? 'pending' : 'fulfilled',
        name
      );
      const reason = new Error(`abandon ${name}`);
      ac.abort(reason);
      if (parks) assertRejectsWithAbortError(await outcome(write), reason);
      assertRejectsWithAbortError(await outcome(writer.closed), reason);
      assertRejectsWithAbortError(await outcome(writer.write(chunk)), reason);
      const settled = await outcome(finishedWritable);
      strictEqual(settled.status, 'fulfilled', name);
      assertAbortError(settled.value, reason);
      assertRejectsWithAbortError(
        await outcome(pair.readable.getReader().read()),
        reason
      );
    }
    strictEqual(transformerCancelCalls, 0);
  },
};

// The hook on the writable half: the readable half errors too. A pending
// read, reader.closed, writer.closed and finished() on the readable all
// report the AbortError; the transformer's cancel is not called.
export const addAbortSignalOnPairWritableErrorsReadable = {
  async test() {
    transformerCancelCalls = 0;
    for (const [name, { make }] of Object.entries(pairs)) {
      const pair = make();
      const ac = new AbortController();
      if (!usingTsImpl) {
        throws(() => addAbortSignal(ac.signal, pair.writable), unsupported);
        continue;
      }
      addAbortSignal(ac.signal, pair.writable);
      const finishedReadable = finishedOf(pair.readable);
      const writer = pair.writable.getWriter();
      const reader = pair.readable.getReader();
      const read = reader.read();
      read.catch(noop);
      reader.closed.catch(noop);
      writer.closed.catch(noop);
      strictEqual((await outcome(read, 5)).status, 'pending', name);
      const reason = new Error(`abandon ${name}`);
      ac.abort(reason);
      assertRejectsWithAbortError(await outcome(read), reason);
      assertRejectsWithAbortError(await outcome(reader.closed), reason);
      assertRejectsWithAbortError(await outcome(writer.closed), reason);
      const settled = await outcome(finishedReadable);
      strictEqual(settled.status, 'fulfilled', name);
      assertAbortError(settled.value, reason);
    }
    strictEqual(transformerCancelCalls, 0);
  },
};

// The hook on the readable half of a pipeThrough() stage tears the pipe
// down: the upstream source is cancelled with the AbortError and unlocked,
// and reads from the pair's readable reject with it.
export const addAbortSignalOnPipedPairReadableCancelsSource = {
  async test() {
    for (const [name, { make, chunk }] of Object.entries(pairs)) {
      const pair = make();
      const ac = new AbortController();
      if (!usingTsImpl) {
        throws(() => addAbortSignal(ac.signal, pair.readable), unsupported);
        continue;
      }
      addAbortSignal(ac.signal, pair.readable);
      let cancelReason;
      const source = new ReadableStream({
        start(controller) {
          controller.enqueue(chunk);
        },
        pull() {
          return new Promise(noop);
        },
        cancel(reason) {
          cancelReason = reason;
        },
      });
      const reader = source.pipeThrough(pair).getReader();
      const first = reader.read();
      first.catch(noop);
      await scheduler.wait(5);
      const reason = new Error(`abandon ${name}`);
      ac.abort(reason);
      assertRejectsWithAbortError(await outcome(reader.read()), reason);
      // The pipe cancels the source from its own reactions.
      await scheduler.wait(5);
      assertAbortError(cancelReason, reason);
      strictEqual(source.locked, false, name);
    }
  },
};

// The hook on a half whose pair the other half has already torn down is
// inert: the half is no longer readable or writable, and its stored error
// stays the first one.
export const addAbortSignalOnPairHalfAlreadyErroredIsInert = {
  async test() {
    const ac = new AbortController();
    if (!usingTsImpl) {
      throws(
        () => addAbortSignal(ac.signal, new TransformStream().readable),
        unsupported
      );
      return;
    }
    const boom = new Error('boom');
    {
      const pair = new TransformStream();
      await pair.writable.getWriter().abort(boom);
      addAbortSignal(ac.signal, pair.readable);
      ac.abort(new Error('late'));
      const read = await outcome(pair.readable.getReader().read());
      strictEqual(read.status, 'rejected');
      strictEqual(read.reason, boom);
    }
    {
      const pair = new TransformStream();
      await pair.readable.cancel(boom);
      const ac2 = new AbortController();
      addAbortSignal(ac2.signal, pair.writable);
      ac2.abort(new Error('late'));
      const closed = await outcome(pair.writable.getWriter().closed);
      strictEqual(closed.status, 'rejected');
      strictEqual(closed.reason, boom);
    }
  },
};
