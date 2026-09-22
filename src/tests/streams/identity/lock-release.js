// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// releaseLock() on identity stream readers and writers: the released
// handle's closed promise rejects with TypeError, and the stream returns to
// a lockable state. The rejection message decoration differs slightly
// between the implementations; the class and the "has been released"
// phrasing are shared.

import { ok, strictEqual, rejects } from 'node:assert';

export const releaseLockRejectsClosedPromises = {
  async test() {
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const reader = its.readable.getReader();
    const readerClosed = reader.closed;
    const writerClosed = writer.closed;
    reader.releaseLock();
    writer.releaseLock();
    await rejects(readerClosed, (err) => {
      ok(err instanceof TypeError);
      return /has been released/.test(err.message);
    });
    await rejects(writerClosed, (err) => {
      ok(err instanceof TypeError);
      return /has been released/.test(err.message);
    });
    // The stream is unlocked and reusable.
    strictEqual(its.readable.locked, false);
    strictEqual(its.writable.locked, false);
    ok(its.readable.getReader());
    ok(its.writable.getWriter());
  },
};
