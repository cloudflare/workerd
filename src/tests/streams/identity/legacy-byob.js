// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pre-flag BYOB semantics, as seen by workers predating
// streams_byob_reader_detaches_buffer (2021-11-10) and
// internal_stream_byob_return_view (2024-05-13): the caller's buffer is NOT
// detached — the fill happens in place and the result view aliases the very
// same buffer — and a BYOB read at EOF resolves with value undefined rather
// than a zero-length view. Legacy suite: C++ implementation only; see
// identity-cpp-legacy.wd-test.

import { ok, strictEqual, deepStrictEqual } from 'node:assert';

// The mid-read destination-mutation tests below exist because the
// non-detached buffer remains in the caller's hands while the read is
// parked; they guard against V8 asserts and out-of-bounds writes, not just
// data correctness.

export const legacyByobFillsInPlace = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader({ mode: 'byob' });
    const writePromise = writer.write(new Uint8Array([1, 2, 3, 4, 5]));

    const backing = new ArrayBuffer(8);
    new Uint8Array(backing).fill(0xee);
    const view = new Uint8Array(backing, 4, 3);

    const { value, done } = await reader.read(view);
    strictEqual(done, false);
    // The result aliases the caller's buffer, which was not detached.
    strictEqual(value.buffer, backing);
    strictEqual(backing.byteLength, 8);
    strictEqual(view.byteLength, 3, 'input view must remain usable');
    strictEqual(value.byteOffset, 4);
    strictEqual(value.byteLength, 3);
    deepStrictEqual([...value], [1, 2, 3]);
    // The fill landed in place, inside the view's region only.
    deepStrictEqual(
      [...new Uint8Array(backing)],
      [0xee, 0xee, 0xee, 0xee, 1, 2, 3, 0xee]
    );

    const rest = await reader.read(new Uint8Array(4));
    deepStrictEqual([...rest.value], [4, 5]);
    await writePromise;
    await writer.close();
  },
};

export const legacyByobDestinationTransferredMidRead = {
  async test() {
    // With the buffer not detached at read() time, the caller can still
    // transfer it while the read is parked. The read must not write into
    // the transferred backing store: it resolves with a zero-length view
    // over a fresh empty buffer, and the data that would have filled it is
    // lost.
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();
    const buffer = new ArrayBuffer(4096);
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.transferToFixedLength(64);
    await writer.write(new Uint8Array(1337));
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 0);
    strictEqual(result.value.buffer.byteLength, 0);
  },
};

export const legacyByobDestinationShrunkMidRead = {
  async test() {
    // Shrinking a resizable destination while the read is parked truncates
    // the delivery to the buffer's size at completion time.
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();
    const buffer = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.resize(64);
    const writeData = new Uint8Array(1337);
    for (let i = 0; i < writeData.length; i++) writeData[i] = i % 256;
    await writer.write(writeData);
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 64);
    strictEqual(result.value.buffer.byteLength, 64);
    deepStrictEqual(
      Array.from(result.value),
      Array.from(writeData.slice(0, 64))
    );
  },
};

export const legacyByobDestinationGrownMidRead = {
  async test() {
    // Growing a resizable destination while the read is parked delivers
    // normally into the grown buffer, with the tail untouched (zeroed).
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();
    const buffer = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.resize(8192);
    const writeData = new Uint8Array(100);
    for (let i = 0; i < writeData.length; i++) writeData[i] = i;
    await writer.write(writeData);
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 100);
    strictEqual(result.value.buffer.byteLength, 8192);
    deepStrictEqual(Array.from(result.value), Array.from(writeData));
    const fullBuffer = new Uint8Array(result.value.buffer);
    for (let i = 100; i < fullBuffer.byteLength; i++) {
      strictEqual(fullBuffer[i], 0, `byte ${i} must be untouched`);
    }
  },
};

export const legacyByobEofReturnsUndefined = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await writer.close();
    const reader = readable.getReader({ mode: 'byob' });
    const backing = new ArrayBuffer(10);
    const result = await reader.read(new Uint8Array(backing));
    strictEqual(result.done, true);
    strictEqual(result.value, undefined);
    // The caller keeps a usable reference to the buffer either way.
    ok(backing.byteLength === 10);
  },
};
