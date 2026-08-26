// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// BYOB read fundamentals on the readable side of an IdentityTransformStream.
// Unlike a standard TransformStream, the identity readable supports BYOB.
//
// These tests assume the modern BYOB semantics pinned by the
// streams_byob_reader_detaches_buffer and internal_stream_byob_return_view
// compatibility flags, which both test configs set explicitly.

import { ok, strictEqual, deepStrictEqual } from 'node:assert';

export const supportsByobReader = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader({ mode: 'byob' });
    const writePromise = writer.write(new Uint8Array([10, 20, 30]));
    const { value, done } = await reader.read(new Uint8Array(10));
    strictEqual(done, false);
    ok(value instanceof Uint8Array);
    strictEqual(value.byteLength, 3);
    deepStrictEqual([...value], [10, 20, 30]);
    await writePromise;
    await writer.close();
  },
};

export const partialFillAcrossReads = {
  async test() {
    // A write larger than the destination view is delivered across multiple
    // reads, and the write only resolves once fully consumed.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader({ mode: 'byob' });

    let writeResolved = false;
    const writePromise = writer
      .write(new Uint8Array([1, 2, 3, 4, 5]))
      .then(() => (writeResolved = true));

    const first = await reader.read(new Uint8Array(3));
    strictEqual(first.done, false);
    deepStrictEqual([...first.value], [1, 2, 3]);
    // Only 3 of 5 bytes consumed: the write must still be pending.
    strictEqual(writeResolved, false);

    const second = await reader.read(new Uint8Array(3));
    strictEqual(second.done, false);
    deepStrictEqual([...second.value], [4, 5]);

    await writePromise;
    strictEqual(writeResolved, true);
    await writer.close();
  },
};

export const byobViewLyingAboutLength = {
  async test() {
    // The destination view's own properties must not be trusted: the fill
    // must be bounded by the view's REAL extent (from the internal slots),
    // so a shadowing byteLength getter can neither under-fill the view nor
    // — the dangerous direction — cause a write past its end. Sentinel
    // bytes around the view's real region make an out-of-bounds write
    // visible.
    for (const lie of [1, 10_000]) {
      const { readable, writable } = new IdentityTransformStream();
      const writer = writable.getWriter();
      const reader = readable.getReader({ mode: 'byob' });
      const writePromise = writer.write(new Uint8Array([1, 2, 3, 4, 5]));

      const backing = new ArrayBuffer(8);
      new Uint8Array(backing).fill(0xee);
      const view = new Uint8Array(backing, 4, 3);
      Object.defineProperty(view, 'byteLength', {
        get() {
          return lie;
        },
      });

      const { value, done } = await reader.read(view);
      strictEqual(done, false, `byteLength lie ${lie}: read was done`);
      strictEqual(value.byteOffset, 4, `byteLength lie ${lie}: wrong offset`);
      strictEqual(value.byteLength, 3, `byteLength lie ${lie}: wrong fill`);
      deepStrictEqual([...value], [1, 2, 3]);
      // The rest of the (transferred) backing buffer must be untouched;
      // the sentinel beyond the view's real end is the overwrite guard.
      const full = new Uint8Array(value.buffer);
      deepStrictEqual(
        [...full],
        [0xee, 0xee, 0xee, 0xee, 1, 2, 3, 0xee],
        `byteLength lie ${lie}: bytes outside the view were touched`
      );

      // The unread remainder is delivered to the next (honest) read, and
      // only then does the write complete.
      const rest = await reader.read(new Uint8Array(4));
      deepStrictEqual([...rest.value], [4, 5]);
      await writePromise;
      await writer.close();
    }
  },
};

export const byobViewLyingAfterEnqueue = {
  async test() {
    // Once a view has been enqueued as the pending BYOB request (its buffer
    // transferred per streams_byob_reader_detaches_buffer), the
    // implementation must never consult the view object again: the extent
    // was captured at enqueue. Throwing getters installed after the read()
    // call catch any later user-visible metadata access on the fill or
    // fulfillment path.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader({ mode: 'byob' });

    const backing = new ArrayBuffer(8);
    new Uint8Array(backing).fill(0xee);
    const view = new Uint8Array(backing, 4, 3);

    // Park the read first: the view is now the pending BYOB request.
    const readPromise = reader.read(view);

    const boom = () => {
      throw new Error('should not be called');
    };
    Object.defineProperty(view, 'byteLength', { get: boom });
    Object.defineProperty(view, 'byteOffset', { get: boom });
    Object.defineProperty(view, 'buffer', { get: boom });
    Object.defineProperty(view, 'constructor', { get: boom });
    Object.defineProperty(backing, 'byteLength', { get: boom });

    // Only now deliver the data that fulfills the parked request.
    const writePromise = writer.write(new Uint8Array([1, 2, 3, 4, 5]));

    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(value.byteOffset, 4);
    strictEqual(value.byteLength, 3);
    deepStrictEqual([...value], [1, 2, 3]);
    // Sentinels around the view's real region are untouched.
    const full = new Uint8Array(value.buffer);
    deepStrictEqual([...full], [0xee, 0xee, 0xee, 0xee, 1, 2, 3, 0xee]);

    // The unread remainder and write completion are unaffected.
    const rest = await reader.read(new Uint8Array(4));
    deepStrictEqual([...rest.value], [4, 5]);
    await writePromise;
    await writer.close();
  },
};

export const eofReturnsZeroLengthView = {
  async test() {
    // After close, a BYOB read resolves done with a zero-length view whose
    // buffer is the (transferred) buffer that was passed in, preserving its
    // byteLength so callers can reuse it.
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.close();
    const reader = readable.getReader({ mode: 'byob' });
    const result = await reader.read(new Uint8Array(10));
    strictEqual(result.done, true);
    ok(result.value instanceof Uint8Array);
    strictEqual(result.value.byteLength, 0);
    strictEqual(result.value.buffer.byteLength, 10);
  },
};
