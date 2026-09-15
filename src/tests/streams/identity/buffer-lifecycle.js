// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Buffer lifecycle hazards around write(): the bytes delivered to the reader
// are a snapshot taken synchronously inside write(), unaffected by the
// caller resizing, detaching, or otherwise mutating the buffer afterward.
// Both implementations document and honor this: internal.c++ copies in
// processChunk "because ArrayBuffers might not be detached by the Writer,
// or might be detached after being written, or might be resizable and
// resized after being written", and identity.ts snapshots in its strategy
// size callback — the one hook the writable machinery runs synchronously
// inside write() — for the same reasons.
//
// Degenerate inputs that are already unusable at write() time (detached
// buffer, view made out-of-bounds by a shrink) are a separate matter and
// partly diverge; see the individual tests at the bottom.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Writes chunk, synchronously runs mutate (before any await, exactly as a
// misbehaving caller would), and asserts the reader receives expectedBytes.
// A close is queued behind the write so a dropped chunk surfaces as a clean
// done-too-early failure rather than a hang.
async function expectDelivery(chunk, mutate, expectedBytes) {
  const { readable, writable } = new IdentityTransformStream();
  const writer = writable.getWriter();
  const reader = readable.getReader();
  const writePromise = writer.write(chunk);
  mutate();
  const closePromise = writer.close();
  const { value, done } = await reader.read();
  strictEqual(done, false, 'chunk was dropped');
  deepStrictEqual([...value], expectedBytes);
  await writePromise;
  const tail = await reader.read();
  strictEqual(tail.done, true);
  await closePromise;
}

export const resizableShrinkAfterWrite = {
  async test() {
    const rab = new ArrayBuffer(8, { maxByteLength: 16 });
    const view = new Uint8Array(rab);
    view.set([1, 2, 3, 4, 5, 6, 7, 8]);
    await expectDelivery(view, () => rab.resize(2), [1, 2, 3, 4, 5, 6, 7, 8]);
  },
};

export const resizableGrowAfterWrite = {
  async test() {
    // The view is length-tracking, so after the grow it spans 16 bytes; the
    // delivered chunk is still the 4 bytes captured at write().
    const rab = new ArrayBuffer(4, { maxByteLength: 16 });
    const view = new Uint8Array(rab);
    view.set([1, 2, 3, 4]);
    await expectDelivery(view, () => rab.resize(16), [1, 2, 3, 4]);
  },
};

export const resizableBufferShrinkToZeroAfterWrite = {
  async test() {
    // The ArrayBuffer itself as the chunk, shrunk to nothing after write.
    const rab = new ArrayBuffer(4, { maxByteLength: 8 });
    new Uint8Array(rab).set([9, 8, 7, 6]);
    await expectDelivery(rab, () => rab.resize(0), [9, 8, 7, 6]);
  },
};

export const viewDetachedAfterWrite = {
  async test() {
    const ab = new ArrayBuffer(4);
    const view = new Uint8Array(ab);
    view.set([1, 2, 3, 4]);
    await expectDelivery(view, () => ab.transfer(), [1, 2, 3, 4]);
  },
};

export const bufferDetachedAfterWrite = {
  async test() {
    const ab = new ArrayBuffer(4);
    new Uint8Array(ab).set([5, 6, 7, 8]);
    await expectDelivery(ab, () => ab.transfer(), [5, 6, 7, 8]);
  },
};

export const alreadyDetachedBufferAtWrite = {
  async test() {
    // Writing a buffer that is already detached diverges:
    // - C++ observes byteLength 0 and treats it as a zero-length no-op.
    // - TypeScript rejects the write with TypeError (the chunk cannot be
    //   read at all); like any other invalid chunk the rejection is
    //   per-write — the stream survives it.
    const ab = new ArrayBuffer(4);
    ab.transfer();
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    if (usingTsImpl) {
      await rejects(writer.write(ab), TypeError);
      // The stream is usable after the rejection.
      const writePromise = writer.write(new Uint8Array([9]));
      const { value, done } = await reader.read();
      strictEqual(done, false);
      strictEqual(value[0], 9);
      await writePromise;
      await writer.close();
    } else {
      await writer.write(ab);
      const closePromise = writer.close();
      strictEqual((await reader.read()).done, true);
      await closePromise;
    }
  },
};

export const alreadyDetachedViewAtWrite = {
  async test() {
    // A view over an already-detached buffer reports byteLength 0, so both
    // implementations treat it as a zero-length no-op.
    const ab = new ArrayBuffer(4);
    const view = new Uint8Array(ab);
    ab.transfer();
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write(view);
    const closePromise = writer.close();
    strictEqual((await reader.read()).done, true);
    await closePromise;
  },
};

export const outOfBoundsViewAtWrite = {
  async test() {
    // A fixed-length view made out-of-bounds by a shrink reports
    // byteLength 0, so both implementations treat it as a zero-length
    // no-op.
    const rab = new ArrayBuffer(8, { maxByteLength: 8 });
    const view = new Uint8Array(rab, 4, 4);
    rab.resize(2);
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write(view);
    const closePromise = writer.close();
    strictEqual((await reader.read()).done, true);
    await closePromise;
  },
};

export const lyingAboutLength = {
  async test() {
    // Own properties that lie about a view's length must be ignored: both
    // implementations read buffer metadata from the internal slots (C++ via
    // the V8 API, TypeScript via captured primordial getters), so a
    // shadowing byteLength getter cannot change what gets written.
    {
      // An empty view claiming to be huge is still a zero-length no-op.
      const u8 = new Uint8Array();
      Object.defineProperty(u8, 'byteLength', {
        get() {
          return 10_000;
        },
      });
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader();
      await writer.write(u8);
      const closePromise = writer.close();
      strictEqual((await reader.read()).done, true);
      await closePromise;
    }
    // A non-empty view claiming a different size still delivers exactly
    // its real bytes, whichever direction the lie goes. The larger lie is
    // the important one: an implementation that trusted it would read past
    // the end of the backing buffer.
    for (const lie of [2, 10_000]) {
      const u8 = new Uint8Array([1, 2, 3, 4]);
      Object.defineProperty(u8, 'byteLength', {
        get() {
          return lie;
        },
      });
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader();
      const writePromise = writer.write(u8);
      const { value, done } = await reader.read();
      strictEqual(done, false, `byteLength lie ${lie}: chunk was dropped`);
      deepStrictEqual(
        [...value],
        [1, 2, 3, 4],
        `byteLength lie ${lie}: wrong bytes delivered`
      );
      await writePromise;
      await writer.close();
    }

    // Attempting to change the length getter after the write is ignored.
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromise = writer.write(u8);
    Object.defineProperty(u8.buffer, 'byteLength', {
      get() {
        throw new Error('should not be called');
      },
    });
    Object.defineProperty(u8, 'byteLength', {
      get() {
        throw new Error('should not be called');
      },
    });
    Object.defineProperty(u8.buffer, 'byteOffset', {
      get() {
        throw new Error('should not be called');
      },
    });
    const { value, done } = await reader.read();
    strictEqual(done, false, `byteLength chunk was dropped`);
    deepStrictEqual([...value], [1, 2, 3, 4], `byteLength getter called`);
    await writePromise;
    await writer.close();
  },
};
