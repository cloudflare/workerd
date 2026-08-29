// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which chunk types the writable side of an IdentityTransformStream accepts:
// ArrayBufferView (any), ArrayBuffer, and (for historical reasons) strings,
// which are UTF-8 encoded. Everything else rejects with TypeError. (Both
// test configs pin capture_async_api_throws, so the C++ implementation
// returns a rejected promise here like TypeScript does, rather than its
// pre-flag synchronous throw. It still emits a console warning.)
//
// What happens to the stream afterward deliberately diverges and is asserted
// per implementation below:
// - C++: the stream is unaffected by the invalid chunk and remains usable.
// - TypeScript: the invalid chunk errors the stream — the writer's closed
//   promise rejects and subsequent writes reject.

import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

export const acceptsUint8Array = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    // Start the read before awaiting the write: the identity stream is a
    // rendezvous, so a write only completes once a read consumes it.
    const readPromise = reader.read();
    await writer.write(new Uint8Array([1, 2, 3]));
    const { value, done } = await readPromise;
    strictEqual(done, false);
    deepStrictEqual([...value], [1, 2, 3]);
    await writer.close();
  },
};

export const acceptsArrayBuffer = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.write(new Uint8Array([10, 20, 30]).buffer);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    deepStrictEqual([...value], [10, 20, 30]);
    await writer.close();
  },
};

export const acceptsDataViewSubrange = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const buf = new Uint8Array([5, 6, 7, 8]).buffer;
    const dv = new DataView(buf, 1, 2); // bytes [6, 7]
    const readPromise = reader.read();
    await writer.write(dv);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    deepStrictEqual([...value], [6, 7]);
    await writer.close();
  },
};

export const acceptsStringAsUtf8 = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.write('hello');
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'hello');
    await writer.close();
  },
};

export const respectsViewOffsets = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const u8 = new Uint8Array([1, 2, 3, 4]);
    const readPromise = reader.read();
    await writer.write(u8.subarray(1, 3));
    const { value } = await readPromise;
    strictEqual(value.length, 2);
    strictEqual(value[0], 2);
    strictEqual(value[1], 3);
    await writer.close();
  },
};

// An invalid chunk rejects ITS OWN write only; the stream stays usable
// (parity — the DECIDED contract for the internal transforms, matching
// the C++ internal controllers; under TypeScript the sink signals the
// rejection through the non-fatal write-rejection channel).
export const rejectsNumberChunk = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await rejects(writer.write(42), TypeError);
    // The stream is unaffected: a subsequent valid write still flows.
    const reader = readable.getReader();
    const writePromise = writer.write(new Uint8Array([1]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(value[0], 1);
    await writePromise;
    await writer.close();
  },
};

export const invalidChunkAfterQueuedValidWrites = {
  async test() {
    // An invalid chunk queued BEHIND valid writes must not cost them their
    // delivery: validation errors surface in FIFO order, after everything
    // written earlier has been consumed, and the stream survives the
    // rejection (parity — the DECIDED per-write-rejection contract).
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const dec = new TextDecoder();
    const enc = new TextEncoder();
    const w1 = writer.write(enc.encode('a'));
    const w2 = writer.write(enc.encode('b'));
    const wBad = writer.write(42);
    const [r1] = await Promise.all([reader.read(), w1]);
    strictEqual(dec.decode(r1.value), 'a');
    const [r2] = await Promise.all([reader.read(), w2]);
    strictEqual(dec.decode(r2.value), 'b');
    await rejects(wBad, TypeError);
    // The stream survives; later traffic still flows.
    const w4 = writer.write(enc.encode('c'));
    const [r4] = await Promise.all([reader.read(), w4]);
    strictEqual(dec.decode(r4.value), 'c');
    await writer.close();
  },
};

export const rejectsObjectChunk = {
  async test() {
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await rejects(writer.write({ data: 'nope' }), TypeError);
  },
};
