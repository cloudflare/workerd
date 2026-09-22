// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The onread option: the read loop reads into the caller's buffer (or one
// the caller's generator returns) and hands each fill to the callback
// instead of pushing 'data'. The BYOB read transfers the buffer it is
// given, so a fixed buffer's original Uint8Array is detached after the
// first fill; the loop carries on over the transferred backing store, and
// each callback sees a Buffer over it whose contents are valid until the
// next read.

import { strictEqual, deepStrictEqual, ok, throws } from 'node:assert';
import { Buffer } from 'node:buffer';
import { echo, once, echoSegments } from 'servers';

// A fixed buffer keeps receiving fills across several echoed writes.
export const fixedBufferReceivesEveryFill = {
  async test(ctrl, env) {
    const buffer = Buffer.alloc(1024);
    let received = '';
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer,
        callback(nread, buf) {
          fills.push(nread);
          strictEqual(buf.byteLength, nread);
          strictEqual(buf.buffer.byteLength, 1024);
          received += buf.toString();
        },
      },
    });
    socket.on('data', () => {
      throw new Error("'data' must not fire with onread");
    });
    await once(socket, 'connect');
    // Each write's echo is awaited before the next, so the fills are
    // separate however TCP delivers them.
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    // The caller's buffer was transferred by the first BYOB read.
    strictEqual(buffer.byteLength, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// A fixed buffer that is a view into a larger allocation keeps its offset
// and capacity across fills: every read lands in the caller's range of the
// (transferred) backing store, and the bytes around it are never touched.
export const fixedSubarrayKeepsItsRange = {
  async test(ctrl, env) {
    const backing = new ArrayBuffer(256);
    new Uint8Array(backing).fill(0xaa);
    const view = new Uint8Array(backing, 64, 32);
    let received = '';
    // Observed in the callback, asserted afterwards: a throw from inside the
    // callback destroys the socket with it (callbackThrowDestroysSocket),
    // which would report the failure through 'error' instead of here.
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: view,
        callback(nread, buf) {
          const around = new Uint8Array(buf.buffer);
          fills.push({
            nread,
            byteLength: buf.byteLength,
            byteOffset: buf.byteOffset,
            allocation: buf.buffer.byteLength,
            aroundIntact:
              around.subarray(0, 64).every((b) => b === 0xaa) &&
              around.subarray(96).every((b) => b === 0xaa),
          });
          received += buf.toString();
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    for (const fill of fills) {
      strictEqual(fill.byteLength, fill.nread);
      ok(fill.nread <= 32, `fill of ${fill.nread} exceeds the view's 32 bytes`);
      strictEqual(fill.byteOffset, 64);
      strictEqual(fill.allocation, 256);
      // The allocation outside the view is untouched.
      strictEqual(fill.aroundIntact, true);
    }
    // The caller's view was transferred by the first BYOB read.
    strictEqual(view.byteLength, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// A generator supplies a fresh buffer for every read; each fill is
// delivered in it.
export const generatedBuffersReceiveFills = {
  async test(ctrl, env) {
    const handed = [];
    let received = '';
    const socket = echo(env, {
      onread: {
        buffer() {
          const buffer = new Uint8Array(16);
          handed.push(buffer);
          return buffer;
        },
        callback(nread, buf) {
          received += buf.toString();
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['0123456789abcdef', 'ghij'],
      () => received.length
    );
    strictEqual(received, '0123456789abcdefghij');
    ok(handed.length >= 2);
    socket.end();
    await once(socket, 'close');
  },
};

// A generator handing out the same buffer every time (Node fills it in
// place): the first read transfers and detaches it, and every later read is
// continued over the store it moved to, so every fill arrives, in a Buffer
// over a store of the caller's capacity.
export const sharedGeneratorBufferReceivesEveryFill = {
  async test(ctrl, env) {
    const shared = new Uint8Array(1024);
    let received = '';
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: () => shared,
        callback(nread, buf) {
          fills.push({ nread, capacity: buf.buffer.byteLength });
          received += buf.toString();
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    for (const fill of fills) strictEqual(fill.capacity, 1024);
    strictEqual(shared.byteLength, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// A generator rotating through a pool of buffers, so a fill can be held
// while the next one lands: each buffer is continued over its own store, a
// fill's Buffer stays readable until its buffer is handed out again, and is
// detached by the read that reuses it.
export const rotatingGeneratorBuffersReceiveEveryFill = {
  async test(ctrl, env) {
    const pool = [new Uint8Array(64), new Uint8Array(64)];
    let handed = 0;
    let received = '';
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: () => pool[handed++ % pool.length],
        callback(nread, buf) {
          const text = buf.toString();
          fills.push({
            buf,
            text,
            // The previous fill's Buffer, over the other buffer's store, is
            // still intact when this one is delivered.
            previousIntact:
              fills.length === 0 ||
              fills.at(-1).buf.toString() === fills.at(-1).text,
          });
          received += text;
        },
      },
    });
    await once(socket, 'connect');
    await echoSegments(
      socket,
      ['first', 'second', 'third'],
      () => received.length
    );
    strictEqual(received, 'firstsecondthird');
    ok(fills.length >= 3, `expected three fills or more, got ${fills.length}`);
    for (const fill of fills) strictEqual(fill.previousIntact, true);
    // The third read reused the first fill's buffer, transferring its store:
    // that fill's Buffer is detached; the last fill's is still readable.
    strictEqual(fills[0].buf.byteLength, 0);
    strictEqual(fills.at(-1).buf.toString(), fills.at(-1).text);
    for (const buffer of pool) strictEqual(buffer.byteLength, 0);
    socket.end();
    await once(socket, 'close');
  },
};

// Returning false from the callback stops the read loop (nothing further is
// delivered, over a fixed buffer too); resume() restarts it.
export const callbackFalseStopsReading = {
  async test(ctrl, env) {
    let calls = 0;
    let received = '';
    const socket = echo(env, {
      onread: {
        buffer: Buffer.alloc(64),
        callback(nread, buf) {
          calls++;
          received += buf.toString();
          // Stop after the first fill only.
          return calls === 1 ? false : undefined;
        },
      },
    });
    await once(socket, 'connect');
    // A single byte: the first fill cannot be a part of it, and the loop
    // stops after that fill.
    await echoSegments(socket, ['1'], () => received.length);
    socket.write('two');
    await scheduler.wait(30);
    strictEqual(calls, 1);
    strictEqual(received, '1');
    socket.resume();
    for (let i = 0; received.length < 4; i++) {
      ok(i < 2000, 'the echo never resumed');
      await scheduler.wait(2);
    }
    strictEqual(received, '1two');
    socket.end();
    await once(socket, 'close');
  },
};

// The read loop's failures surface as the socket's 'error' (then 'close'),
// instead of silently ending the loop with the socket open: here, a
// generator that throws — its error is the socket's.
export const generatorThrowDestroysSocket = {
  async test(ctrl, env) {
    const boom = new Error('no buffer for you');
    let calls = 0;
    const socket = echo(env, {
      onread: {
        buffer() {
          if (++calls === 2) throw boom;
          return new Uint8Array(16);
        },
        callback() {},
      },
    });
    await once(socket, 'connect');
    const errored = once(socket, 'error');
    const closed = once(socket, 'close');
    socket.write('x');
    strictEqual(await errored, boom);
    await closed;
    strictEqual(socket.destroyed, true);
    strictEqual(calls, 2);
  },
};

// A callback that throws: the loop destroys the socket with its error, no
// further fill is delivered. (Node lets the throw escape onStreamRead as an
// uncaught exception instead.)
export const callbackThrowDestroysSocket = {
  async test(ctrl, env) {
    const boom = new Error('callback refused the fill');
    let calls = 0;
    const socket = echo(env, {
      onread: {
        buffer: new Uint8Array(16),
        callback() {
          calls++;
          throw boom;
        },
      },
    });
    await once(socket, 'connect');
    const errored = once(socket, 'error');
    const closed = once(socket, 'close');
    socket.write('x');
    strictEqual(await errored, boom);
    await closed;
    strictEqual(socket.destroyed, true);
    strictEqual(socket.errored, boom);
    strictEqual(calls, 1);
  },
};

// The same through streaming mode: a 'data' listener runs inside the loop's
// push() when the socket is flowing with nothing buffered, so its throw is
// the loop's too and destroys the socket with it.
export const dataListenerThrowDestroysSocket = {
  async test(ctrl, env) {
    const boom = new Error('listener refused the chunk');
    let chunks = 0;
    const socket = echo(env);
    socket.on('data', () => {
      chunks++;
      throw boom;
    });
    await once(socket, 'connect');
    const errored = once(socket, 'error');
    const closed = once(socket, 'close');
    socket.write('x');
    strictEqual(await errored, boom);
    await closed;
    strictEqual(socket.destroyed, true);
    strictEqual(socket.errored, boom);
    strictEqual(chunks, 1);
  },
};

// A generator returning anything but a Uint8Array: ERR_INVALID_ARG_TYPE
// (Node keeps reading into the previous buffer, which the transferring
// read here no longer has).
export const generatorGarbageDestroysSocket = {
  async test(ctrl, env) {
    for (const garbage of [undefined, null, 'string', 42, {}, [1, 2]]) {
      let calls = 0;
      const socket = echo(env, {
        onread: {
          buffer() {
            return ++calls === 1 ? new Uint8Array(16) : garbage;
          },
          callback() {},
        },
      });
      await once(socket, 'connect');
      const errored = once(socket, 'error');
      const closed = once(socket, 'close');
      socket.write('x');
      const err = await errored;
      await closed;
      strictEqual(err.code, 'ERR_INVALID_ARG_TYPE', String(garbage));
      strictEqual(socket.destroyed, true);
    }
  },
};

// An empty view — a zero-length one, or the fixed buffer once the callback
// has detached it — cannot be read into: the socket errors with ENOBUFS,
// as Node's read into an empty buffer does.
export const emptyOrDetachedBufferDestroysSocketWithEnobufs = {
  async test(ctrl, env) {
    const empty = echo(env, {
      onread: {
        buffer() {
          return new Uint8Array(0);
        },
        callback() {},
      },
    });
    const emptyErrored = once(empty, 'error');
    const emptyClosed = once(empty, 'close');
    const err = await emptyErrored;
    await emptyClosed;
    strictEqual(err.code, 'ENOBUFS');
    strictEqual(err.syscall, 'read');

    let fills = 0;
    const detaching = echo(env, {
      onread: {
        buffer: Buffer.alloc(64),
        callback(nread, buf) {
          fills++;
          structuredClone(buf.buffer, { transfer: [buf.buffer] });
        },
      },
    });
    await once(detaching, 'connect');
    const errored = once(detaching, 'error');
    const closed = once(detaching, 'close');
    detaching.write('x');
    const detachedErr = await errored;
    await closed;
    strictEqual(detachedErr.code, 'ENOBUFS');
    strictEqual(fills, 1);
    strictEqual(detaching.destroyed, true);
  },
};

// A view over a SharedArrayBuffer cannot be read into (the read transfers
// its buffer): the read's TypeError is the socket's error.
export const sharedOnreadBufferDestroysSocket = {
  async test(ctrl, env) {
    const socket = echo(env, {
      onread: {
        buffer: new Uint8Array(new SharedArrayBuffer(64)),
        callback() {},
      },
    });
    const errored = once(socket, 'error');
    const closed = once(socket, 'close');
    const err = await errored;
    await closed;
    strictEqual(err.name, 'TypeError');
    strictEqual(socket.destroyed, true);
  },
};

// A fixed buffer over a resizable ArrayBuffer: the BYOB read transfers it
// like any other (the caller's buffer is detached, so resizing it throws),
// and the transferred backing store the loop continues over — the one the
// callback's Buffer sits on, until the next read transfers it again —
// keeps the resizability. The loop's view is fixed to the caller's range:
// shrinking the current store below it from the callback leaves the next
// read an empty view, which fails as ENOBUFS.
export const resizableOnreadBufferIsTransferredResizable = {
  async test(ctrl, env) {
    const original = new ArrayBuffer(8, { maxByteLength: 64 });
    const fills = [];
    const socket = echo(env, {
      onread: {
        buffer: new Uint8Array(original),
        callback(nread, buf) {
          fills.push(nread);
          strictEqual(buf.buffer.resizable, true);
          strictEqual(buf.buffer.byteLength, 8);
          throws(() => original.resize(4), { name: 'TypeError' });
          buf.buffer.resize(4);
        },
      },
    });
    await once(socket, 'connect');
    strictEqual(original.detached, true);
    const errored = once(socket, 'error');
    const closed = once(socket, 'close');
    socket.write('abc');
    const err = await errored;
    await closed;
    deepStrictEqual(fills, [3]);
    strictEqual(err.code, 'ENOBUFS');
    strictEqual(socket.destroyed, true);
  },
};
