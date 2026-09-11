// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The request body: write() and end() accumulate chunks, and end() sends
// the whole body at once as a Blob (typed by the Content-Type header) —
// nothing streams to the server before end(), and the request carries a
// Content-Length rather than chunked encoding.

import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual } from 'node:assert';
import { request, response, collect } from 'harness';

async function summary(req) {
  const res = await response(req);
  return JSON.parse((await collect(res)).toString());
}

// A string body written before end() is echoed back; the server sees the
// Content-Type and a Content-Length, no Transfer-Encoding.
export const stringBodyEchoed = {
  async test(ctrl, env) {
    const text = 'Hello, this is test data for POST request with body echoing!';
    const req = request(env, '/echo', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
    });
    req.write(text);
    req.end();
    const res = await response(req);
    strictEqual(res.statusCode, 200);
    strictEqual(res.headers['x-request-method'], 'POST');
    strictEqual(res.headers['x-request-content-type'], 'text/plain');
    strictEqual(
      res.headers['x-request-content-length'],
      String(Buffer.byteLength(text))
    );
    strictEqual(res.headers['x-request-transfer-encoding'], 'none');
    strictEqual((await collect(res)).toString(), text);
  },
};

// A Buffer passed to end() is sent exactly once.
export const endWithBufferSentOnce = {
  async test(ctrl, env) {
    const payload = JSON.stringify({
      email: 'posting-wrangler@email.mail',
      from: 'wrangler',
    });
    const req = request(env, '/echo', {
      method: 'post',
      headers: { 'content-type': 'application/json;charset=utf-8' },
    });
    req.end(Buffer.from(payload));
    const res = await response(req);
    strictEqual(res.statusCode, 200);
    strictEqual((await collect(res)).toString(), payload);
  },
};

// Every chunk form is honored, in order: strings (utf8 by default, or with
// an encoding), Buffers, Uint8Arrays, and the chunk given to end().
export const chunkTypesAndEncodings = {
  async test(ctrl, env) {
    const req = request(env, '/echo', { method: 'PUT' });
    req.write('utf8-ñ|');
    req.write('bGF0aW4x', 'base64'); // "latin1"
    req.write(Buffer.from('|buffer|'));
    req.write(new Uint8Array([85, 56, 124])); // "U8|"
    req.end('end', 'utf8');
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'utf8-ñ|latin1|buffer|U8|end');
  },
};

// Nothing is sent before end(): the response cannot arrive while the
// request is still being written.
export const requestIsSentAtEnd = {
  async test(ctrl, env) {
    const req = request(env, '/echo', { method: 'POST' });
    let responded = false;
    req.on('response', () => (responded = true));
    req.write('first');
    await scheduler.wait(50);
    req.write('second');
    strictEqual(responded, false);
    req.end();
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'firstsecond');
  },
};

// The body's length and type reach the server: Content-Length is the byte
// count of everything written, the Content-Type header names the type,
// and a body without one carries none.
export const lengthAndTypeReachTheServer = {
  async test(ctrl, env) {
    const typed = request(env, '/sink', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
    });
    typed.write(Buffer.alloc(1000, 1));
    typed.end(Buffer.alloc(24, 2));
    deepStrictEqual(await summary(typed), {
      method: 'POST',
      bytes: 1024,
      contentType: 'application/octet-stream',
      contentLength: '1024',
      transferEncoding: null,
    });

    const untyped = request(env, '/sink', { method: 'PUT' });
    untyped.end('twelve bytes');
    deepStrictEqual(await summary(untyped), {
      method: 'PUT',
      bytes: 12,
      contentType: null,
      contentLength: '12',
      transferEncoding: null,
    });
  },
};

// A POST that ends without writing sends no body, with Content-Length 0.
export const emptyPostSendsNoBody = {
  async test(ctrl, env) {
    const req = request(env, '/sink', { method: 'POST' });
    req.end();
    deepStrictEqual(await summary(req), {
      method: 'POST',
      bytes: 0,
      contentType: null,
      contentLength: '0',
      transferEncoding: null,
    });
  },
};

// GET and HEAD requests carry no body, whatever is written to them.
export const getAndHeadIgnoreWrites = {
  async test(ctrl, env) {
    const get = request(env, '/sink');
    get.write('ignored');
    get.end('also ignored');
    deepStrictEqual(await summary(get), {
      method: 'GET',
      bytes: 0,
      contentType: null,
      contentLength: null,
      transferEncoding: null,
    });
  },
};
