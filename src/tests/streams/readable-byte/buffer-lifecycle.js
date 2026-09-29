// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Buffer ownership hazards at every stage of the BYOB cycle: detached
// buffers, foreign buffers in respondWithNewView, resizable
// ArrayBuffers, and non-detachable (WebAssembly.Memory, SharedArrayBuffer)
// buffers. The BEHAVIOR is parity — only messages differ (the WPT
// bad-buffers-and-views and non-transferable-buffers families) — except
// for resizable buffers (ledger #30): TS transfers them to fixed length
// (spec TransferArrayBuffer), while C++ can hand the source a resizable
// byobRequest buffer and return resizable results.

import { deepStrictEqual, strictEqual, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function byteStream(source = {}) {
  let controller;
  const rs = new ReadableStream({
    type: 'bytes',
    start(c) {
      controller = c;
    },
    ...source,
  });
  return { rs, controller: () => controller };
}

// enqueue() of a view over a detached buffer throws TypeError.
export const enqueueDetachedBuffer = {
  test() {
    const { controller } = byteStream();
    const buf = new ArrayBuffer(4);
    const view = new Uint8Array(buf);
    buf.transfer();
    throws(() => controller().enqueue(view), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'chunk must have a non-zero byteLength'
        : 'Cannot enqueue a zero-length ArrayBuffer.',
    });
  },
};

// read(view) with a detached view rejects TypeError.
export const readDetachedView = {
  async test() {
    const { rs } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const buf = new ArrayBuffer(4);
    const view = new Uint8Array(buf);
    buf.transfer();
    await rejects(reader.read(view), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'view must have a non-zero byteLength'
        : /positive-sized TypedArray/,
    });
  },
};

// Detaching the byobRequest's own view then calling respond() throws
// TypeError on both sides; the outstanding read stays pending (bounded)
// and is cleaned up via cancel.
export const respondAfterViewDetached = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(4));
    await scheduler.wait(5);
    const req = controller().byobRequest;
    req.view.buffer.transfer();
    throws(() => req.respond(2), {
      name: 'TypeError',
      message: usingTsImpl
        ? /detached ArrayBuffer/
        : 'Cannot respond with a zero-length or detached view',
    });
    const outcome = await Promise.race([
      read.then(() => 'settled'),
      scheduler.wait(50).then(() => 'pending'),
    ]);
    strictEqual(outcome, 'pending');
    await reader.cancel('cleanup');
  },
};

// respondWithNewView() requires a view over the SAME buffer region:
// a foreign buffer throws RangeError on both sides.
export const respondWithNewViewForeignBuffer = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const read = reader.read(new Uint8Array(4));
    await scheduler.wait(5);
    const req = controller().byobRequest;
    throws(() => req.respondWithNewView(new Uint8Array([9, 9])), {
      name: 'RangeError',
      message: usingTsImpl
        ? /same byteLength as the request/
        : 'The underlying ArrayBuffer is not the correct length.',
    });
    const outcome = await Promise.race([
      read.then(() => 'settled'),
      scheduler.wait(50).then(() => 'pending'),
    ]);
    strictEqual(outcome, 'pending');
    await reader.cancel('cleanup');
  },
};

// A view over a RESIZABLE ArrayBuffer can be enqueued; the enqueue
// detaches the buffer (so resize() then throws TypeError) and the bytes
// are delivered (parity).
export const enqueueResizableBuffer = {
  async test() {
    const { rs, controller } = byteStream();
    const rab = new ArrayBuffer(4, { maxByteLength: 16 });
    const view = new Uint8Array(rab, 0, 2);
    view[0] = 1;
    view[1] = 2;
    controller().enqueue(view);
    throws(() => rab.resize(8), TypeError); // detached by enqueue
    const { value, done } = await rs.getReader().read();
    strictEqual(done, false);
    strictEqual(value.byteLength, 2);
    strictEqual(value[1], 2);
    strictEqual(value.buffer.resizable, false);
  },
};

// read(view) with a view over a resizable buffer works on both sides.
export const readResizableView = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    const rab = new ArrayBuffer(4, { maxByteLength: 16 });
    const read = reader.read(new Uint8Array(rab));
    await scheduler.wait(5);
    const req = controller().byobRequest;
    if (req !== null) {
      req.view[0] = 3;
      req.respond(1);
    } else {
      controller().enqueue(new Uint8Array([3]));
    }
    const { value, done } = await read;
    strictEqual(done, false);
    strictEqual(value.byteLength, 1);
    strictEqual(value[0], 3);
    strictEqual(value.buffer.resizable, !usingTsImpl);
  },
};

// A read(view) over a resizable buffer: TS exposes a fixed-length
// byobRequest buffer, so the source cannot shrink it and respond() works;
// C++ lets it shrink and respond() then throws (ledger #30).
export const resizableByobRequestCannotShrink = {
  async test() {
    for (const release of [false, true]) {
      const { rs, controller } = byteStream();
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.read(
        new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }))
      );
      read.catch(() => {});
      await scheduler.wait(5);
      const req = controller().byobRequest;
      strictEqual(req.view.buffer.resizable, !usingTsImpl);
      if (release) reader.releaseLock();
      if (usingTsImpl) {
        throws(() => req.view.buffer.resize(0), TypeError);
        req.view.set([1, 2, 3, 4]);
        req.respond(4);
        const { value, done } = release
          ? await rs.getReader().read()
          : await read;
        strictEqual(done, false);
        deepStrictEqual([...value], [1, 2, 3, 4]);
        strictEqual(value.buffer.resizable, false);
      } else {
        req.view.buffer.resize(0);
        throws(() => req.respond(4), {
          name: 'TypeError',
          message: 'Cannot respond with a zero-length or detached view',
        });
        if (!release) {
          const outcome = await Promise.race([
            read.then(() => 'settled'),
            scheduler.wait(50).then(() => 'pending'),
          ]);
          strictEqual(outcome, 'pending');
        }
        await rs.cancel('cleanup').catch(() => {});
        await reader.cancel('cleanup').catch(() => {});
      }
    }
  },
};

// Results built over a resizable buffer handed in by respondWithNewView()
// or by read(view) on a closed stream are fixed-length on TS only
// (ledger #30).
export const resizableBuffersDeliveredFixedLength = {
  async test() {
    {
      const { rs, controller } = byteStream();
      const reader = rs.getReader({ mode: 'byob' });
      const read = reader.read(new Uint8Array(4));
      await scheduler.wait(5);
      const rab = new ArrayBuffer(4, { maxByteLength: 16 });
      new Uint8Array(rab).set([1, 2, 3, 4]);
      controller().byobRequest.respondWithNewView(new Uint8Array(rab));
      const { value } = await read;
      deepStrictEqual([...value], [1, 2, 3, 4]);
      strictEqual(value.buffer.resizable, !usingTsImpl);
    }
    {
      const { rs, controller } = byteStream();
      controller().close();
      const { value, done } = await rs
        .getReader({ mode: 'byob' })
        .read(new Uint8Array(new ArrayBuffer(4, { maxByteLength: 16 })));
      strictEqual(done, true);
      strictEqual(value.byteLength, 0);
      strictEqual(value.buffer.resizable, !usingTsImpl);
    }
  },
};

// SharedArrayBuffer-backed views are rejected with TypeError by
// read(view), enqueue() and respondWithNewView().
export const sharedBuffersRejected = {
  async test() {
    const { rs, controller } = byteStream();
    const reader = rs.getReader({ mode: 'byob' });
    await rejects(reader.read(new Uint8Array(new SharedArrayBuffer(4))), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'view must not be backed by a SharedArrayBuffer'
        : 'Unabled to use non-detachable ArrayBuffer.',
    });
    throws(
      () => controller().enqueue(new Uint8Array(new SharedArrayBuffer(4))),
      {
        name: 'TypeError',
        message: usingTsImpl
          ? 'chunk must not be backed by a SharedArrayBuffer'
          : 'The provided ArrayBuffer must be detachable.',
      }
    );
    const read = reader.read(new Uint8Array(4));
    await scheduler.wait(5);
    throws(
      () =>
        controller().byobRequest.respondWithNewView(
          new Uint8Array(new SharedArrayBuffer(4))
        ),
      {
        name: 'TypeError',
        message: usingTsImpl
          ? 'view must not be backed by a SharedArrayBuffer'
          : 'Unable to use non-detachable ArrayBuffer.',
      }
    );
    controller().enqueue(new Uint8Array([5]));
    deepStrictEqual([...(await read).value], [5]);
  },
};

// Non-detachable buffers (WebAssembly.Memory) are rejected with
// TypeError from both enqueue() and read(view).
export const nonDetachableBuffersRejected = {
  async test() {
    const { controller } = byteStream();
    const mem = new WebAssembly.Memory({ initial: 1 });
    const view = new Uint8Array(mem.buffer, 0, 4);
    throws(() => controller().enqueue(view), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'ArrayBuffer is not detachable and could not be cloned.'
        : 'The provided ArrayBuffer must be detachable.',
    });
    const { rs: rs2 } = byteStream();
    await rejects(
      rs2.getReader({ mode: 'byob' }).read(new Uint8Array(mem.buffer, 0, 4)),
      {
        name: 'TypeError',
        message: usingTsImpl
          ? 'ArrayBuffer is not detachable and could not be cloned.'
          : 'Unabled to use non-detachable ArrayBuffer.',
      }
    );
  },
};
