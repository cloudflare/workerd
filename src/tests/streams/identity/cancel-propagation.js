// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Canceling the readable side propagates an error to the writable side.
//
// How the cancel reason surfaces deliberately diverges between the
// implementations, and both sides are asserted below (see
// propagation-helpers.js):
// - TypeScript: the original reason instance surfaces everywhere — pending
//   write, pending close, writer.closed, and subsequent writes.
// - C++: the reason reaches the writable side through a kj::Exception
//   boundary, so it surfaces as a re-created Error carrying the same
//   message — unlike abort, where writer.closed sees the original instance.
//   A write after a plain cancel rejects with that re-created reason (the
//   stream is in an errored state), but if a close() was already in flight
//   when the cancel landed, later writes reject with a generic TypeError
//   ("This WritableStream has been closed.") instead.

import { ok, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { captureRejection, assertRejectsWithReason } from 'propagation-helpers';

export const cancelRejectsPendingWriteAndClose = {
  async test() {
    const reason = new Error('cancel reason');
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromise = writer.write(new TextEncoder().encode('test'));
    const closePromise = writer.close();
    const writerClosed = writer.closed;
    await reader.cancel(reason);
    await assertRejectsWithReason(writePromise, reason);
    await assertRejectsWithReason(closePromise, reason);
    await assertRejectsWithReason(writerClosed, reason);
    // A write after the (cancelled) close diverges: C++ reports the stream
    // as closed rather than surfacing the cancel reason.
    const err = await captureRejection(writer.write(new Uint8Array([1])));
    if (usingTsImpl) {
      strictEqual(err, reason);
    } else {
      ok(err instanceof TypeError);
      strictEqual(err.message, 'This WritableStream has been closed.');
    }
  },
};

export const cancelRejectsSubsequentWrites = {
  async test() {
    // With no close() in flight, a cancel leaves the writable errored with
    // the cancel reason, and later writes reject with it in both
    // implementations (re-created in C++, original instance in TypeScript).
    const reason = new Error('cancel reason');
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await readable.cancel(reason);
    await assertRejectsWithReason(writer.write(new Uint8Array([1])), reason);
  },
};

export const pipeCancellationWinsOverLaterReadableCancel = {
  async test() {
    // The C++ implementation cancels the pending sink write when pipeTo() is aborted. That
    // disconnection must become the transform's terminal error before a later readable.cancel()
    // can supply a different reason. The TypeScript implementation has no corresponding KJ
    // operation.
    if (usingTsImpl) return;

    const source = new Response(new Uint8Array([1])).body;
    const transform = new IdentityTransformStream();
    const abortController = new AbortController();
    const pipePromise = source.pipeTo(transform.writable, {
      signal: abortController.signal,
      preventAbort: true,
      preventCancel: true,
    });

    await scheduler.wait(0);
    abortController.abort(new Error('pipe canceled'));
    await captureRejection(pipePromise);

    await transform.readable.cancel(new Error('later readable cancel'));
    const writer = transform.writable.getWriter();
    const error = await captureRejection(writer.write(new Uint8Array([2])));
    strictEqual(error.message, 'Network connection lost.');
  },
};

export const cancelResolvesReaderClosedPromise = {
  async test() {
    // The canceling reader's own closed promise resolves (with undefined),
    // it does not reject.
    const { readable } = new IdentityTransformStream();
    const reader = readable.getReader();
    await reader.cancel();
    strictEqual(await reader.closed, undefined);
  },
};

export const cancelledReaderReadsResolveDone = {
  async test() {
    // After cancel, the canceling reader's own reads resolve done rather
    // than rejecting.
    const { readable } = new IdentityTransformStream();
    const reader = readable.getReader();
    await reader.cancel(new Error('cancel reason'));
    const { done } = await reader.read();
    strictEqual(done, true);
  },
};
