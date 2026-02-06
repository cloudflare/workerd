// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

// This is testing non-standard behavior that we support for historical reasons. While the spec
// dictates that BYOB buffers should be detached when passed to a pending read, historically we've
// not done so (there's a compat flag to enable that behavior). This means the read/write buffers
// can be modified after being passed in, including being detached or resized. When a buffer is
// detached or resized, we need to ensure that the BYOB read still resolves correctly and doesn't
// end with a v8 assert or produce invalid data.

// Test that transferring the buffer during a pending BYOB read resolves with a zero-length result
export const transferBuffer = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(4096);
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.transferToFixedLength(64);

    await writer.write(new Uint8Array(1337));

    // When the buffer is detached, the read resolves with a new zero-length ArrayBuffer
    // (the data that was read is lost)
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 0);
    // The buffer should be a new ArrayBuffer, not the original detached one
    strictEqual(result.value.buffer.byteLength, 0);
  },
};

// Test that resizing a resizable buffer smaller during a pending BYOB read returns truncated data
export const resizeBufferSmaller = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.resize(64);

    // Write data that would exceed the resized buffer
    const writeData = new Uint8Array(1337);
    for (let i = 0; i < writeData.length; i++) {
      writeData[i] = i % 256;
    }
    await writer.write(writeData);

    // When the buffer is resized smaller, we get truncated data
    // (only the bytes that fit in the new size)
    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 64);
    // The underlying ArrayBuffer should be the resized buffer
    strictEqual(result.value.buffer.byteLength, 64);
    // Verify the truncated data matches the first 64 bytes written
    deepStrictEqual(
      Array.from(result.value),
      Array.from(writeData.slice(0, 64))
    );
  },
};

// Test that resizing a resizable buffer larger during a pending BYOB read works correctly
export const resizeBufferLarger = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(4096, { maxByteLength: 8192 });
    const readPromise = reader.read(new Uint8Array(buffer));
    buffer.resize(8192);

    // Write some data
    const writeData = new Uint8Array(100);
    for (let i = 0; i < writeData.length; i++) {
      writeData[i] = i;
    }
    await writer.write(writeData);

    const result = await readPromise;
    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 100);
    // The underlying ArrayBuffer should be the resized buffer
    strictEqual(result.value.buffer.byteLength, 8192);

    // Verify the data matches what was written
    deepStrictEqual(Array.from(result.value), Array.from(writeData));

    // Verify the remaining bytes in the underlying ArrayBuffer are zeroes
    const fullBuffer = new Uint8Array(result.value.buffer);
    strictEqual(fullBuffer.byteLength, 8192);
    for (let i = 100; i < fullBuffer.byteLength; i++) {
      strictEqual(
        fullBuffer[i],
        0,
        `Expected byte at index ${i} to be 0, got ${fullBuffer[i]}`
      );
    }
  },
};

// Test that resizing a write buffer smaller after queuing doesn't produce invalid data
export const writeBufferResizedSmaller = {
  async test() {
    const { writable, readable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const buf1 = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const buf2 = new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }));
    buf2.set([11, 12, 13, 14, 15, 16, 17, 18]);

    const p1 = writer.write(buf1);
    const p2 = writer.write(buf2);
    // Resize smaller after queuing. The write captures the BackingStore at
    // time of write, so the view is now out-of-bounds and should be treated as zeroes beyond
    // the resized length
    buf2.buffer.resize(6);

    const r1 = await reader.read();
    const r2 = await reader.read();

    await p1;
    await p2;

    // First write should be unaffected
    strictEqual(r1.done, false);
    deepStrictEqual(Array.from(r1.value), [1, 2, 3, 4, 5, 6, 7, 8]);

    // Second write - uses original view size but bytes beyond the resized buffer are zeroed
    strictEqual(r2.done, false);
    deepStrictEqual(Array.from(r2.value), [11, 12, 13, 14, 15, 16, 0, 0]);
  },
};

// Test that resizing a write buffer larger after queuing doesn't produce invalid data
export const writeBufferResizedLarger = {
  async test() {
    const { writable, readable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const buf1 = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const buf2 = new Uint8Array(new ArrayBuffer(8, { maxByteLength: 16 }));
    buf2.set([11, 12, 13, 14, 15, 16, 17, 18]);

    const p1 = writer.write(buf1);
    const p2 = writer.write(buf2);
    buf2.buffer.resize(12); // Resize larger after queuing

    const r1 = await reader.read();
    const r2 = await reader.read();

    await p1;
    await p2;

    // First write should be unaffected
    strictEqual(r1.done, false);
    deepStrictEqual(Array.from(r1.value), [1, 2, 3, 4, 5, 6, 7, 8]);

    // Second write - should contain original 8 bytes (view size at time of write)
    strictEqual(r2.done, false);
    deepStrictEqual(Array.from(r2.value), [11, 12, 13, 14, 15, 16, 17, 18]);
  },
};

// Test that transferring a write buffer after queuing still writes correctly (BackingStore
// is captured at time of write, so transfer doesn't affect the queued write)
export const writeBufferTransferred = {
  async test() {
    const { writable, readable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();

    const buf1 = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const buf2 = new Uint8Array([9, 10, 11, 12, 13, 14, 15, 16]);
    const buf3 = new Uint8Array([17, 18, 19, 20, 21, 22, 23, 24]);
    const buf4 = new Uint8Array([25, 26, 27, 28, 29, 30, 31, 32]);

    // Do not await the writes so they are queued up before the transfer
    writer.write(buf1);
    writer.write(buf2);
    writer.write(buf3);
    writer.write(buf4);

    buf3.buffer.transfer();

    const r1 = await reader.read();
    const r2 = await reader.read();
    const r3 = await reader.read();
    const r4 = await reader.read();

    // All reads should produce the original data since the transferred buffer's data is
    // captured at time of write
    strictEqual(r1.done, false);
    deepStrictEqual(Array.from(r1.value), [1, 2, 3, 4, 5, 6, 7, 8]);
    strictEqual(r2.done, false);
    deepStrictEqual(Array.from(r2.value), [9, 10, 11, 12, 13, 14, 15, 16]);
    strictEqual(r3.done, false);
    deepStrictEqual(Array.from(r3.value), [17, 18, 19, 20, 21, 22, 23, 24]);
    strictEqual(r4.done, false);
    deepStrictEqual(Array.from(r4.value), [25, 26, 27, 28, 29, 30, 31, 32]);
  },
};
