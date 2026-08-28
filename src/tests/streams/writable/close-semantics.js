// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// close() semantics: promise fan-out when the close hook throws, and
// double-close rejection. Migrated from streams-js-test.js. Both main
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
