// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over the compression streams (the bulk-drain
// conduit the C++ bridge drives to consume TypeScript streams). Exists only
// under the TS implementation (compression-ts.wd-test sets
// expose_draining_reader); the C++ cell asserts the global's absence.
//
// Compression-specific facts: no declared expectedLength, and the eager
// codec's output waits in its own buffer, delivered one 64 KiB piece per
// read. A closed stream whose remaining output fits one piece is swept by a
// SINGLE read reporting done (the piece and the close sentinel together);
// a larger backlog takes one piece per read, the last of them reporting
// done.

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
    const size = 1024 * 1024;
    const compressed = await pump(new CompressionStream('gzip'), [
      new Uint8Array(size),
    ]);
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    await writer.write(compressed);
    await writer.close();
    const reader = new ReadableStreamDrainingReader(ds.readable);
    let total = 0;
    let reads = 0;
    for (;;) {
      const { chunks, done } = await reader.read();
      reads++;
      for (const chunk of chunks) {
        ok(chunk.byteLength <= 64 * 1024);
        total += chunk.byteLength;
      }
      if (done) break;
    }
    strictEqual(total, size);
    strictEqual(reads, size / (64 * 1024));
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
