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
//   reason.

import { ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { captureRejection, assertRejectsWithReason } from 'propagation-helpers';

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
