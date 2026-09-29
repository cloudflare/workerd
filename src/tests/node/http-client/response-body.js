// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The response body: the fetch Response's ReadableStream pumped into the
// IncomingMessage by a default reader, one read() per _read(), with the
// Readable's push() return value as backpressure; the bytes pass through
// untouched (no decompression, as in Node).

import { Buffer } from 'node:buffer';
import { gunzipSync } from 'node:zlib';
import { strictEqual, ok, deepStrictEqual } from 'node:assert';
import { get, response, collect, once } from 'harness';

// The body arrives as Buffer chunks and ends with `complete` set; status
// and headers are on the message from the start.
export const bodyArrivesAsBuffers = {
  async test(ctrl, env) {
    const res = await response(get(env, '/pong'));
    strictEqual(res.statusCode, 200);
    strictEqual(res.complete, false);
    const chunks = [];
    res.on('data', (chunk) => chunks.push(chunk));
    await once(res, 'end');
    ok(chunks.every((chunk) => Buffer.isBuffer(chunk)));
    strictEqual(Buffer.concat(chunks).toString(), 'pong');
    strictEqual(res.complete, true);
    strictEqual(res.readableEnded, true);
  },
};

// setEncoding() turns the chunks into strings.
export const setEncodingYieldsStrings = {
  async test(ctrl, env) {
    const res = await response(get(env, '/pong'));
    res.setEncoding('utf8');
    const chunks = [];
    res.on('data', (chunk) => chunks.push(chunk));
    await once(res, 'end');
    ok(chunks.every((chunk) => typeof chunk === 'string'));
    strictEqual(chunks.join(''), 'pong');
  },
};

// A chunked body arrives as the server writes it: three writes 30 ms apart
// are three 'data' events, each in hand before the next is written.
export const chunkedBodyArrivesIncrementally = {
  async test(ctrl, env) {
    const res = await response(get(env, '/chunked?n=3&delay=30'));
    strictEqual(res.headers['transfer-encoding'], 'chunked');
    const arrivals = [];
    res.on('data', (chunk) => arrivals.push([Date.now(), chunk.toString()]));
    await once(res, 'end');
    deepStrictEqual(
      arrivals.map(([, text]) => text),
      ['chunk-0|', 'chunk-1|', 'chunk-2|']
    );
    ok(arrivals[2][0] - arrivals[0][0] >= 40, 'chunks were not coalesced');
  },
};

// A megabyte arrives whole, in several chunks, bytes intact.
export const largeBodyArrivesIntact = {
  async test(ctrl, env) {
    const bytes = 1024 * 1024;
    const res = await response(get(env, `/large?bytes=${bytes}`));
    strictEqual(res.headers['content-length'], String(bytes));
    let events = 0;
    res.on('data', () => events++);
    const body = await collect(res);
    ok(events > 1, `expected several data events, got ${events}`);
    strictEqual(body.length, bytes);
    for (const i of [0, 250, 251, 65536, bytes - 1]) {
      strictEqual(body[i], i % 251, `byte ${i}`);
    }
  },
};

// pause() holds the pump (no 'data' while paused), resume() continues it
// with the same reader; nothing is lost.
export const pauseAndResumeWithoutLoss = {
  async test(ctrl, env) {
    const bytes = 512 * 1024;
    const res = await response(get(env, `/large?bytes=${bytes}`));
    const events = [];
    let total = 0;
    res.on('data', (chunk) => {
      total += chunk.length;
      if (events.length === 0) {
        events.push('pause');
        res.pause();
        setTimeout(() => {
          events.push(`resume:${total}`);
          res.resume();
        }, 30);
      } else if (events.length === 1) {
        events.push('data while paused');
      }
    });
    await once(res, 'end');
    strictEqual(total, bytes);
    strictEqual(events.length, 2);
    strictEqual(events[0], 'pause');
    ok(events[1].startsWith('resume:'));
  },
};

// Statuses that forbid a body (204, 304), and an empty 200, end at once
// with no 'data'; the headers are there.
export const bodilessResponsesEnd = {
  async test(ctrl, env) {
    for (const status of [204, 304, 200]) {
      const res = await response(get(env, `/status/${status}`));
      strictEqual(res.statusCode, status);
      strictEqual(res.headers['x-status'], String(status));
      const body = await collect(res);
      strictEqual(body.length, 0);
      strictEqual(res.complete, true);
    }
  },
};

// The reply to a HEAD has no body: 'end' without 'data'.
export const headResponseHasNoBody = {
  async test(ctrl, env) {
    const res = await response(get(env, '/asd', { method: 'HEAD' }));
    strictEqual(res.statusCode, 200);
    strictEqual((await collect(res)).length, 0);
    strictEqual(res.complete, true);
  },
};

// A compressed body passes through as sent: the client does not
// decompress, and Content-Encoding is left for the caller.
export const compressedBodyPassesThrough = {
  async test(ctrl, env) {
    const res = await response(get(env, '/gzip'));
    strictEqual(res.headers['content-encoding'], 'gzip');
    const raw = await collect(res);
    strictEqual(gunzipSync(raw).toString(), 'hello from gzip server');
  },
};

// Without a consumer the body waits: no 'end' until the message is read.
export const bodyWaitsForAConsumer = {
  async test(ctrl, env) {
    const res = await response(get(env, '/asd'));
    let ended = false;
    res.on('end', () => (ended = true));
    await scheduler.wait(30);
    strictEqual(ended, false);
    strictEqual(res.readableFlowing, null);
    strictEqual((await collect(res)).toString(), 'asd');
    strictEqual(ended, true);
  },
};
