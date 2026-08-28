// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over identity streams. The draining reader
// is the internal bulk reader the C++ bridge drives to drain
// TypeScript-implemented streams — including reading a FixedLengthStream's
// expectedLength for Content-Length derivation. It exists only under the
// TypeScript implementation: identity-ts.wd-test sets the internal-testing
// expose_draining_reader flag, which the TS bootstrap consults to install
// the global. Under the C++ implementation the analogous DrainingReader is
// C++-internal and no global exists — every test here asserts that absence
// in the C++ cell, so a leak of the internal class would be caught.

/* global ReadableStreamDrainingReader */

import { ok, strictEqual, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

// Drains the reader to EOF, returning the concatenated bytes.
async function drainToBytes(reader) {
  const parts = [];
  let total = 0;
  for (;;) {
    const { chunks, done } = await reader.read();
    for (const chunk of chunks) {
      parts.push(chunk);
      total += chunk.byteLength;
    }
    if (done) break;
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.byteLength;
  }
  return out;
}

export const drainingReaderExposure = {
  test() {
    if (usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'function');
    } else {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
    }
  },
};

export const drainingReaderExpectedLengthPassThrough = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // FixedLengthStream passes its declared total through as a bigint —
    // the Content-Length path.
    {
      const fls = new FixedLengthStream(42);
      const reader = new ReadableStreamDrainingReader(fls.readable);
      strictEqual(reader.expectedLength, 42n);
      await reader.cancel();
    }
    // Zero and full-uint64-range bigints pass through as-is.
    {
      const fls = new FixedLengthStream(0);
      const reader = new ReadableStreamDrainingReader(fls.readable);
      strictEqual(reader.expectedLength, 0n);
      await reader.cancel();
    }
    {
      const fls = new FixedLengthStream(9007199254740993n);
      const reader = new ReadableStreamDrainingReader(fls.readable);
      strictEqual(reader.expectedLength, 9007199254740993n);
      await reader.cancel();
    }
    // A plain IdentityTransformStream has no declared total (undefined →
    // chunked encoding).
    {
      const its = new IdentityTransformStream();
      const reader = new ReadableStreamDrainingReader(its.readable);
      strictEqual(reader.expectedLength, undefined);
      await reader.cancel();
    }
    // After releaseLock the pass-through reports undefined.
    {
      const fls = new FixedLengthStream(7);
      const reader = new ReadableStreamDrainingReader(fls.readable);
      strictEqual(reader.expectedLength, 7n);
      reader.releaseLock();
      strictEqual(reader.expectedLength, undefined);
      strictEqual(fls.readable.locked, false);
    }
  },
};

export const drainingReaderDrainsIdentityStream = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // Queued writes and a close settle through draining-reader consumption
    // exactly as through a default reader: the rendezvous holds across the
    // conduit. Because this is a rendezvous stream, nothing is ever
    // synchronously buffered on the readable side — each read exercises the
    // always-makes-progress fallback and yields exactly one chunk, with EOF
    // arriving as a separate empty-batch read. (Contrast
    // drainingReaderBatchesBufferedChunks, where a real backlog IS swept in
    // one read.)
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const pending = [
      writer.write(enc.encode('hello ')),
      writer.write(enc.encode('draining ')),
      writer.write(enc.encode('world')),
      writer.close(),
    ];
    const reader = new ReadableStreamDrainingReader(its.readable);
    strictEqual(its.readable.locked, true);
    for (const expected of ['hello ', 'draining ', 'world']) {
      const { chunks, done } = await reader.read();
      strictEqual(done, false);
      strictEqual(chunks.length, 1);
      strictEqual(dec.decode(chunks[0]), expected);
    }
    const tail = await reader.read();
    strictEqual(tail.done, true);
    strictEqual(tail.chunks.length, 0);
    await Promise.all(pending);
  },
};

export const drainingReaderBatchesBufferedChunks = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // The defining characteristic: every synchronously available chunk is
    // drained in a SINGLE read. A tee sibling provides the backlog — after
    // the other branch consumes everything, the sibling holds all the
    // buffered copies plus the close sentinel, and one read must return
    // them all, with done reported in the same result.
    const its = new IdentityTransformStream();
    const writer = its.writable.getWriter();
    const enc = new TextEncoder();
    const dec = new TextDecoder();
    const pending = [
      writer.write(enc.encode('a')),
      writer.write(enc.encode('b')),
      writer.write(enc.encode('c')),
      writer.close(),
    ];
    const [a, b] = its.readable.tee();
    const readerA = a.getReader();
    for (;;) {
      const { done } = await readerA.read();
      if (done) break;
    }
    await Promise.all(pending);
    // Branch b now has all three chunks and EOF synchronously buffered.
    const reader = new ReadableStreamDrainingReader(b);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    deepStrictEqual(
      chunks.map((chunk) => dec.decode(chunk)),
      ['a', 'b', 'c']
    );
  },
};

export const drainingReaderDrainsFixedLengthStream = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const fls = new FixedLengthStream(11);
    const writer = fls.writable.getWriter();
    const pending = [
      writer.write(new TextEncoder().encode('foo bar baz')),
      writer.close(),
    ];
    const reader = new ReadableStreamDrainingReader(fls.readable);
    // The declared total is visible before (and during) the drain.
    strictEqual(reader.expectedLength, 11n);
    const bytes = await drainToBytes(reader);
    strictEqual(new TextDecoder().decode(bytes), 'foo bar baz');
    strictEqual(bytes.byteLength, 11);
    await Promise.all(pending);
  },
};

export const drainingReaderLocking = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const its = new IdentityTransformStream();
    const reader = new ReadableStreamDrainingReader(its.readable);
    // Acquisition locks the stream against every other reader kind.
    strictEqual(its.readable.locked, true);
    throws(() => new ReadableStreamDrainingReader(its.readable), TypeError);
    throws(() => its.readable.getReader(), TypeError);
    // Release restores the stream to a usable, lockable state.
    reader.releaseLock();
    strictEqual(its.readable.locked, false);
    ok(its.readable.getReader());
  },
};
