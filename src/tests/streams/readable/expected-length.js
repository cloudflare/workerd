// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Content-Length carried by a fork of a default source that declares
// expectedLength: a tee branch, a branch teed again, and the copy
// new Request() makes of a request's body. Observed through the SELF echo
// service. The declaration is the stream's total, so a fork reports it
// only while nothing has been read before the fork.
//
// DIVERGENCE (ledger #35): forks made before any read carry the declared
// length in TypeScript; C++ gives tee branches no length, so they go out
// chunked. Once anything has been read before a fork, the fork and its
// own forks go out chunked in both (PARITY). A partly read stream itself
// (ledger #36) is pinned in js-rpc-streams-crossflag-test.js, as no
// consumer here accepts one.

import { deepStrictEqual, strictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();

// A source that declares the 10 bytes it delivers, in two chunks.
function tenByteSource() {
  return new ReadableStream({
    expectedLength: 10,
    start(c) {
      c.enqueue(enc.encode('01234'));
      c.enqueue(enc.encode('56789'));
      c.close();
    },
  });
}

async function readChunk(stream) {
  const reader = stream.getReader();
  const { value } = await reader.read();
  reader.releaseLock();
  return new TextDecoder().decode(value);
}

// Posts the request through SELF, and returns the Content-Length it carried
// and the body echoed back.
async function post(env, request) {
  const resp = await env.SELF.fetch(request);
  const text = await resp.text();
  return [resp.headers.get('observed-content-length'), text];
}

function requestWith(body) {
  return new Request('http://example.org', { method: 'POST', body });
}

export const forkBeforeReadKeepsDeclaredLength = {
  async test(ctrl, env) {
    const expected = usingTsImpl ? '10' : 'null';
    const [a, b] = tenByteSource().tee();
    const [c, d] = a.tee();
    for (const request of [
      requestWith(b),
      requestWith(c),
      new Request(requestWith(d)),
    ]) {
      deepStrictEqual(await post(env, request), [expected, '0123456789']);
    }
  },
};

export const forkAfterReadSendsChunked = {
  async test(ctrl, env) {
    // Read before the tee: both branches, a branch teed again, and the
    // copy new Request() makes.
    {
      const rs = tenByteSource();
      strictEqual(await readChunk(rs), '01234');
      const [a, b] = rs.tee();
      const [c, d] = a.tee();
      for (const request of [
        requestWith(b),
        requestWith(c),
        new Request(requestWith(d)),
      ]) {
        deepStrictEqual(await post(env, request), ['null', '56789']);
      }
    }

    // A branch read, then teed again: its branches go out chunked, while
    // its unread sibling keeps the declared length (ledger #35).
    {
      const [a, b] = tenByteSource().tee();
      strictEqual(await readChunk(a), '01234');
      const [c, d] = a.tee();
      for (const request of [requestWith(c), new Request(requestWith(d))]) {
        deepStrictEqual(await post(env, request), ['null', '56789']);
      }
      deepStrictEqual(await post(env, requestWith(b)), [
        usingTsImpl ? '10' : 'null',
        '0123456789',
      ]);
    }
  },
};
