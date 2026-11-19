// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The body of a Request or Response made from a string, BufferSource or
// Blob: a byte stream that copies the value into a new chunk for each
// read. A value of up to 128 KiB is one chunk, and a longer one is split
// into chunks of up to 128 KiB, each of a string ending on a code point
// boundary. A lone surrogate in a string is written as U+FFFD under
// string_body_replace_surrogates, and as WTF-8 otherwise. The stream
// declares the value's length. PARITY throughout.

import { deepStrictEqual, ok, strictEqual } from 'node:assert';

const MAX_CHUNK = 128 * 1024;
const enc = new TextEncoder();
const replaceSurrogates =
  globalThis.Cloudflare.compatibilityFlags['string_body_replace_surrogates'] ===
  true;

// Encodes `text` as WTF-8: as UTF-8, but with a lone surrogate written as the
// three bytes UTF-8 would give its code point.
function wtf8(text) {
  const bytes = [];
  for (const char of text) {
    const code = char.codePointAt(0);
    if (code >= 0xd800 && code <= 0xdfff) {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f));
      bytes.push(0x80 | (code & 0x3f));
    } else {
      bytes.push(...enc.encode(char));
    }
  }
  return new Uint8Array(bytes);
}

// The bytes of a string body.
function encodeBody(text) {
  return replaceSurrogates ? enc.encode(text) : wtf8(text);
}

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

// Checks that each chunk is at most MAX_CHUNK bytes and starts a code point
// rather than continuing one, and that together they are `expected`.
function checkTextChunks(chunks, expected, label) {
  for (const chunk of chunks) {
    ok(chunk.byteLength > 0, label);
    ok(chunk.byteLength <= MAX_CHUNK, label);
    ok((chunk[0] & 0xc0) !== 0x80, label);
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

export const inMemoryBodyChunksEndOnCodePoints = {
  async test() {
    const text = longText();
    const expected = encodeBody(text);
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

export const inMemoryBodyWritesLoneSurrogates = {
  async test() {
    const surrogate = replaceSurrogates
      ? [0xef, 0xbf, 0xbd]
      : [0xed, 0xa0, 0x80];
    const expected = new Uint8Array([0x61, ...surrogate, 0x62]);
    // A decoder replaces each of the WTF-8 bytes, which are not valid UTF-8.
    const text = replaceSurrogates ? 'a\uFFFDb' : 'a\uFFFD\uFFFD\uFFFDb';
    // Read through the body's stream, and straight from the string.
    deepStrictEqual(
      concat(await readChunks(new Response('a\uD800b').body)),
      expected
    );
    strictEqual(await new Response(new Response('a\uD800b').body).text(), text);
    deepStrictEqual(
      new Uint8Array(await new Response('a\uD800b').arrayBuffer()),
      expected
    );
    strictEqual(await new Response('a\uD800b').text(), text);
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

// The body of each kind a value can make, and the bytes its stream produces.
function readSources() {
  const text = '"a=b\uD800"';
  const bytes = encodeBody(text);
  return [
    ['string', () => text, bytes],
    ['string with a BOM', () => '\uFEFF' + text, encodeBody('\uFEFF' + text)],
    ['empty string', () => '', new Uint8Array(0)],
    ['Uint8Array', () => bytes.slice(), bytes],
    ['ArrayBuffer', () => bytes.slice().buffer, bytes],
    ['Blob', () => new Blob([bytes]), bytes],
    ['URLSearchParams', () => new URLSearchParams('a=b'), enc.encode('a=b')],
  ];
}

// A stream of `bytes` that closes only when it is read, as an in-memory
// body's stream does: a stream already closed when it is read is left
// unlocked by the legacy implementation.
function streamOf(bytes) {
  return new ReadableStream(
    {
      pull(controller) {
        if (bytes.byteLength > 0) controller.enqueue(bytes.slice());
        controller.close();
      },
    },
    { highWaterMark: 0 }
  );
}

// Each way JavaScript reads a whole body, with what it read in a form that
// deepStrictEqual() compares.
const READS = {
  arrayBuffer: async (r) => [...new Uint8Array(await r.arrayBuffer())],
  bytes: async (r) => [...(await r.bytes())],
  text: (r) => r.text(),
  json: (r) => r.json(),
  formData: async (r) => [...(await r.formData())],
  blob: async (r) => {
    const blob = await r.blob();
    return { type: blob.type, text: await blob.text() };
  },
};

async function outcome(promise) {
  try {
    return { value: await promise };
  } catch (e) {
    return { error: `${e.name}: ${e.message}` };
  }
}

// How a used Request or Response shows it: each further use, and how it
// fails. Each use is made on a body that `makeUsed` has just used, so that
// no earlier use creates the body's stream first.
async function usedState(makeUsed, env) {
  const uses = {
    bodyUsed: (r) => r.bodyUsed,
    locked: (r) => r.body?.locked,
    getReader: (r) => r.body.getReader(),
    cancel: (r) => r.body.cancel(),
    tee: (r) => r.body.tee(),
    clone: (r) => r.clone(),
    newResponse: (r) => new Response(r.body),
  };
  for (const [read, consume] of Object.entries(READS)) {
    uses[read] = consume;
  }
  const state = {};
  for (const [name, use] of Object.entries(uses)) {
    const r = await makeUsed();
    state[name] = await outcome((async () => use(r))());
  }
  if ((await makeUsed()) instanceof Request) {
    const requestUses = {
      newRequest: (r) => new Request(r).bodyUsed,
      requestAsInit: async (r) => {
        const request = new Request('http://example.org', r);
        return {
          bodyUsed: request.bodyUsed,
          text: await outcome(request.text()),
        };
      },
      fetch: async (r) => (await env.SELF.fetch(r)).status,
    };
    for (const [name, use] of Object.entries(requestUses)) {
      const r = await makeUsed();
      state[name] = await outcome((async () => use(r))());
    }
  }
  return state;
}

// A body made from a value is read straight from the value, without the
// body's stream, but reads what its stream would, and is left as reading
// its stream would leave it.
export const inMemoryBodyReadsLikeItsStream = {
  async test(ctrl, env) {
    const headers = { 'content-type': 'application/x-www-form-urlencoded' };
    const kinds = {
      Response: (body) => new Response(body, { headers }),
      Request: (body) =>
        new Request('http://example.org', {
          method: 'POST',
          body,
          headers,
          duplex: 'half',
        }),
    };
    for (const [kind, make] of Object.entries(kinds)) {
      for (const [read, consume] of Object.entries(READS)) {
        for (const [name, value, bytes] of readSources()) {
          const label = `${kind} of ${name}, ${read}()`;
          deepStrictEqual(
            await outcome(consume(make(value()))),
            await outcome(consume(make(streamOf(bytes)))),
            label
          );
          const usedBy = (body) => async () => {
            const r = make(body());
            await outcome(consume(r));
            return r;
          };
          deepStrictEqual(
            await usedState(usedBy(value), env),
            await usedState(
              usedBy(() => streamOf(bytes)),
              env
            ),
            label
          );
        }
      }
    }
  },
};

// Sent, a body made from a value is left as a sent stream would leave it.
export const inMemoryBodySentLikeItsStream = {
  async test(ctrl, env) {
    for (const [name, value, bytes] of readSources()) {
      const sentBy = (body, init) => async () => {
        const r = new Request('http://example.org', {
          method: 'POST',
          body: body(),
          ...init,
        });
        deepStrictEqual(
          [...new Uint8Array(await (await env.SELF.fetch(r)).arrayBuffer())],
          [...bytes],
          name
        );
        return r;
      };
      deepStrictEqual(
        await usedState(sentBy(value), env),
        await usedState(
          sentBy(() => streamOf(bytes), { duplex: 'half' }),
          env
        ),
        name
      );
    }
  },
};

// The number of promise jobs that run before `promise` settles.
async function jobsToSettle(promise) {
  let settled = false;
  const settle = () => {
    settled = true;
  };
  promise.then(settle, settle);
  let jobs = 0;
  while (!settled) {
    await null;
    jobs++;
  }
  return jobs;
}

// Read straight from the value, a body's arrayBuffer(), bytes(), text() and
// formData() settle as a promise that one job resolves does: nothing is read
// through a stream.
export const inMemoryBodyReadSettlesAtOnce = {
  async test() {
    const headers = { 'content-type': 'application/x-www-form-urlencoded' };
    const oneJob = await jobsToSettle(Promise.resolve().then(() => {}));
    for (const read of ['arrayBuffer', 'bytes', 'text', 'formData']) {
      const body = new Response('a=b', { headers });
      strictEqual(await jobsToSettle(body[read]()), oneJob, read);
    }
  },
};
