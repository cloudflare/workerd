// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Aborting the writable side propagates an error to the readable side.
//
// How the abort reason surfaces deliberately diverges between the
// implementations, and both sides are asserted below (see
// propagation-helpers.js):
// - TypeScript: the original reason instance surfaces everywhere — pending
//   and subsequent reads, both closed promises, and subsequent writes.
// - C++: reads and reader.closed reject with a re-created Error carrying the
//   same message (the reason crosses a kj::Exception boundary), while
//   writer.closed rejects with the original instance (the writable side
//   holds the JS value directly). Subsequent writes reject with a generic
//   TypeError ("This WritableStream has been closed."), not the abort
//   reason. When the abort cancels a write parked in the sink, reads reject
//   with that cancellation's disconnection error ("Network connection
//   lost."), which takes hold before the abort reason arrives.

import { ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { captureRejection, assertRejectsWithReason } from 'propagation-helpers';

// Yields through the event loop, so that every settlement already in flight
// has landed before the caller asserts on it.
const tick = () => scheduler.wait(0);

// Writes `bytes` and yields until the write has reached the sink, where it
// parks until a read consumes it. Resolves with `{ writePromise }`: resolving
// with the promise itself would adopt it, waiting for that read.
async function parkWrite(writer, bytes) {
  const writePromise = writer.write(bytes);
  writePromise.catch(() => {});
  await tick();
  return { writePromise };
}

// Aborts through `target` (a writer or an unlocked stream) and requires the
// abort to fulfill without any read being issued.
async function abortWithoutRead(target, reason) {
  let settled = false;
  const abortPromise = target.abort(reason);
  abortPromise.then(
    () => (settled = true),
    () => (settled = true)
  );
  await tick();
  ok(settled, 'abort() must not wait for a read');
  await abortPromise;
}

export const abortRejectsPendingRead = {
  async test() {
    const reason = new Error('boom');
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    const readerClosed = reader.closed;
    const writerClosed = writer.closed;
    await writer.abort(reason);
    await assertRejectsWithReason(readPromise, reason);
    await assertRejectsWithReason(readerClosed, reason);
    // Both implementations reject writer.closed with the original instance:
    // the writable side holds the JS value without crossing kj.
    strictEqual(await captureRejection(writerClosed), reason);
  },
};

export const abortRejectsSubsequentReads = {
  async test() {
    const reason = new Error('boom');
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.abort(reason);
    const reader = readable.getReader();
    await assertRejectsWithReason(reader.read(), reason);
  },
};

export const abortClearsPendingWrite = {
  async test() {
    // Modern semantics (internal_writable_stream_abort_clears_queue pinned
    // in the C++ cell; TypeScript hard-codes it): abort() with an
    // unconsumed write clears it proactively — no reader ever required —
    // and the pending write rejects with the abort reason itself:
    // undefined when none is given, the original instance when one is
    // (the abort-side identity exception applies in both implementations
    // here). The legacy counterpart is legacyAbortWaitsForPendingWrite.
    {
      const { writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const writePromise = writer.write(new Uint8Array(10));
      await writer.abort();
      strictEqual(await captureRejection(writePromise), undefined);
    }
    {
      const { writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reason = new Error('modern abort');
      const writePromise = writer.write(new Uint8Array(10));
      await writer.abort(reason);
      strictEqual(await captureRejection(writePromise), reason);
    }
  },
};

export const abortClearsParkedWrite = {
  async test() {
    // A write that has reached the sink parks until a read consumes it.
    // abort() clears it without waiting for that read: the abort fulfills,
    // and the write and writer.closed reject with the original reason.
    for (const create of [
      () => new IdentityTransformStream(),
      () => new FixedLengthStream(4),
    ]) {
      const reason = new Error('boom');
      const { writable } = create();
      const writer = writable.getWriter();
      const writerClosed = writer.closed;
      const { writePromise } = await parkWrite(writer, new Uint8Array([1, 2]));
      await abortWithoutRead(writer, reason);
      strictEqual(await captureRejection(writePromise), reason);
      strictEqual(await captureRejection(writerClosed), reason);
    }
    {
      // The same for a write parked after an earlier write was consumed.
      const reason = new Error('boom');
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader();
      const readPromise = reader.read();
      await writer.write(new Uint8Array([1]));
      strictEqual((await readPromise).value[0], 1);
      const { writePromise } = await parkWrite(writer, new Uint8Array([2]));
      await abortWithoutRead(writer, reason);
      strictEqual(await captureRejection(writePromise), reason);
    }
    {
      // The same when the writer has been released and the stream itself
      // is aborted.
      const reason = new Error('boom');
      const { writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const { writePromise } = await parkWrite(writer, new Uint8Array([1, 2]));
      writer.releaseLock();
      await abortWithoutRead(writable, reason);
      strictEqual(await captureRejection(writePromise), reason);
    }
  },
};

export const abortClearsParkedWriteAndQueue = {
  async test() {
    // Writes and a close queued behind a parked write reject with the
    // original reason too.
    const reason = new Error('boom');
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const { writePromise: parked } = await parkWrite(
      writer,
      new Uint8Array([1])
    );
    const queued = [
      writer.write(new Uint8Array([2])),
      writer.write(new Uint8Array([3])),
      writer.close(),
    ];
    for (const promise of queued) promise.catch(() => {});
    await abortWithoutRead(writer, reason);
    for (const promise of [parked, ...queued]) {
      strictEqual(await captureRejection(promise), reason);
    }
  },
};

export const abortParkedWriteErrorsReadable = {
  async test() {
    const reason = new Error('boom');
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readerClosed = reader.closed;
    readerClosed.catch(() => {});
    await parkWrite(writer, new Uint8Array([1, 2]));
    await abortWithoutRead(writer, reason);
    const readError = await captureRejection(reader.read());
    const closedError = await captureRejection(readerClosed);
    if (usingTsImpl) {
      // The original abort reason, as for any abort.
      strictEqual(readError, reason);
      strictEqual(closedError, reason);
    } else {
      // Cancelling the parked sink write puts the transform into its
      // disconnection error before the abort reason arrives.
      for (const err of [readError, closedError]) {
        ok(err instanceof Error);
        strictEqual(err.message, 'Network connection lost.');
      }
    }
  },
};

export const abortRejectsSubsequentWrites = {
  async test() {
    const reason = new Error('boom');
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.abort(reason);
    const err = await captureRejection(writer.write(new Uint8Array([1])));
    if (usingTsImpl) {
      // The original abort reason.
      strictEqual(err, reason);
    } else {
      // A generic closed-stream TypeError, not the abort reason.
      ok(err instanceof TypeError);
      strictEqual(err.message, 'This WritableStream has been closed.');
    }
  },
};
