// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over the encoding streams — the bulk-drain
// conduit the C++ bridge drives to consume TypeScript streams (see the
// identity suite's draining-reader.js for the conduit basics). Exists only
// under the TS implementation (encoding-ts.wd-test sets
// expose_draining_reader); the C++ cell asserts the global's absence.
//
// Encoding-specific facts pinned here: neither stream declares an
// expectedLength; the encoder's readable drains as Uint8Array chunks (one
// per read — under readable HWM 0 nothing is ever synchronously buffered —
// unless a tee sibling built a real backlog); and the decoder's STRING
// chunks pass through the conduit untouched — byte validation happens at
// consumption (see body-integration.js), not in the conduit.

/* global ReadableStreamDrainingReader */

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const drainingReaderOverEncoder = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const pending = [writer.write('en'), writer.write('code'), writer.close()];
    const reader = new ReadableStreamDrainingReader(tes.readable);
    strictEqual(reader.expectedLength, undefined);
    strictEqual(tes.readable.locked, true);
    const dec = new TextDecoder();
    for (const expected of ['en', 'code']) {
      const { chunks, done } = await reader.read();
      strictEqual(done, false);
      strictEqual(chunks.length, 1);
      strictEqual(chunks[0].constructor, Uint8Array);
      strictEqual(dec.decode(chunks[0]), expected);
    }
    const tail = await reader.read();
    strictEqual(tail.done, true);
    strictEqual(tail.chunks.length, 0);
    await Promise.all(pending);
  },
};

export const drainingReaderBatchesEncoderBacklog = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // A tee sibling holds every buffered copy plus the close sentinel once
    // the other branch has consumed the stream; one read sweeps them all.
    const tes = new TextEncoderStream();
    const writer = tes.writable.getWriter();
    const pending = [writer.write('a'), writer.write('b'), writer.close()];
    const [a, b] = tes.readable.tee();
    const readerA = a.getReader();
    for (;;) {
      const { done } = await readerA.read();
      if (done) break;
    }
    await Promise.all(pending);
    const reader = new ReadableStreamDrainingReader(b);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    const dec = new TextDecoder();
    deepStrictEqual(
      chunks.map((chunk) => dec.decode(chunk)),
      ['a', 'b']
    );
  },
};

export const drainingReaderOverDecoderYieldsRawStrings = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const tds = new TextDecoderStream();
    const writer = tds.writable.getWriter();
    const pending = [
      writer.write(new TextEncoder().encode('str')),
      writer.close(),
    ];
    const reader = new ReadableStreamDrainingReader(tds.readable);
    strictEqual(reader.expectedLength, undefined);
    const { chunks, done } = await reader.read();
    strictEqual(done, false);
    deepStrictEqual(chunks, ['str']);
    await Promise.all(pending);
    strictEqual((await reader.read()).done, true);
  },
};
