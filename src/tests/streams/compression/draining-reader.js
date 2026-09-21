// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over the compression streams (the bulk-drain
// conduit the C++ bridge drives to consume TypeScript streams). Exists only
// under the TS implementation (compression-ts.wd-test sets
// expose_draining_reader); the C++ cell asserts the global's absence.
//
// Compression-specific facts: no declared expectedLength, and — because the
// eager codec's output is queued ahead of demand, in pieces of at most
// 64 KiB — a closed stream's entire backlog (all queued pieces plus the
// close sentinel) is swept by a SINGLE read reporting done, no tee sibling
// needed (contrast the encoding suite, where HWM-0 production means nothing
// is ever synchronously buffered).

/* global ReadableStreamDrainingReader */

import { strictEqual, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';
import { pump } from 'round-trip';

export const drainingReaderSweepsBufferedBacklog = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const cs = new CompressionStream('gzip');
    const writer = cs.writable.getWriter();
    await writer.write(new TextEncoder().encode('drain me'));
    await writer.close();
    const reader = new ReadableStreamDrainingReader(cs.readable);
    strictEqual(reader.expectedLength, undefined);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    ok(chunks.length >= 1);
    for (const chunk of chunks) {
      strictEqual(chunk.constructor, Uint8Array);
    }
    strictEqual(chunks[0][0], 0x1f); // gzip magic
    const tail = await reader.read();
    strictEqual(tail.done, true);
    strictEqual(tail.chunks.length, 0);
  },
};

export const drainingReaderTakesBoundedPieces = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const kPiece = 64 * 1024;
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      new Uint8Array(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(compressed);
    await writer.close();
    const reader = new ReadableStreamDrainingReader(ds.readable);
    // One sweep takes the whole backlog, as the pieces it was queued in.
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    strictEqual(chunks.length, size / kPiece);
    let total = 0;
    for (const chunk of chunks) {
      strictEqual(chunk.byteLength, kPiece);
      total += chunk.byteLength;
    }
    strictEqual(total, size);
  },
};

export const drainingReaderLocksReadable = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const cs = new CompressionStream('gzip');
    const reader = new ReadableStreamDrainingReader(cs.readable);
    strictEqual(cs.readable.locked, true);
    reader.releaseLock();
    strictEqual(cs.readable.locked, false);
    ok(cs.readable.getReader());
  },
};
