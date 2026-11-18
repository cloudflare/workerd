// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The body of a Request or Response made from a string, BufferSource or
// Blob: a byte stream that copies the value into a new chunk for each
// read. A value of up to 128 KiB is one chunk, and a longer one is split
// into chunks of up to 128 KiB, each of a string complete UTF-8 on its
// own. The stream declares the value's length. PARITY throughout.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

const MAX_CHUNK = 128 * 1024;
const enc = new TextEncoder();

async function readChunks(stream) {
  const reader = stream.getReader();
  const chunks = [];
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return chunks;
    chunks.push(value);
  }
}

function concat(chunks) {
  const result = new Uint8Array(chunks.reduce((n, c) => n + c.byteLength, 0));
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return result;
}

// Checks that each chunk is at most MAX_CHUNK bytes and decodes on its own
// with a fatal decoder, and that together they are `expected`.
function checkTextChunks(chunks, expected, label) {
  for (const chunk of chunks) {
    ok(chunk.byteLength > 0, label);
    ok(chunk.byteLength <= MAX_CHUNK, label);
    new TextDecoder('utf-8', { fatal: true }).decode(chunk);
  }
  deepStrictEqual(concat(chunks), expected, label);
}

export const inMemoryBodySmallValueIsOneChunk = {
  async test() {
    const text = 'héllo \u{1F600}';
    const bytes = enc.encode(text);
    for (const [label, value] of [
      ['string', text],
      ['ArrayBuffer', bytes.buffer.slice(0)],
      ['view', new Uint8Array([0, ...bytes, 0]).subarray(1, bytes.length + 1)],
      ['Blob', new Blob([text])],
    ]) {
      const chunks = await readChunks(new Response(value).body);
      strictEqual(chunks.length, 1, label);
      deepStrictEqual(chunks[0], bytes, label);
    }
  },
};

export const inMemoryBodyEmptyValueClosesWithoutChunks = {
  async test(ctrl, env) {
    for (const value of ['', new ArrayBuffer(0), new Blob([])]) {
      deepStrictEqual(await readChunks(new Response(value).body), []);
    }
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body: '',
    });
    strictEqual(resp.headers.get('observed-content-length'), '0');
    strictEqual(await resp.text(), '');
  },
};

// A 7-byte unit, whose length does not divide the chunk size, so that
// code points straddle every chunk boundary, with lone surrogates mixed in.
function longText() {
  let text = '';
  for (let i = 0; i < 4; i++) {
    text += 'é\u{1F600}x'.repeat(20000) + '\uD800' + '\uDFFF';
  }
  return text;
}

export const inMemoryBodyChunksAreCompleteUtf8 = {
  async test() {
    const text = longText();
    const expected = enc.encode(text);
    ok(expected.byteLength > 4 * MAX_CHUNK);
    checkTextChunks(
      await readChunks(new Response(text).body),
      expected,
      'default reader'
    );
    const [a, b] = new Response(text).body.tee();
    const [chunksA, chunksB] = await Promise.all([
      readChunks(a),
      readChunks(b),
    ]);
    checkTextChunks(chunksA, expected, 'first branch');
    checkTextChunks(chunksB, expected, 'second branch');
  },
};

export const inMemoryBodyLongBufferInChunks = {
  async test() {
    const bytes = new Uint8Array(2 * MAX_CHUNK + 10);
    for (let i = 0; i < bytes.length; i++) bytes[i] = i * 7;
    for (const value of [bytes, new Blob([bytes])]) {
      const chunks = await readChunks(new Response(value).body);
      deepStrictEqual(
        chunks.map((c) => c.byteLength),
        [MAX_CHUNK, MAX_CHUNK, 10]
      );
      deepStrictEqual(concat(chunks), bytes);
    }
  },
};

export const inMemoryBodyWritesLoneSurrogatesAsReplacement = {
  async test() {
    const response = new Response('a\uD800b');
    deepStrictEqual(
      new Uint8Array(await response.clone().arrayBuffer()),
      new Uint8Array([0x61, 0xef, 0xbf, 0xbd, 0x62])
    );
    strictEqual(await response.text(), 'a�b');
  },
};

export const inMemoryBodyReadsIntoByobViews = {
  async test() {
    const reader = new Response('hello world').body.getReader({
      mode: 'byob',
    });
    let text = '';
    for (;;) {
      const { value, done } = await reader.read(new Uint8Array(4));
      if (done) break;
      ok(value.byteLength <= 4);
      text += new TextDecoder().decode(value);
    }
    strictEqual(text, 'hello world');
  },
};

export const inMemoryBodyIsCopiedAtConstruction = {
  async test() {
    const bytes = enc.encode('original');
    const response = new Response(bytes);
    bytes.fill(0x78);
    structuredClone(bytes.buffer, { transfer: [bytes.buffer] });
    strictEqual(await response.text(), 'original');
  },
};

export const inMemoryBodyDeclaresItsLength = {
  async test(ctrl, env) {
    const text = 'héllo \u{1F600}';
    for (const body of [text, new Response(text).body]) {
      const resp = await env.SELF.fetch('http://example.org', {
        method: 'POST',
        body,
      });
      strictEqual(
        resp.headers.get('observed-content-length'),
        String(enc.encode(text).byteLength)
      );
      strictEqual(await resp.text(), text);
    }
  },
};

const PAYLOAD = 'compress me please '.repeat(20000);

// Served by main.js at /gzip-in-memory-body: an in-memory body's stream,
// sent with Content-Encoding: gzip, which compresses it on the way out.
export function compressedInMemoryBodyResponse() {
  return new Response(new Response(PAYLOAD).body, {
    headers: { 'content-encoding': 'gzip' },
  });
}

// The declared length is the uncompressed one, so the encoded body goes
// out with none.
export const inMemoryBodyCompressedResponseDropsLength = {
  async test(ctrl, env) {
    const resp = await env.SELF.fetch('http://example.org/gzip-in-memory-body');
    strictEqual(resp.headers.get('content-encoding'), 'gzip');
    strictEqual(await resp.text(), PAYLOAD);
  },
};
