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
// (ledger #36, a C++ defect) is pinned in js-rpc-streams-crossflag-test.js,
// as no consumer here accepts one.
//
// C++ DEFECT (ledger #37): a response whose body declares expectedLength
// (a default source or a TransformStream readable) and is compressed on
// the way out by Content-Encoding: gzip. TypeScript reports no length for
// an encoded body, so it goes out chunked and decodes in full. C++ keeps
// the uncompressed length as the Content-Length, as its tryGetLength()
// ignores the encoding, so the compressed body falls short of it and the
// reader fails with 'Network connection lost.'
//
// PARITY: a body (a default source or a TransformStream readable) that
// delivers less than its declared expectedLength fails the transfer with
// 'Network connection lost.', as a request body and as a response body:
// the declaration is the Content-Length, which the transport enforces.

import { deepStrictEqual, rejects, strictEqual } from 'node:assert';
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

const PAYLOAD = 'compress me please '.repeat(2000);

const BODY_KINDS = ['default', 'transform'];

// A body that declares expectedLength and delivers text: a default
// source, or a TransformStream's readable through its transformer's
// expectedLength.
function declaredBody(kind, expectedLength, text) {
  const bytes = enc.encode(text);
  if (kind === 'transform') {
    const { readable, writable } = new TransformStream({ expectedLength });
    const writer = writable.getWriter();
    writer.write(bytes);
    writer.close();
    return readable;
  }
  return new ReadableStream({
    expectedLength,
    start(c) {
      c.enqueue(bytes);
      c.close();
    },
  });
}

// Declares 10 bytes and delivers 5.
function shortBody(kind) {
  return declaredBody(kind, 10, '01234');
}

// Served by main.js at /gzip?body=<kind>: a body declaring PAYLOAD's
// uncompressed length, sent with Content-Encoding: gzip, which compresses
// it on the way out.
export function compressedResponse(url) {
  const body = declaredBody(
    url.searchParams.get('body'),
    enc.encode(PAYLOAD).length,
    PAYLOAD
  );
  return new Response(body, { headers: { 'content-encoding': 'gzip' } });
}

// Served by main.js at /short?body=<kind>.
export function shortResponse(url) {
  return new Response(shortBody(url.searchParams.get('body')));
}

// PARITY: the declared length is the Content-Length, so a body that falls
// short of it fails the transfer rather than going out complete.
export const shortBodyFailsTransfer = {
  async test(ctrl, env) {
    const lost = { message: 'Network connection lost.' };
    for (const kind of BODY_KINDS) {
      await rejects(post(env, requestWith(shortBody(kind))), lost, kind);
      const resp = await env.SELF.fetch(
        `http://example.org/short?body=${kind}`
      );
      await rejects(resp.text(), lost, kind);
    }
  },
};

// C++ DEFECT (ledger #37)
export const compressedResponseDropsDeclaredLength = {
  async test(ctrl, env) {
    for (const kind of BODY_KINDS) {
      const resp = await env.SELF.fetch(`http://example.org/gzip?body=${kind}`);
      strictEqual(resp.headers.get('content-encoding'), 'gzip', kind);
      if (usingTsImpl) {
        strictEqual(await resp.text(), PAYLOAD, kind);
      } else {
        await rejects(resp.text(), { message: 'Network connection lost.' });
      }
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
