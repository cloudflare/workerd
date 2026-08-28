// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDefaultReader: read ordering, releaseLock, and the
// closed-promise lifecycle.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { rejectionOf, drainToArray } from 'helpers';

// Reads resolve in order against queued chunks (parity).
export const readsResolveInOrder = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(1);
        c.enqueue(2);
        c.close();
      },
    });
    const reader = rs.getReader();
    const [r1, r2, r3] = await Promise.all([
      reader.read(),
      reader.read(),
      reader.read(),
    ]);
    strictEqual(r1.value, 1);
    strictEqual(r2.value, 2);
    strictEqual(r3.done, true);
  },
};

// releaseLock() with a read pending rejects the pending read with
// TypeError and unlocks the stream (migrated from streams-test.js
// cancelReadsOnReleaseLock).
export const releaseLockRejectsPendingReads = {
  async test() {
    const rs = new ReadableStream();
    const reader = rs.getReader();
    const read = reader.read();
    reader.releaseLock();
    const err = await rejectionOf(read);
    strictEqual(err.name, 'TypeError');
    strictEqual(rs.locked, false);
  },
};

// DIVERGENCE (the WPT default-reader.any 'closed is replaced' seeds and
// the templated.any disabled block): after the stream closes and the
// reader releases its lock, the spec (TypeScript) REPLACES reader.closed
// with a fresh promise rejected with a release TypeError; C++ keeps the
// original settled closed promise.
export const closedReplacedOnReleaseAfterClose = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    const closed1 = reader.closed;
    controller.close();
    strictEqual(await closed1, undefined);
    reader.releaseLock();
    const closed2 = reader.closed;
    if (usingTsImpl) {
      ok(closed1 !== closed2);
      const err = await rejectionOf(closed2);
      strictEqual(err.name, 'TypeError');
      strictEqual(err.message, 'This reader has been released');
    } else {
      strictEqual(closed1, closed2);
      strictEqual(await closed2, undefined);
    }
  },
};

// Same divergence for the errored stream: TypeScript replaces closed
// with a release rejection, C++ keeps the original error rejection.
export const closedReplacedOnReleaseAfterError = {
  async test() {
    const err = new Error('stream-error');
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    const closed1 = reader.closed;
    controller.error(err);
    strictEqual(await rejectionOf(closed1), err);
    reader.releaseLock();
    const closed2 = reader.closed;
    if (usingTsImpl) {
      ok(closed1 !== closed2);
      const rel = await rejectionOf(closed2);
      strictEqual(rel.name, 'TypeError');
      strictEqual(rel.message, 'This reader has been released');
    } else {
      strictEqual(closed1, closed2);
      strictEqual(await rejectionOf(closed2), err);
    }
  },
};

// An error of literal undefined is preserved: closed rejects with
// undefined (parity; the WPT default-reader.any seed).
export const closedRejectsWithUndefinedError = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const reader = rs.getReader();
    controller.error(undefined);
    let rejected = false;
    let reason = 'sentinel';
    await reader.closed.catch((e) => {
      rejected = true;
      reason = e;
    });
    strictEqual(rejected, true);
    strictEqual(reason, undefined);
  },
};

// reader.cancel() resolves pending reads done and fulfills closed
// (migrated from streams-test.js cancelReaderResolvesClosedPromise and
// streams-js-test.js readableStreamCancelReads, value half).
export const readerCancelResolvesReadsAndClosed = {
  async test() {
    const rs = new ReadableStream();
    const reader = rs.getReader();
    const read = reader.read();
    let closedResolved = false;
    reader.closed.then(() => (closedResolved = true));
    await reader.cancel('bye');
    const r = await read;
    strictEqual(r.done, true);
    strictEqual(r.value, undefined);
    await scheduler.wait(1);
    strictEqual(closedResolved, true);
  },
};

// Chunks queued before the reader detaches survive releaseLock() and a
// new reader picks them up (parity).
export const queuedChunksSurviveReaderSwap = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('a');
        c.enqueue('b');
        c.close();
      },
    });
    const r1 = rs.getReader();
    strictEqual((await r1.read()).value, 'a');
    r1.releaseLock();
    deepStrictEqualArray(await drainToArray(rs), ['b']);
  },
};

function deepStrictEqualArray(actual, expected) {
  strictEqual(actual.length, expected.length);
  for (let i = 0; i < expected.length; i++) {
    strictEqual(actual[i], expected[i]);
  }
}
