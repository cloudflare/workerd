// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// close() semantics: promise fan-out when the close hook throws,
// double-close rejection, and a close() while a queued close waits on the
// sink's abort(). Mostly migrated from streams-js-test.js. Both main
// cells run with async APIs returning rejections (capture_async_api_throws
// pinned for C++; hard-coded in TypeScript) — the sync-throw side lives in
// the legacy-throws cell.

import { strictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A close() racing an immediate abort(). Under C++ the close hook runs
// synchronously from writer.close(), so its throw is in flight when the
// abort arrives: close and abort both reject with the thrown error, and
// closed rejects with the abort reason (undefined). Under TypeScript the
// close is still queued at abort time ([[started]] gating), the erroring
// pass rejects it with the abort reason, the close hook NEVER runs, and
// the abort fulfills.
export const writableStreamCloseThrowRejectsPromises = {
  async test() {
    let closeCalled = false;
    const ws = new WritableStream({
      async close() {
        closeCalled = true;
        throw new Error('boom');
      },
    });

    const writer = ws.getWriter();
    const close = writer.close();
    const abort = writer.abort();
    const closed = writer.closed;

    const res = await Promise.allSettled([close, abort, closed]);

    strictEqual(res[0].status, 'rejected');
    strictEqual(res[2].status, 'rejected');
    strictEqual(res[2].reason, undefined);

    if (usingTsImpl) {
      strictEqual(closeCalled, false);
      strictEqual(res[0].reason, undefined);
      strictEqual(res[1].status, 'fulfilled');
    } else {
      strictEqual(closeCalled, true);
      strictEqual(res[0].reason.message, 'boom');
      strictEqual(res[1].status, 'rejected');
      strictEqual(res[1].reason.message, 'boom');
    }
  },
};

// A second close() while one is pending rejects with TypeError.
export const writerDoubleClose = {
  async test() {
    const ws = new WritableStream({
      write() {},
    });
    const writer = ws.getWriter();

    writer.write(123);

    writer.close();
    await rejects(writer.close(), TypeError);
  },
};

// close() while a close queued before an abort waits on the sink's
// abort(): the stream is already errored, but that close is rejected only
// once abort() settles, so it is still queued. TypeScript checks the
// queued close first and rejects with 'Stream is already closing' (spec:
// both close() methods check it before WritableStreamClose checks the
// state); C++ rejects with the abort reason. Through the writer and,
// once it is released, the stream alike (ledger #19). Once abort()
// settles, the queued close rejects with the abort reason and the abort
// fulfills (parity).
export const closeWhileQueuedCloseAwaitsAbort = {
  async test() {
    for (const via of ['writer', 'stream']) {
      const abortCalled = Promise.withResolvers();
      const abortFinished = Promise.withResolvers();
      let resolveStart;
      const ws = new WritableStream({
        start() {
          return new Promise((resolve) => {
            resolveStart = resolve;
          });
        },
        abort() {
          abortCalled.resolve();
          return abortFinished.promise;
        },
      });
      const writer = ws.getWriter();
      const reason = new Error('reason');
      const queuedClose = writer.close();
      const abort = writer.abort(reason);
      resolveStart();
      await abortCalled.promise;

      let close;
      if (via === 'writer') {
        close = writer.close();
      } else {
        writer.releaseLock();
        close = ws.close();
      }
      if (usingTsImpl) {
        await rejects(close, {
          name: 'TypeError',
          message: 'Stream is already closing',
        });
      } else {
        await rejects(close, (e) => e === reason);
      }

      abortFinished.resolve();
      await rejects(queuedClose, (e) => e === reason);
      strictEqual(await abort, undefined);
    }
  },
};
