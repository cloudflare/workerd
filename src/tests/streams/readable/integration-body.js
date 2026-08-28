// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStream as a Request/Response body: the readAll consumption
// paths, chunk normalization, cloning, and full fetch round-trips
// through the SELF echo service. Migrated from streams-test.js and the
// parity-parked ts-webstreams-test.js body tests. Everything here is
// PARITY, message texts included, except where branched.

import { strictEqual, ok, rejects } from 'node:assert';

const enc = new TextEncoder();

// Request.text() over small stream chunks (migrated).
export const readAllTextRequestSmall = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('hello '));
        c.enqueue(enc.encode('world!'));
        c.close();
      },
    });
    const request = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });
    strictEqual(await request.text(), 'hello world!');
  },
};

// Request.text() over multi-page chunks (migrated).
export const readAllTextRequestBig = {
  async test() {
    const chunks = [
      'a'.repeat(4097),
      'b'.repeat(4097 * 2),
      'c'.repeat(4097 * 4),
    ];
    let check = '';
    const rs = new ReadableStream({
      pull(c) {
        if (chunks.length === 0) {
          c.close();
          return;
        }
        const chunk = chunks.shift();
        check += chunk;
        c.enqueue(enc.encode(chunk));
      },
    });
    const request = new Request('http://example.org', {
      method: 'POST',
      body: rs,
    });
    const text = await request.text();
    strictEqual(text.length, check.length);
    strictEqual(text, check);
  },
};

// A pull() that throws mid-consumption rejects the consumer (migrated).
export const readAllTextFailedPull = {
  async test() {
    const rs = new ReadableStream({
      async pull() {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });
    await rejects(new Response(rs).text(), { message: 'boom' });
  },
};

// An async start() rejection rejects the consumer (migrated).
export const readAllTextFailedStart = {
  async test() {
    const rs = new ReadableStream({
      async start() {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });
    await rejects(new Response(rs).text(), { message: 'boom' });
  },
};

// controller.error() during consumption rejects the consumer; starting
// the consumption locks the stream (migrated).
export const readAllTextControllerError = {
  async test() {
    const rs = new ReadableStream({
      async start(c) {
        await scheduler.wait(10);
        c.error(new Error('boom'));
      },
    });
    const response = new Response(rs);
    ok(!rs.locked);
    const promise = response.text();
    ok(rs.locked);
    await rejects(promise, { message: 'boom' });
  },
};

// A large streamed body consumes intact (from api/streams/streams-test's
// ResponseTextLargeBody, reduced to deterministic chunking).
export const readAllTextLargeBody = {
  async test() {
    const chunk = 'x'.repeat(65536);
    const COUNT = 16;
    let sent = 0;
    const rs = new ReadableStream({
      pull(c) {
        if (sent < COUNT) {
          sent++;
          c.enqueue(enc.encode(chunk));
        } else {
          c.close();
        }
      },
    });
    const text = await new Response(rs).text();
    strictEqual(text.length, 65536 * COUNT);
    strictEqual(text[0], 'x');
    strictEqual(text[text.length - 1], 'x');
  },
};

// Body consumption normalizes BufferSource chunks: bare ArrayBuffers,
// non-zero-offset DataViews, and typed arrays all contribute their
// bytes; a detached buffer contributes nothing; a non-BufferSource chunk
// rejects with the SAME TypeError message on both sides (parity; the
// parity-parked ts-webstreams test, now shared).
export const bodyConsumptionNormalizesBufferSourceChunks = {
  async test() {
    const bodyOf = (chunks) =>
      new ReadableStream({
        start(c) {
          for (const chunk of chunks) c.enqueue(chunk);
          c.close();
        },
      });
    strictEqual(
      await new Response(bodyOf([enc.encode('ab').buffer])).text(),
      'ab'
    );
    strictEqual(
      await new Response(
        bodyOf([new DataView(enc.encode('_cd').buffer, 1)])
      ).text(),
      'cd'
    );
    strictEqual(
      (await new Response(bodyOf([new Float64Array(1)])).arrayBuffer())
        .byteLength,
      8
    );
    const detached = new ArrayBuffer(4);
    detached.transfer();
    strictEqual(
      await new Response(bodyOf([detached, enc.encode('x')])).text(),
      'x'
    );
    await rejects(new Response(bodyOf(['not bytes'])).text(), {
      name: 'TypeError',
      message: 'This ReadableStream did not return bytes.',
    });
  },
};

// Response.clone() with a stream body: both copies read the full body
// (parity; tee under the hood).
export const cloneWithStreamBody = {
  async test() {
    const rs = new ReadableStream({
      start(c) {
        c.enqueue(enc.encode('clone'));
        c.enqueue(enc.encode(' me'));
        c.close();
      },
    });
    const response = new Response(rs);
    const clone = response.clone();
    const [a, b] = await Promise.all([response.text(), clone.text()]);
    strictEqual(a, 'clone me');
    strictEqual(b, 'clone me');
  },
};

// Cancelling response.body first makes consumption reject with the
// body-already-used TypeError (parity, message included; migrated from
// streams-test.js cancelStreamRejectsBodyConsume).
export const cancelBodyThenConsume = {
  async test() {
    const response = new Response('foo bar');
    response.body.cancel(new Error('a good reason'));
    await rejects(response.text(), {
      name: 'TypeError',
      message: /Body has already been used/,
    });
  },
};

// Full fetch round-trip: a JS ReadableStream as a fetch() body, echoed
// back by the SELF service and read via async iteration (the
// streams-test.js rs shape, workerd#5113 family).
export const fetchBodyRoundtrip = {
  async test(ctrl, env) {
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body: new ReadableStream({
        expectedLength: 10,
        start(c) {
          c.enqueue(enc.encode('hellohello'));
          c.close();
        },
      }),
    });
    const chunks = [];
    for await (const chunk of resp.body) {
      chunks.push(...chunk);
    }
    strictEqual(new TextDecoder().decode(new Uint8Array(chunks)), 'hellohello');
  },
};

// The same round-trip with the stream attached via new Request() (the
// streams-test.js rsRequest shape, workerd#5113 regression).
export const fetchRequestBodyRoundtrip = {
  async test(ctrl, env) {
    const resp = await env.SELF.fetch(
      new Request('http://example.org', {
        method: 'POST',
        body: new ReadableStream({
          expectedLength: 10,
          start(c) {
            c.enqueue(enc.encode('hellohello'));
            c.close();
          },
        }),
      })
    );
    strictEqual(await resp.text(), 'hellohello');
  },
};

// Round-trip through the echo service and BACK INTO a consumer: the
// response body (a native-backed stream) drains via .text().
export const echoedBodyIsConsumable = {
  async test(ctrl, env) {
    const body = 'stream me '.repeat(1000);
    const resp = await env.SELF.fetch('http://example.org', {
      method: 'POST',
      body,
    });
    strictEqual(await resp.text(), body);
  },
};
