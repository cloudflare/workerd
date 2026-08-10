// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual, ok, rejects } from 'node:assert';

// A negative minElements reaches the controller sign-extended to a huge size_t. It must be
// rejected on the element count, before it is scaled by the element size.
export const ByobReaderReadAtLeastNegative = {
  async test() {
    for (const bad of [-1, -2147483648]) {
      const ts = new IdentityTransformStream();
      const reader = ts.readable.getReader({ mode: 'byob' });
      await rejects(
        async () => reader.readAtLeast(bad, new Uint8Array(64)),
        TypeError
      );
      reader.releaseLock();
    }
  },
};

// minElements is a C++ int, so jsg rejects anything outside its range at the argument boundary.
// 2**31 is the first such value. This is what currently keeps a huge element count from ever
// reaching the scaling by element size, where 2**61 * 8 would wrap to 0.
export const ByobReaderReadAtLeastOutOfIntRange = {
  async test() {
    for (const bad of [2 ** 31, 2 ** 61, Number.MAX_SAFE_INTEGER]) {
      const ts = new IdentityTransformStream();
      const reader = ts.readable.getReader({ mode: 'byob' });
      await rejects(
        async () => reader.readAtLeast(bad, new Float64Array(8)),
        TypeError
      );
      reader.releaseLock();
    }
  },
};

// The largest in-range minElements is accepted by jsg and must then be rejected for exceeding the
// buffer, on the element count rather than on a scaled byte count. This pins the seam at 2**31.
export const ByobReaderReadAtLeastIntMax = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    await rejects(
      async () => reader.readAtLeast(2 ** 31 - 1, new Float64Array(8)),
      TypeError
    );
    reader.releaseLock();
  },
};

export const ByobReaderResizePendingRead = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(8192, { maxByteLength: 16384 });
    const view = new Uint8Array(buffer, 4096, 1024);

    const readPromise = reader.read(view);
    buffer.resize(2048);

    await writer.write(new Uint8Array(100).fill(0x41));
    const result = await readPromise;

    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 0);

    reader.releaseLock();
    writer.releaseLock();
  },
};

// Shrink the buffer so that the read destination is partially, but not entirely, cut off. The
// delivered length must come from the buffer's size at completion time, not the size that was
// captured when the read was issued.
export const ByobReaderResizePendingReadPartialTruncation = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(8192, { maxByteLength: 16384 });
    const view = new Uint8Array(buffer, 1024, 4096);

    const readPromise = reader.read(view);
    // Leaves 512 bytes of room at byteOffset 1024, less than the 1000 bytes written below.
    buffer.resize(1536);

    await writer.write(new Uint8Array(1000).fill(0x42));
    const result = await readPromise;

    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 512);
    strictEqual(result.value.byteOffset, 1024);
    strictEqual(result.value.buffer.byteLength, 1536);
    // Every delivered byte must be real data, and nothing may be written past the new end.
    for (let i = 0; i < result.value.byteLength; i++) {
      strictEqual(result.value[i], 0x42);
    }

    reader.releaseLock();
    writer.releaseLock();
  },
};

// Detaching the destination mid-read discards the data rather than writing into the transferred
// backing store.
export const ByobReaderDetachPendingRead = {
  async test() {
    const ts = new IdentityTransformStream();
    const reader = ts.readable.getReader({ mode: 'byob' });
    const writer = ts.writable.getWriter();

    const buffer = new ArrayBuffer(8192);
    const view = new Uint8Array(buffer, 1024, 4096);

    const readPromise = reader.read(view);
    // Transfer detaches the original buffer.
    const transferred = structuredClone(buffer, { transfer: [buffer] });

    await writer.write(new Uint8Array(1000).fill(0x43));
    const result = await readPromise;

    strictEqual(result.done, false);
    strictEqual(result.value.byteLength, 0);
    strictEqual(buffer.byteLength, 0);
    // The transferred buffer must not have received a late write from the pending read.
    const check = new Uint8Array(transferred);
    for (let i = 0; i < check.length; i++) {
      strictEqual(check[i], 0);
    }

    reader.releaseLock();
    writer.releaseLock();
  },
};

export const ByobReaderResizableBufferTempLifetime = {
  async test() {
    const data = 'A'.repeat(1024);
    const response = await fetch('data:text/plain;base64,' + btoa(data));
    const reader = response.body.getReader({ mode: 'byob' });

    const buffer = new ArrayBuffer(512, { maxByteLength: 1024 });
    const view = new Uint8Array(buffer);

    const result = await reader.read(view);
    reader.releaseLock();

    strictEqual(result.done, false);
    ok(result.value.byteLength > 0);
    const text = new TextDecoder().decode(result.value);
    strictEqual(text, 'A'.repeat(result.value.byteLength));
  },
};
