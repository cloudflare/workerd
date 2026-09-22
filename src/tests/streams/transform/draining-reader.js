// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// ReadableStreamDrainingReader over the readable side of JS-BACKED
// TransformStreams — the bulk-drain conduit the C++ bridge drives to
// consume TypeScript streams (conduit basics in the identity suite's
// draining-reader.js). Exists only under the TS implementation
// (transform-ts.wd-test sets expose_draining_reader); the C++ cell
// asserts the global's absence.
//
// Transform facts pinned here: writes flow through the transformer into
// conduit reads with write settlement following readable-side demand; a
// backlog buffered on the readable side (readable hwm covering it) is
// swept in one batched read together with the close sentinel; flush()
// output rides the final batch; a TransformStream readable never
// declares an expectedLength.

/* global ReadableStreamDrainingReader */

import { strictEqual, deepStrictEqual } from 'node:assert';
import { usingTsImpl } from 'which-impl';

export const drainingReaderThroughTransform = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const ts = new TransformStream({
      transform(chunk, controller) {
        controller.enqueue(chunk.toUpperCase());
      },
    });
    const writer = ts.writable.getWriter();
    const pending = [writer.write('one'), writer.write('two'), writer.close()];
    const reader = new ReadableStreamDrainingReader(ts.readable);
    strictEqual(reader.expectedLength, undefined);
    const seen = [];
    for (;;) {
      const { chunks, done } = await reader.read();
      seen.push(...chunks);
      if (done) break;
    }
    deepStrictEqual(seen, ['ONE', 'TWO']);
    await Promise.all(pending);
  },
};

export const drainingReaderSweepsTransformBacklog = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    // With a readable-side hwm covering every output, the whole
    // transformed backlog plus the close sentinel buffers before the
    // conduit attaches, and one read sweeps it.
    const ts = new TransformStream(
      {
        transform(chunk, controller) {
          controller.enqueue(chunk * 10);
        },
      },
      undefined,
      { highWaterMark: 8 }
    );
    const writer = ts.writable.getWriter();
    await writer.write(1);
    await writer.write(2);
    await writer.write(3);
    await writer.close();
    const reader = new ReadableStreamDrainingReader(ts.readable);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    deepStrictEqual(chunks, [10, 20, 30]);
  },
};

export const drainingReaderSeesFlushOutput = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const ts = new TransformStream(
      {
        transform(chunk, controller) {
          controller.enqueue(chunk);
        },
        flush(controller) {
          controller.enqueue('flushed');
        },
      },
      undefined,
      { highWaterMark: 8 }
    );
    const writer = ts.writable.getWriter();
    await writer.write('data');
    await writer.close();
    const reader = new ReadableStreamDrainingReader(ts.readable);
    const { chunks, done } = await reader.read();
    strictEqual(done, true);
    deepStrictEqual(chunks, ['data', 'flushed']);
  },
};

export const drainingReaderTransformErrorPropagation = {
  async test() {
    if (!usingTsImpl) {
      strictEqual(typeof ReadableStreamDrainingReader, 'undefined');
      return;
    }
    const terr = new Error('transform-err');
    const ts = new TransformStream({
      transform() {
        throw terr;
      },
    });
    const writer = ts.writable.getWriter();
    const reader = new ReadableStreamDrainingReader(ts.readable);
    const readP = reader.read();
    writer.write('boom').catch(() => {});
    const reason = await readP.then(
      () => {
        throw new Error('expected rejection');
      },
      (e) => e
    );
    strictEqual(reason, terr);
  },
};
