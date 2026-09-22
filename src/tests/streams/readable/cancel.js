// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream.cancel(): reason plumbing and locked-stream behavior.

import { strictEqual } from 'node:assert';
import { rejectionOf } from 'helpers';

// The cancel reason reaches the source's cancel hook by identity, once
// (parity).
export const cancelReasonIdentity = {
  async test() {
    const reason = { why: 'because' };
    const seen = [];
    const rs = new ReadableStream({
      cancel(r) {
        seen.push(r);
      },
    });
    await rs.cancel(reason);
    strictEqual(seen.length, 1);
    strictEqual(seen[0], reason);
  },
};

// cancel() on a locked stream rejects with TypeError and does NOT run
// the cancel hook (parity).
export const cancelLockedStreamRejects = {
  async test() {
    let cancelCalled = false;
    const rs = new ReadableStream({
      cancel() {
        cancelCalled = true;
      },
    });
    rs.getReader();
    const err = await rejectionOf(rs.cancel('nope'));
    strictEqual(err.name, 'TypeError');
    strictEqual(cancelCalled, false);
  },
};

// A rejecting cancel hook propagates its error (identity) to the
// caller's cancel promise (parity).
export const cancelHookRejectionPropagates = {
  async test() {
    const err = new Error('cancel-hook-fail');
    const rs = new ReadableStream({
      cancel() {
        return Promise.reject(err);
      },
    });
    strictEqual(await rejectionOf(rs.cancel('why')), err);
  },
};

// After cancel, reads are done and the queue is discarded (parity).
export const cancelDiscardsQueue = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue('a');
      },
    });
    const reader = rs.getReader();
    await reader.cancel('discard');
    const r = await reader.read();
    strictEqual(r.done, true);
    strictEqual(r.value, undefined);
  },
};
