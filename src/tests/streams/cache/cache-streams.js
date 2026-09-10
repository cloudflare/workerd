// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The Cache API consuming stream bodies: cache.put() must drain
// whatever body shape the response carries into the cache backend. The
// backend records what arrived (byte count, pattern integrity,
// declared Content-Length) and the MOCK binding reports it back.

import { strictEqual, ok, rejects } from 'node:assert';

const PATTERN_MODULUS = 251;

function patternChunk(offset, length) {
  const chunk = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    chunk[i] = (offset + i) % PATTERN_MODULUS;
  }
  return chunk;
}

function patternedByteSource(total, chunkLength) {
  let offset = 0;
  return new ReadableStream({
    type: 'bytes',
    pull(c) {
      const length = Math.min(chunkLength, total - offset);
      c.enqueue(patternChunk(offset, length));
      offset += length;
      if (offset >= total) c.close();
    },
  });
}

async function lastPut(env) {
  const response = await env.MOCK.fetch('http://cache-backend/last-put');
  return response.json();
}

// A JS value stream of patterned Uint8Array chunks.
export const putJsValueStreamBody = {
  async test(ctrl, env) {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(patternChunk(0, 100));
        c.enqueue(patternChunk(100, 100));
        c.close();
      },
    });
    await caches.default.put(
      'https://example.com/value-stream',
      new Response(rs)
    );
    const record = await lastPut(env);
    strictEqual(record.byteLength, 200);
    strictEqual(record.patternOk, true);
  },
};

// A JS byte stream, chunked.
export const putJsByteStreamBody = {
  async test(ctrl, env) {
    await caches.default.put(
      'https://example.com/byte-stream',
      new Response(patternedByteSource(64 * 1024, 4 * 1024))
    );
    const record = await lastPut(env);
    strictEqual(record.byteLength, 64 * 1024);
    strictEqual(record.patternOk, true);
  },
};

// A FixedLengthStream body: the declared length must surface as a
// concrete Content-Length on the serialized put.
export const putFixedLengthStreamBody = {
  async test(ctrl, env) {
    const fls = new FixedLengthStream(300);
    const putP = caches.default.put(
      'https://example.com/fixed-length',
      new Response(fls.readable)
    );
    const writer = fls.writable.getWriter();
    await writer.write(patternChunk(0, 300));
    await writer.close();
    await putP;
    const record = await lastPut(env);
    strictEqual(record.byteLength, 300);
    strictEqual(record.patternOk, true);
    strictEqual(record.declaredLength, '300');
  },
};

// An IdentityTransformStream body fed by a concurrent writer.
export const putIdentityStreamBody = {
  async test(ctrl, env) {
    const its = new IdentityTransformStream();
    const putP = caches.default.put(
      'https://example.com/identity',
      new Response(its.readable)
    );
    const writer = its.writable.getWriter();
    await writer.write(patternChunk(0, 512));
    await writer.write(patternChunk(512, 512));
    await writer.close();
    await putP;
    const record = await lastPut(env);
    strictEqual(record.byteLength, 1024);
    strictEqual(record.patternOk, true);
  },
};

// A LARGE chunked body: 1 MiB through the put pipeline, byte-exact.
export const putLargeStreamBody = {
  async test(ctrl, env) {
    await caches.default.put(
      'https://example.com/large',
      new Response(patternedByteSource(1024 * 1024, 64 * 1024))
    );
    const record = await lastPut(env);
    strictEqual(record.byteLength, 1024 * 1024);
    strictEqual(record.patternOk, true);
  },
};

// A partially-read (disturbed) body must be rejected by put().
export const putDisturbedBodyRejects = {
  async test() {
    const rs = patternedByteSource(100, 50);
    const response = new Response(rs);
    const reader = response.body.getReader();
    await reader.read();
    reader.releaseLock();
    await rejects(
      caches.default.put('https://example.com/disturbed', response),
      { name: 'TypeError' }
    );
  },
};

// A locked body must be rejected by put().
export const putLockedBodyRejects = {
  async test() {
    const response = new Response(patternedByteSource(100, 50));
    response.body.getReader(); // lock, never read
    await rejects(caches.default.put('https://example.com/locked', response), {
      name: 'TypeError',
    });
  },
};

// An erroring body stream rejects the put with the source's error.
export const putErroringBodyRejects = {
  async test() {
    const boom = new Error('boom');
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(new Uint8Array(16));
      },
      pull(c) {
        c.error(boom);
      },
    });
    await rejects(
      caches.default.put('https://example.com/erroring', new Response(rs)),
      (e) => e !== boom && e.name === 'Error' && e.message === 'boom'
    );
  },
};

// The seed regression, migrated from cache-put-stream-test.js:
// response.clone() over a TransformStream body with CONCURRENT puts of
// both halves must complete while the writer feeds 1 MiB.
export const concurrentClonePuts = {
  async test() {
    const { readable, writable } = new TransformStream();
    const response = new Response(readable);
    const clone = response.clone();

    const puts = Promise.all([
      caches.default.put('https://example.com/clone-1', response),
      caches.default.put('https://example.com/clone-2', clone),
    ]);

    const writer = writable.getWriter();
    const write = (async () => {
      const chunk = new Uint8Array(64 * 1024);
      for (let i = 0; i < 16; ++i) {
        await writer.write(chunk);
      }
      await writer.close();
    })();

    const result = await Promise.race([
      Promise.all([puts, write]).then(() => 'completed'),
      scheduler.wait(5000).then(() => 'timed out'),
    ]);
    strictEqual(result, 'completed');
  },
};

// cache.match streaming out of the backend: a HIT body arrives as a
// readable stream consumable by a reader.
export const matchBodyIsReadableStream = {
  async test() {
    const response = await caches.default.match(
      'https://example.com/cached-resource'
    );
    ok(response);
    ok(response.body instanceof ReadableStream);
    const reader = response.body.getReader();
    const chunks = [];
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(...value);
    }
    strictEqual(
      new TextDecoder().decode(new Uint8Array(chunks)),
      'Cached content'
    );
  },
};
