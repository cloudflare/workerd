// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Manually add globals expected by eslint only for testing
/* global ReadableStreamDrainingReader */

import {
  strictEqual,
  notStrictEqual,
  ok,
  deepStrictEqual,
  rejects,
  throws,
  doesNotMatch,
} from 'node:assert';

const enc = new TextEncoder();

// Verify the implementations are the TypeScript versions, not native C++.
export const isTypeScriptImpl = {
  test() {
    doesNotMatch(IdentityTransformStream.toString(), /\[native code\]/);
    doesNotMatch(FixedLengthStream.toString(), /\[native code\]/);
  },
};

// SymbolToStringTag should be set correctly.
export const toStringTag = {
  test() {
    const its = new IdentityTransformStream();
    strictEqual(
      Object.prototype.toString.call(its),
      '[object IdentityTransformStream]'
    );
    const fls = new FixedLengthStream(0);
    strictEqual(
      Object.prototype.toString.call(fls),
      '[object FixedLengthStream]'
    );
  },
};

// readable and writable should be enumerable on the prototype.
export const propertyEnumerability = {
  test() {
    const its = new IdentityTransformStream();
    const descriptors = Object.getOwnPropertyDescriptors(
      Object.getPrototypeOf(its)
    );
    strictEqual(descriptors.readable.enumerable, true);
    strictEqual(descriptors.writable.enumerable, true);
  },
};

// FixedLengthStream is a subclass of IdentityTransformStream.
export const fixedLengthIsSubclass = {
  test() {
    const fls = new FixedLengthStream(10);
    ok(fls instanceof IdentityTransformStream);
    ok(fls instanceof FixedLengthStream);
  },
};

// --- Chunk validation tests ---

// Accepts Uint8Array.
export const acceptsUint8Array = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    // Start read before awaiting write to avoid backpressure deadlock.
    const readPromise = reader.read();
    await writer.write(new Uint8Array([1, 2, 3]));
    const { value, done } = await readPromise;
    strictEqual(done, false);
    deepStrictEqual([...value], [1, 2, 3]);
    await writer.close();
  },
};

// Accepts strings (converted to UTF-8).
export const acceptsString = {
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

// Accepts ArrayBuffer.
export const acceptsArrayBuffer = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const buf = new Uint8Array([10, 20, 30]).buffer;
    const readPromise = reader.read();
    await writer.write(buf);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    deepStrictEqual([...value], [10, 20, 30]);
    await writer.close();
  },
};

// Accepts DataView.
export const acceptsDataView = {
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

// Rejects non-byte-source chunks with TypeError.
export const rejectsInvalidChunks = {
  async test() {
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await rejects(writer.write(42), TypeError);
  },
};

// Rejects objects with TypeError.
export const rejectsObjectChunks = {
  async test() {
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    await rejects(writer.write({ data: 'nope' }), TypeError);
  },
};

// --- Zero-length write is a no-op ---

export const zeroLengthUint8ArrayIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    // Write zero-length then real data — only the real data appears.
    await writer.write(new Uint8Array(0));
    writer.write(new Uint8Array([42]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual([...value], [42]);
    await writer.close();
  },
};

export const zeroLengthStringIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write('');
    writer.write('x');
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'x');
    await writer.close();
  },
};

export const zeroLengthArrayBufferIsNoop = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    await writer.write(new ArrayBuffer(0));
    writer.write(new Uint8Array([99]));
    const { value, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual([...value], [99]);
    await writer.close();
  },
};

// --- Copy semantics: writes always copy, never transfer ---

export const writeCopiesData = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const original = new Uint8Array([1, 2, 3]);
    writer.write(original);
    const { value } = await reader.read();
    // Mutating the original after write should not affect the read value.
    original[0] = 99;
    strictEqual(value[0], 1);
    // The buffers must be different objects.
    ok(value.buffer !== original.buffer);
    await writer.close();
  },
};

// --- Write subarray: offset+length are respected ---

export const writeSubarray = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const u8 = new Uint8Array([1, 2, 3, 4]);
    writer.write(u8.subarray(1, 3));
    writer.close();
    const { value } = await reader.read();
    strictEqual(value.length, 2);
    strictEqual(value[0], u8[1]);
    strictEqual(value[1], u8[2]);
  },
};

// --- Readable side is a byte stream (BYOB capable) ---

export const readableIsByteStream = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader({ mode: 'byob' });
    writer.write(new Uint8Array([10, 20, 30]));
    const { value, done } = await reader.read(new Uint8Array(10));
    strictEqual(done, false);
    ok(value instanceof Uint8Array);
    deepStrictEqual([...value.slice(0, 3)], [10, 20, 30]);
    await writer.close();
  },
};

// --- Write before read (backpressure) ---

export const writeBeforeRead = {
  async test() {
    const MAX_RW = 10;
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromises = [];
    for (let i = 0; i < MAX_RW; i++) {
      writePromises.push(writer.write(new Uint8Array([i])));
    }
    const chunks = [];
    for (let i = 0; i < MAX_RW; i++) {
      chunks.push(await reader.read());
    }
    await Promise.all(writePromises);
    for (let i = 0; i < chunks.length; i++) {
      deepStrictEqual([...chunks[i].value], [i]);
      strictEqual(chunks[i].done, false);
    }
    const writeClosePromise = writer.close();
    const chunk = await reader.read();
    await writeClosePromise;
    strictEqual(chunk.done, true);
    await writer.closed;
    await reader.closed;
  },
};

// --- Read before write ---

export const readBeforeWrite = {
  async test() {
    const MAX_RW = 10;
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const chunks = [];
    for (let i = 0; i < MAX_RW; i++) {
      const readPromise = reader.read();
      await writer.write(new Uint8Array([i]));
      chunks.push(await readPromise);
    }
    for (let i = 0; i < chunks.length; i++) {
      deepStrictEqual([...chunks[i].value], [i]);
      strictEqual(chunks[i].done, false);
    }
    const readClosePromise = reader.read();
    await writer.close();
    const chunk = await readClosePromise;
    strictEqual(chunk.done, true);
    await writer.closed;
    await reader.closed;
  },
};

// --- Close propagation ---

export const closeSignalsProperly = {
  async test() {
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

// --- Abort propagation ---

export const abortPropagation = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const readPromise = reader.read();
    await writer.abort(new Error('test abort'));
    await rejects(readPromise, Error);
    await rejects(reader.closed, Error);
    await rejects(writer.closed, Error);
  },
};

// --- Cancel propagation ---

export const cancelPropagation = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writePromise = writer.write(enc.encode('test'));
    const closePromise = writer.close();
    await reader.cancel(new Error('cancel reason'));
    await rejects(writePromise);
    await rejects(closePromise);
  },
};

// --- Multi-chunk read-all tests ---

export const readAllBytes = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const N = 4;
    const M = 1000;
    const writePromise = (async () => {
      for (let i = 0; i < N; i++) {
        const chunk = new Uint8Array(M);
        chunk.fill(i + 1);
        await writer.write(chunk);
      }
      await writer.close();
    })();
    // Collect all chunks via the reader.
    const chunks = [];
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    const totalLength = chunks.reduce((s, c) => s + c.byteLength, 0);
    strictEqual(totalLength, N * M);
    const body = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of chunks) {
      body.set(chunk, offset);
      offset += chunk.byteLength;
    }
    for (let i = 0; i < body.length; i++) {
      strictEqual(body[i], Math.floor(i / M) + 1);
    }
    await writePromise;
  },
};

export const readAllText = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const dec = new TextDecoder();
    const writePromise = (async () => {
      await writer.write('hello ');
      await writer.write('world');
      await writer.close();
    })();
    // Collect all chunks via the reader.
    let text = '';
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      text += dec.decode(value, { stream: true });
    }
    text += dec.decode();
    strictEqual(text, 'hello world');
    await writePromise;
  },
};

// --- FixedLengthStream ---

export const fixedLengthStreamBasic = {
  async test() {
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    const readPromise = reader.read();
    await writer.write(enc.encode('hello'));
    await writer.close();
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), 'hello');
    const result2 = await reader.read();
    strictEqual(result2.done, true);
  },
};

export const fixedLengthStreamPreconditions = {
  test() {
    // Can construct with zero.
    new FixedLengthStream(0);
    // Can construct with negative zero (coerced to 0n internally).
    new FixedLengthStream(-0.0);
    // Can construct with MAX_SAFE_INTEGER.
    new FixedLengthStream(Number.MAX_SAFE_INTEGER);
    // Can construct with bigint.
    new FixedLengthStream(100n);
    // Can construct with max uint64_t.
    new FixedLengthStream(0xffff_ffff_ffff_ffffn);
    // Cannot construct with non-integer (fraction) — BigInt(0.5) throws RangeError.
    throws(() => new FixedLengthStream(0.5), RangeError);
    // Cannot construct with NaN — BigInt(NaN) throws RangeError.
    throws(() => new FixedLengthStream(NaN), RangeError);
    // Cannot construct with negative integer.
    throws(() => new FixedLengthStream(-1), RangeError);
    // Cannot construct with negative bigint.
    throws(() => new FixedLengthStream(-1n), RangeError);
    // Cannot construct with bigint exceeding uint64_t max.
    throws(() => new FixedLengthStream(0x1_0000_0000_0000_0000n), RangeError);
    // Cannot construct with non-number/non-bigint.
    throws(() => new FixedLengthStream('10'), TypeError);
  },
};

export const teeFixedLengthStreamNoHang = {
  async test() {
    const ts = new FixedLengthStream(11);
    const writer = ts.writable.getWriter();
    writer.write(enc.encode('foo bar baz'));
    writer.close();
    const [left, _right] = ts.readable.tee();
    // Read from the teed branch directly via a reader.
    const reader = left.getReader();
    const dec = new TextDecoder();
    let text = '';
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      text += dec.decode(value, { stream: true });
    }
    text += dec.decode();
    strictEqual(text, 'foo bar baz');
  },
};

// --- Lock release behavior ---

export const closedPromiseUnderLockRelease = {
  async test() {
    const { readable, writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    const reader = readable.getReader();
    const writerClosed = writer.closed;
    const readerClosed = reader.closed;
    writer.releaseLock();
    await rejects(writerClosed, TypeError);
    reader.releaseLock();
    await rejects(readerClosed, TypeError);
  },
};

// --- writableStrategy / highWaterMark forwarding ---

// ITS with explicit HWM: initial desiredSize equals HWM.
export const itsWithHWMSetsDesiredSize = {
  test() {
    const { writable } = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = writable.getWriter();
    strictEqual(writer.desiredSize, 10);
  },
};

// FLS: HWM larger than expectedLength is capped.
export const flsHWMCappedAtExpectedLength = {
  test() {
    const fls = new FixedLengthStream(10, { highWaterMark: 100 });
    const writer = fls.writable.getWriter();
    strictEqual(writer.desiredSize, 10);
  },
};

// FLS: HWM smaller than expectedLength is kept as-is.
export const flsHWMNotCappedWhenSmaller = {
  test() {
    const fls = new FixedLengthStream(100, { highWaterMark: 5 });
    const writer = fls.writable.getWriter();
    strictEqual(writer.desiredSize, 5);
  },
};

// FLS with no writableStrategy: default HWM (1).
export const flsNoHWMDefaultBehavior = {
  test() {
    const fls = new FixedLengthStream(10);
    const writer = fls.writable.getWriter();
    strictEqual(writer.desiredSize, 1);
  },
};

// ITS with no writableStrategy: default HWM (1).
export const itsNoHWMDefaultBehavior = {
  test() {
    const { writable } = new IdentityTransformStream();
    const writer = writable.getWriter();
    strictEqual(writer.desiredSize, 1);
  },
};

// FLS with bigint expectedLength: HWM capping works.
export const flsBigintExpectedLengthCapsHWM = {
  test() {
    const fls = new FixedLengthStream(10n, { highWaterMark: 100 });
    const writer = fls.writable.getWriter();
    strictEqual(writer.desiredSize, 10);
  },
};

// ITS with HWM: desiredSize tracks byte count (not chunk count),
// including bytes currently being processed (in-flight). This matches
// the C++ WritableStreamInternalController.adjustWriteBufferSize model
// where all pipeline bytes are counted until fully consumed.
export const itsHWMByteLevelQueueTracking = {
  async test() {
    const { readable, writable } = new IdentityTransformStream({
      highWaterMark: 20,
    });
    const writer = writable.getWriter();
    const reader = readable.getReader();

    strictEqual(writer.desiredSize, 20);

    // Writes stay counted in desiredSize until fully consumed.
    writer.write(new Uint8Array(5));
    strictEqual(writer.desiredSize, 15); // 20 - 5

    writer.write(new Uint8Array(5));
    strictEqual(writer.desiredSize, 10); // 20 - 10

    writer.write(new Uint8Array(7));
    strictEqual(writer.desiredSize, 3); // 20 - 17

    // Read all chunks to drain. After each read resolves a write,
    // the bytes are subtracted and desiredSize recovers.
    await reader.read();
    await reader.read();
    await reader.read();

    // All writes consumed — desiredSize back to HWM.
    strictEqual(writer.desiredSize, 20);

    await writer.close();
  },
};

// ITS with HWM + string writes: byteSize uses str.length * 3 as a
// conservative upper-bound estimate (max UTF-8 bytes per UTF-16 code
// unit). This is intentional — byteSize only feeds backpressure
// signaling, so overcounting is harmless (just signals backpressure
// slightly earlier).
export const itsHWMStringOverestimate = {
  async test() {
    const { readable, writable } = new IdentityTransformStream({
      highWaterMark: 100,
    });
    const writer = writable.getWriter();
    const reader = readable.getReader();

    strictEqual(writer.desiredSize, 100);

    // "hello" is 5 chars, all ASCII → 5 actual UTF-8 bytes.
    // byteSize estimates 5 * 3 = 15.
    writer.write('hello');
    strictEqual(writer.desiredSize, 85); // 100 - 15

    // After reading, the estimate is subtracted back.
    await reader.read();
    strictEqual(writer.desiredSize, 100);

    await writer.close();
  },
};

// Mirrors the C++ identitytransformstream-backpressure-test: desiredSize
// tracking with interleaved reads after the controller has started.
export const itsHWMBackpressureMatchesCpp = {
  async test() {
    const ts = new IdentityTransformStream({ highWaterMark: 10 });
    const writer = ts.writable.getWriter();
    const reader = ts.readable.getReader();

    strictEqual(writer.desiredSize, 10);

    // Wait for writer.ready so the controller's start has resolved.
    const firstReady = writer.ready;
    await writer.ready;

    writer.write(new Uint8Array(1));
    strictEqual(writer.desiredSize, 9);

    // A second write that fills the buffer completely.
    writer.write(new Uint8Array(9));
    strictEqual(writer.desiredSize, 0);

    // The ready promise should have been replaced (backpressure on).
    notStrictEqual(firstReady, writer.ready);

    async function waitForReady() {
      strictEqual(writer.desiredSize, 0);
      await writer.ready;
      // After reading 1 byte, backpressure relieved by that amount.
      strictEqual(writer.desiredSize, 1);
    }

    await Promise.all([waitForReady(), reader.read()]);

    // Read the remaining 9 bytes.
    await reader.read();
    strictEqual(writer.desiredSize, 10);
  },
};

// FLS: HWM forwarding + capping work together with data flow.
export const flsHWMForwardingWithDataFlow = {
  async test() {
    const data = 'hello world, padding'; // exactly 20 bytes
    const fls = new FixedLengthStream(data.length, { highWaterMark: 50 });
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Capped: min(50, 20) = 20.
    strictEqual(writer.desiredSize, data.length);
    const readPromise = reader.read();
    await writer.write(data);
    const { value, done } = await readPromise;
    strictEqual(done, false);
    strictEqual(new TextDecoder().decode(value), data);
    await writer.close();
  },
};

// --- Illegal invocation on wrong receiver ---

// --- FixedLengthStream overwrite / underwrite enforcement ---

export const flsOverwriteThrows = {
  async test() {
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Write exactly 5 bytes — reader unblocks the rendezvous.
    const writeP = writer.write(new Uint8Array([1, 2, 3, 4, 5]));
    await reader.read(); // drain to unblock the write
    await writeP;
    // Now remaining is 0; next write fails before the rendezvous.
    await rejects(
      () => writer.write(new Uint8Array([6])),
      (err) => {
        ok(err instanceof RangeError);
        ok(
          err.message.includes(
            'Attempt to write too many bytes through a FixedLengthStream'
          )
        );
        return true;
      }
    );
  },
};

export const flsOverwriteSingleChunkThrows = {
  async test() {
    // Single write exceeding the limit.
    const fls = new FixedLengthStream(3);
    const writer = fls.writable.getWriter();
    await rejects(
      () => writer.write(new Uint8Array([1, 2, 3, 4])),
      (err) => {
        ok(err instanceof RangeError);
        ok(err.message.includes('too many bytes'));
        return true;
      }
    );
  },
};

export const flsUnderwriteThrows = {
  async test() {
    const fls = new FixedLengthStream(10);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Write 3 of 10, drain to unblock, then close.
    const writeP = writer.write(new Uint8Array([1, 2, 3]));
    await reader.read();
    await writeP;
    await rejects(
      () => writer.close(),
      (err) => {
        ok(err instanceof RangeError);
        ok(
          err.message.includes('did not see all expected bytes before close()')
        );
        return true;
      }
    );
  },
};

export const flsUnderwriteZeroBytesThrows = {
  async test() {
    // Close immediately without writing anything.
    const fls = new FixedLengthStream(5);
    const writer = fls.writable.getWriter();
    await rejects(
      () => writer.close(),
      (err) => {
        ok(err instanceof RangeError);
        ok(err.message.includes('did not see all expected bytes'));
        return true;
      }
    );
  },
};

export const flsExactWriteSucceeds = {
  async test() {
    const fls = new FixedLengthStream(6);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    // Write in two chunks totaling exactly 6 bytes.
    // Interleave reads to unblock the rendezvous.
    const w1 = writer.write(new Uint8Array([1, 2, 3]));
    const r1 = await reader.read();
    await w1;
    strictEqual(r1.value.byteLength, 3);
    const w2 = writer.write(new Uint8Array([4, 5, 6]));
    const r2 = await reader.read();
    await w2;
    strictEqual(r2.value.byteLength, 3);
    await writer.close();
    const r3 = await reader.read();
    strictEqual(r3.done, true);
  },
};

export const flsAbortSkipsUnderwriteCheck = {
  async test() {
    // Abort should NOT trigger the underwrite error — only 1 of 100
    // bytes written, but abort is not close, so no underwrite check.
    const fls = new FixedLengthStream(100);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    const writeP = writer.write(new Uint8Array([1]));
    await reader.read(); // drain to unblock
    await writeP;
    // Abort should resolve without throwing underwrite RangeError.
    await writer.abort(new Error('cancelled'));
    // Writable is errored (aborted), not just closed normally.
    strictEqual(fls.writable.locked, true);
    writer.releaseLock();
    strictEqual(fls.writable.locked, false);
  },
};

export const flsOverwriteErrorsReadable = {
  async test() {
    // After overwrite, the readable side should also be errored.
    const fls = new FixedLengthStream(2);
    const writer = fls.writable.getWriter();
    const reader = fls.readable.getReader();
    await rejects(
      () => writer.write(new Uint8Array([1, 2, 3])),
      (err) => err instanceof RangeError
    );
    // Readable should be errored too.
    await rejects(
      () => reader.read(),
      (err) => err instanceof RangeError
    );
  },
};

// --- expectedLength / Content-Length integration via DrainingReader ---

export const flsExpectedLengthViaReader = {
  test() {
    // FixedLengthStream(42) should expose expectedLength=42n through a
    // DrainingReader on its readable side (the C++ Content-Length path).
    const fls = new FixedLengthStream(42);
    const reader = new ReadableStreamDrainingReader(fls.readable);
    strictEqual(reader.expectedLength, 42n);
    reader.cancel();
  },
};

export const flsBigintExpectedLengthViaReader = {
  test() {
    // bigint expectedLength should pass through as-is.
    const fls = new FixedLengthStream(9007199254740993n);
    const reader = new ReadableStreamDrainingReader(fls.readable);
    strictEqual(reader.expectedLength, 9007199254740993n);
    reader.cancel();
  },
};

export const itsNoExpectedLengthViaReader = {
  test() {
    // Plain IdentityTransformStream has no expectedLength (undefined →
    // chunked encoding).
    const its = new IdentityTransformStream();
    const reader = new ReadableStreamDrainingReader(its.readable);
    strictEqual(reader.expectedLength, undefined);
    reader.cancel();
  },
};

export const flsExpectedLengthZeroViaReader = {
  test() {
    // FixedLengthStream(0) — valid, means "the source will close without
    // delivering any bytes". expectedLength should be 0n.
    const fls = new FixedLengthStream(0);
    const reader = new ReadableStreamDrainingReader(fls.readable);
    strictEqual(reader.expectedLength, 0n);
    reader.cancel();
  },
};

export const illegalInvocation = {
  test() {
    const its = new IdentityTransformStream();
    const desc = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(its),
      'readable'
    );
    throws(() => desc.get.call({}), TypeError);
    throws(() => desc.get.call(null), TypeError);
  },
};
