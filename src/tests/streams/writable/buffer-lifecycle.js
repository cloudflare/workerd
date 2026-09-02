// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Buffer lifecycle for written chunks. A default WritableStream never
// copies, validates, or detaches chunks — BufferSources travel by
// reference like any other value. What the sink OBSERVES for a buffer
// detached, resized, or mutated after write() therefore follows the
// startedness model (see AGENTS.md): the C++ sink already ran inside
// write() and saw the pre-change state; the TypeScript sink runs a
// microtask later and sees the post-change state. Queue accounting is
// immune on both sides: size() is invoked synchronously inside write(),
// so later buffer changes cannot disturb the recorded totals.

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Chunks travel by reference — no snapshot is taken. Whether a same-turn
// mutation after write() returns is visible to the sink follows the
// startedness model.
export const chunkMutationVisibility = {
  async test() {
    let seenValue;
    let seenSameObject;
    const chunk = new Uint8Array([1]);
    const ws = new WritableStream({
      write(v) {
        seenSameObject = v === chunk;
        seenValue = v[0];
      },
    });
    const writer = ws.getWriter();
    const write = writer.write(chunk);
    chunk[0] = 42;
    await write;
    strictEqual(seenSameObject, true);
    strictEqual(seenValue, usingTsImpl ? 42 : 1);
  },
};

// An ALREADY-detached ArrayBuffer is an acceptable chunk: no validation,
// passed through with byteLength 0 (parity).
export const detachedBufferChunkPassesThrough = {
  async test() {
    const ab = new ArrayBuffer(8);
    structuredClone(ab, { transfer: [ab] }); // detach
    let seenByteLength = null;
    const ws = new WritableStream({
      write(v) {
        seenByteLength = v.byteLength;
      },
    });
    const writer = ws.getWriter();
    strictEqual(await writer.write(ab), undefined);
    strictEqual(seenByteLength, 0);
  },
};

// Detaching the chunk's buffer after write() returns, same turn: the
// write fulfills on both sides, but only the TypeScript sink observes
// the detached (zero-length) view.
export const detachAfterWriteTiming = {
  async test() {
    let seen;
    const u8 = new Uint8Array([7]);
    const ws = new WritableStream({
      write(v) {
        seen = { same: v === u8, byteLength: v.byteLength };
      },
    });
    const writer = ws.getWriter();
    const write = writer.write(u8);
    structuredClone(u8.buffer, { transfer: [u8.buffer] }); // detach
    strictEqual(await write, undefined);
    strictEqual(seen.same, true);
    strictEqual(seen.byteLength, usingTsImpl ? 0 : 1);
  },
};

// A length-tracking view over a resizable ArrayBuffer grown after
// write(): the C++ sink saw the original length, the TypeScript sink the
// grown one.
export const resizableGrowAfterWrite = {
  async test() {
    let seen;
    const rab = new ArrayBuffer(2, { maxByteLength: 8 });
    const view = new Uint8Array(rab); // length-tracking
    const ws = new WritableStream({
      write(v) {
        seen = {
          byteLength: v.byteLength,
          bufferByteLength: v.buffer.byteLength,
        };
      },
    });
    const writer = ws.getWriter();
    const write = writer.write(view);
    rab.resize(8);
    strictEqual(await write, undefined);
    strictEqual(seen.byteLength, usingTsImpl ? 8 : 2);
    strictEqual(seen.bufferByteLength, usingTsImpl ? 8 : 2);
  },
};

// A fixed-window view over a resizable ArrayBuffer shrunk out of bounds
// after write(): the write still fulfills everywhere; the C++ sink saw
// the in-bounds view, the TypeScript sink an out-of-bounds one
// (byteLength 0, element reads undefined).
export const resizableShrinkOutOfBounds = {
  async test() {
    let seen;
    const rab = new ArrayBuffer(8, { maxByteLength: 8 });
    const view = new Uint8Array(rab, 4, 4);
    const ws = new WritableStream({
      write(v) {
        seen = { byteLength: v.byteLength, elem: v[0] };
      },
    });
    const writer = ws.getWriter();
    const write = writer.write(view);
    rab.resize(2); // the view's window is now out of bounds
    strictEqual(await write, undefined);
    if (usingTsImpl) {
      strictEqual(seen.byteLength, 0);
      strictEqual(seen.elem, undefined);
    } else {
      strictEqual(seen.byteLength, 4);
      strictEqual(seen.elem, 0);
    }
  },
};

// The user size() runs synchronously inside write(), so the recorded
// queue total is immune to a later detach of the sized chunk (parity).
export const sizeRecordedBeforeDetach = {
  async test() {
    const u8 = new Uint8Array(4);
    const ws = new WritableStream(
      {
        async write() {
          await scheduler.wait(10);
        },
      },
      {
        size(chunk) {
          return chunk.byteLength;
        },
        highWaterMark: 8,
      }
    );
    const writer = ws.getWriter();
    const write = writer.write(u8); // size 4 recorded now
    strictEqual(writer.desiredSize, 4);
    structuredClone(u8.buffer, { transfer: [u8.buffer] }); // byteLength now 0
    strictEqual(writer.desiredSize, 4, 'recorded total unaffected by detach');
    await write;
    strictEqual(writer.desiredSize, 8, 'full recovery after drain');
    ok(u8.byteLength === 0, 'chunk really was detached');
  },
};
