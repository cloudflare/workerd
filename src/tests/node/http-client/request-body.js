// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The request body: write() and end() accumulate chunks, and end() sends
// the whole body at once as a Blob (typed by the Content-Type header) —
// nothing streams to the server before end(), and the request carries a
// Content-Length rather than chunked encoding.

import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { request, response, collect, once, record } from 'harness';

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

// A chunk's bytes are captured at write(), as Node has them on the wire or
// copied into its pending queue by then: what the caller does to the
// buffer afterwards — mutate it, detach it, shrink it — does not change
// what is sent.
export const chunkIsCapturedAtWrite = {
  async test(ctrl, env) {
    const enc = new TextEncoder();
    const req = request(env, '/echo', { method: 'POST' });
    const mutated = enc.encode('1234');
    req.write(mutated);
    mutated.set(enc.encode('5678'));

    const detached = enc.encode('AB');
    req.write(detached);
    structuredClone(detached.buffer, { transfer: [detached.buffer] });
    strictEqual(detached.buffer.detached, true);

    const resizable = new ArrayBuffer(4, { maxByteLength: 8 });
    const shrunk = new Uint8Array(resizable);
    shrunk.set(enc.encode('abcd'));
    req.write(shrunk);
    resizable.resize(2);
    strictEqual(shrunk.byteLength, 2);

    req.end();
    const res = await response(req);
    strictEqual((await collect(res)).toString(), '1234ABabcd');
  },
};

// Views over a SharedArrayBuffer and over a WebAssembly.Memory are sent as
// any other chunk; a zero-length view, a detached one included, sends
// nothing and is accepted.
export const sharedWasmEmptyAndDetachedViews = {
  async test(ctrl, env) {
    const enc = new TextEncoder();
    const req = request(env, '/echo', { method: 'POST' });
    const shared = new Uint8Array(new SharedArrayBuffer(4));
    shared.set(enc.encode('SAB!'));
    const memory = new WebAssembly.Memory({ initial: 1 });
    const wasm = new Uint8Array(memory.buffer, 0, 4);
    wasm.set(enc.encode('WASM'));
    const gone = new Uint8Array(4);
    structuredClone(gone.buffer, { transfer: [gone.buffer] });
    strictEqual(req.write(shared), true);
    strictEqual(req.write(new Uint8Array(0)), true);
    strictEqual(req.write(gone), true);
    strictEqual(req.write(wasm), true);
    req.end();
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'SAB!WASM');
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

// end() ends the body: `writableEnded` is set at once, and a write()
// afterwards — also one issued from inside 'finish' — is a write after
// end: it returns false, its callback and the request's 'error' report
// ERR_STREAM_WRITE_AFTER_END, nothing of it is sent, and the request
// completes with what was written before.
export const writeAfterEndFails = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/echo', { method: 'POST' });
    record(log, 'req', req, ['error']);
    req.on('finish', () => {
      strictEqual(
        req.write('from finish', (err) => log.push(`finish-write:${err.code}`)),
        false
      );
    });
    req.write('kept');
    strictEqual(req.writableEnded, false);
    req.end();
    strictEqual(req.writableEnded, true);
    strictEqual(
      req.write('late', (err) => log.push(`late-write:${err.code}`)),
      false
    );
    const res = await once(req, 'response');
    strictEqual((await collect(res)).toString(), 'kept');
    await once(req, 'close');
    deepStrictEqual(log, [
      'late-write:ERR_STREAM_WRITE_AFTER_END',
      'req:error(Error/ERR_STREAM_WRITE_AFTER_END/write after end)',
      'finish-write:ERR_STREAM_WRITE_AFTER_END',
      'req:error(Error/ERR_STREAM_WRITE_AFTER_END/write after end)',
    ]);
  },
};

// end(chunk) after end() is a write after end too (callback and 'error',
// reported after the 'finish' the first end() had already scheduled); the
// request, already ended, is still sent and answered. A bare end(cb) after
// end() calls back once the request has finished, or at once with
// ERR_STREAM_ALREADY_FINISHED if it already has — also once the exchange
// is over and the request destroyed.
export const endAfterEndReportsAndStillSends = {
  async test(ctrl, env) {
    const log = [];
    const req = request(env, '/echo', { method: 'POST' });
    record(log, 'req', req, ['error']);
    req.end('kept');
    req.end('late', (err) => log.push(`late-end:${err.code}`));
    req.end((err) =>
      log.push(`bare-end:${err === undefined ? 'ok' : err.code}`)
    );
    const res = await once(req, 'response');
    strictEqual((await collect(res)).toString(), 'kept');
    await once(req, 'close');
    req.end((err) => log.push(`after-finish-end:${err.code}`));
    await scheduler.wait(5);
    deepStrictEqual(log, [
      'bare-end:ok',
      'late-end:ERR_STREAM_WRITE_AFTER_END',
      'req:error(Error/ERR_STREAM_WRITE_AFTER_END/write after end)',
      'after-finish-end:ERR_STREAM_ALREADY_FINISHED',
    ]);
  },
};

// A chunk that is neither a string nor a byte view is refused synchronously
// with ERR_INVALID_ARG_TYPE, before anything is captured.
export const invalidChunkThrows = {
  async test(ctrl, env) {
    const req = request(env, '/echo', { method: 'POST' });
    for (const chunk of [42, {}, [1, 2], true]) {
      throws(() => req.write(chunk), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
      });
    }
    req.end('ok');
    const res = await response(req);
    strictEqual((await collect(res)).toString(), 'ok');
  },
};
