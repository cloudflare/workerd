// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// What becomes of the buffers a handler writes. The body is a byte stream
// whose enqueue would transfer (detach) whatever it is given, so the
// response copies every chunk as it flushes it: a written buffer stays the
// caller's — reusable once the write's callback has fired, as in Node —
// and views over memory that cannot be transferred (a SharedArrayBuffer, a
// WebAssembly.Memory) are written like any other.

import { strictEqual, ok } from 'node:assert';
import { withServer } from 'harness';

const enc = new TextEncoder();

// The idiomatic fill / write / refill loop: after the write's callback the
// buffer is intact and refillable; the second write sends the new bytes
// and the first bytes went out as they were.
export const writtenBufferIsReusableAfterCallback = {
  async test(ctrl, env) {
    let observed;
    await withServer(
      async (req, res) => {
        res.writeHead(200);
        const buffer = new Uint8Array(4);
        buffer.set(enc.encode('abcd'));
        await new Promise((resolve) => res.write(buffer, resolve));
        observed = {
          detached: buffer.buffer.detached,
          length: buffer.byteLength,
        };
        buffer.set(enc.encode('wxyz'));
        await new Promise((resolve) => res.write(buffer, resolve));
        buffer.fill(0);
        res.end();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'abcdwxyz');
        strictEqual(observed.detached, false);
        strictEqual(observed.length, 4);
      }
    );
  },
};

// A chunk handed to end() is flushed synchronously; it stays the caller's
// too, and a Buffer (a view over a larger allocation) is treated the same.
export const chunkGivenToEndStaysUsable = {
  async test(ctrl, env) {
    let observed;
    await withServer(
      (req, res) => {
        res.writeHead(200);
        const whole = new Uint8Array(8);
        whole.set(enc.encode('12345678'));
        const view = whole.subarray(2, 6);
        res.end(view);
        observed = {
          detached: whole.buffer.detached,
          viewLength: view.byteLength,
          text: new TextDecoder().decode(whole),
        };
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), '3456');
        strictEqual(observed.detached, false);
        strictEqual(observed.viewLength, 4);
        strictEqual(observed.text, '12345678');
      }
    );
  },
};

// A write is captured when it is flushed: a mutation after the write's
// callback is not sent. (A mutation before the flush — in the same tick as
// the write — is, as with Node's corked socket.)
export const mutationAfterCallbackIsNotSent = {
  async test(ctrl, env) {
    await withServer(
      async (req, res) => {
        res.writeHead(200);
        const buffer = enc.encode('keep');
        await new Promise((resolve) => res.write(buffer, resolve));
        buffer.set(enc.encode('lost'));
        await new Promise((resolve) => setTimeout(resolve, 10));
        res.end();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'keep');
      }
    );
  },
};

// Views over a SharedArrayBuffer and over a WebAssembly.Memory cannot be
// transferred; they are written all the same, and the response completes
// normally.
export const sharedAndWasmMemoryViewsAreWritten = {
  async test(ctrl, env) {
    const events = [];
    await withServer(
      (req, res) => {
        res.on('error', (err) => events.push(`error(${err.message})`));
        res.on('finish', () => events.push('finish'));
        const shared = new Uint8Array(new SharedArrayBuffer(4));
        shared.set(enc.encode('SAB!'));
        const memory = new WebAssembly.Memory({ initial: 1 });
        const wasm = new Uint8Array(memory.buffer, 0, 4);
        wasm.set(enc.encode('WASM'));
        res.writeHead(200);
        res.write(shared);
        res.write(wasm);
        res.end(shared.subarray(0, 1));
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'SAB!WASMS');
        strictEqual(events.length, 1);
        strictEqual(events[0], 'finish');
      }
    );
  },
};

// The copy is of the trimmed bytes when a Content-Length caps the body;
// the buffers written past the cap stay untouched too.
export const trimmedWriteLeavesBufferIntact = {
  async test(ctrl, env) {
    let observed;
    await withServer(
      async (req, res) => {
        res.writeHead(200, { 'Content-Length': '6' });
        const first = enc.encode('abcd');
        const second = enc.encode('efgh');
        const third = enc.encode('ijkl');
        res.write(first);
        res.write(second);
        await new Promise((resolve) => res.write(third, resolve));
        observed = [first, second, third].map((b) => b.buffer.detached);
        res.end();
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'abcdef');
        strictEqual(observed.some(Boolean), false);
      }
    );
  },
};

// A zero-length view — including one over a detached buffer — contributes
// nothing and is accepted, as an empty write is.
export const emptyAndDetachedViewsContributeNothing = {
  async test(ctrl, env) {
    let returned;
    await withServer(
      (req, res) => {
        res.writeHead(200);
        const gone = new Uint8Array(4);
        structuredClone(gone.buffer, { transfer: [gone.buffer] });
        ok(gone.buffer.detached);
        returned = [res.write(new Uint8Array(0)), res.write(gone)];
        res.end('tail');
      },
      async () => {
        const res = await env.SERVICE.fetch('http://x/');
        strictEqual(await res.text(), 'tail');
        strictEqual(returned[0], true);
        strictEqual(returned[1], true);
      }
    );
  },
};
