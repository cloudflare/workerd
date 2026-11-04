// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// String chunks in a body the runtime sends on: a fetch request body, an
// iterable request body, and a response the fetch handler returns,
// observed through the SELF service.
//
// PARITY: a string chunk is sent as its UTF-8 encoding, lone surrogates
// replaced with U+FFFD and embedded NULs kept, alongside the byte chunks
// around it. Strings are not valid body chunks (a body read in JavaScript,
// text() and the like, rejects them in both; integration-body.js), but
// the C++ implementation has always encoded them when it sends a body on,
// and code such as server-sent-event handlers relies on it.

import { deepStrictEqual, strictEqual } from 'node:assert';

const enc = new TextEncoder();

// Bytes and strings in one stream, with the UTF-8 the runtime sends for
// them.
const MIXED_CHUNKS = [enc.encode('ab'), 'cd', '', '\uD800', 'x\0y'];
const MIXED_BYTES = [
  ...enc.encode('abcd'),
  0xef,
  0xbf,
  0xbd,
  ...enc.encode('x\0y'),
];

function streamOf(chunks) {
  return new ReadableStream({
    start(c) {
      for (const chunk of chunks) c.enqueue(chunk);
      c.close();
    },
  });
}

async function echoedBytes(env, body) {
  const resp = await env.SELF.fetch('http://example.org', {
    method: 'POST',
    body,
  });
  return [...new Uint8Array(await resp.arrayBuffer())];
}

// Served by main.js at /strings: an event stream whose second event is a
// string chunk.
export function stringsResponse() {
  return new Response(
    streamOf([enc.encode('data: one\n\n'), 'data: two\n\n']),
    { headers: { 'content-type': 'text/event-stream' } }
  );
}

export const stringChunksInRequestBody = {
  async test(ctrl, env) {
    deepStrictEqual(
      await echoedBytes(env, streamOf(MIXED_CHUNKS)),
      MIXED_BYTES
    );

    // One chunk per pull, so the strings reach the pump in batches of
    // their own.
    let i = 0;
    const pulled = new ReadableStream({
      pull(c) {
        if (i < MIXED_CHUNKS.length) c.enqueue(MIXED_CHUNKS[i++]);
        else c.close();
      },
    });
    deepStrictEqual(await echoedBytes(env, pulled), MIXED_BYTES);
  },
};

export const stringChunksInIterableRequestBody = {
  async test(ctrl, env) {
    async function* body() {
      yield enc.encode('ab');
      yield 'cd';
    }
    deepStrictEqual(await echoedBytes(env, body()), [...enc.encode('abcd')]);
  },
};

export const stringChunksInReturnedResponse = {
  async test(ctrl, env) {
    const resp = await env.SELF.fetch('http://example.org/strings');
    strictEqual(await resp.text(), 'data: one\n\ndata: two\n\n');
  },
};
