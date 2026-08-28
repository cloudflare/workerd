// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// locked/disturbed state coupling between a JS ReadableStream and the
// Body machinery that adopts it.

import { strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// A DISTURBED stream is rejected by the Response constructor with the
// same TypeError on both sides.
export const disturbedStreamIntoResponse = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new TextEncoder().encode('x'));
        c.close();
      },
    });
    const reader = rs.getReader();
    await reader.read();
    reader.releaseLock();
    throws(() => new Response(rs), {
      name: 'TypeError',
      message: /disturbed/,
    });
  },
};

// A merely LOCKED (not disturbed) stream is accepted by the Response
// constructor on both sides; consumption then fails only when the body
// is actually used. Pinned as observed: construction succeeds.
export const lockedStreamIntoResponse = {
  test() {
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    rs.getReader();
    new Response(rs);
  },
};

// Response(body).body returns the SAME stream object; consuming the
// response flips bodyUsed. DIVERGENCE in the post-consumption lock: C++
// releases the adopted stream's lock after text() completes, TypeScript
// keeps it locked.
export const bodyIdentityAndLockCoupling = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    const resp = new Response(rs);
    strictEqual(resp.body, rs);
    strictEqual(rs.locked, false);
    strictEqual(resp.bodyUsed, false);
    strictEqual(await resp.text(), '');
    strictEqual(resp.bodyUsed, true);
    strictEqual(rs.locked, usingTsImpl ? true : false);
  },
};
