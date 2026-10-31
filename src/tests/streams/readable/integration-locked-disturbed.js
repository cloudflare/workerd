// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// locked/disturbed state coupling between a JS ReadableStream and the
// Body machinery that adopts it.

import { notStrictEqual, strictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const disturbed = { name: 'TypeError', message: /disturbed/ };

function closedWithChunk() {
  return new ReadableStream({
    start(c) {
      c.enqueue(new TextEncoder().encode('abc'));
      c.close();
    },
  });
}

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

// A LOCKED (not disturbed) stream is rejected by the Response
// constructor with a TypeError on both sides (spec: extract a body).
export const lockedStreamIntoResponse = {
  test() {
    const rs = new ReadableStream({
      start(c) {
        c.close();
      },
    });
    rs.getReader();
    throws(() => new Response(rs), {
      name: 'TypeError',
      message: /locked to a reader/,
    });
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

// The stream tee() leaves behind, whether teed directly or as a branch
// teed again, is disturbed, so the Response constructor rejects it as it
// does any disturbed stream (parity, ledger #33).
export const teedStreamIntoResponse = {
  test() {
    const teed = closedWithChunk();
    teed.tee();
    const [branch] = closedWithChunk().tee();
    branch.tee();
    for (const stream of [teed, branch]) {
      strictEqual(stream.locked, true);
      throws(() => new Response(stream), disturbed);
    }
  },
};

// bodyUsed once a Response's body has been teed, for a native body and a
// JS-backed one: true from the tee() on (parity, ledger #33). clone()
// gives the response a branch as its body, so its bodyUsed stays false,
// but the body it had is the stream tee() left behind.
export const bodyUsedAfterBodyTee = {
  async test() {
    const responses = () => [
      new Response('abc'),
      new Response(closedWithChunk()),
    ];
    for (const response of responses()) {
      const [a, b] = response.body.tee();
      strictEqual(response.bodyUsed, true);
      strictEqual(await new Response(a).text(), 'abc');
      strictEqual(await new Response(b).text(), 'abc');
      strictEqual(response.bodyUsed, true);
    }
    for (const response of responses()) {
      const body = response.body;
      const clone = response.clone();
      strictEqual(response.bodyUsed, false);
      notStrictEqual(response.body, body);
      throws(() => new Response(body), disturbed);
      strictEqual(await response.text(), 'abc');
      strictEqual(await clone.text(), 'abc');
    }
  },
};
