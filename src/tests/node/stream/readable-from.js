// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.from() over a web ReadableStream: the stream is consumed through
// its async iterator rather than a reader, which shapes how chunks, errors,
// and destruction cross over.

import { Readable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual } from 'node:assert';

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Readable.from() defaults to objectMode, so byte chunks arrive as the
// Uint8Arrays the stream enqueued; with objectMode: false they become
// Buffers.
export const fromWebStreamChunkTypes = {
  async test() {
    const make = () =>
      new ReadableStream({
        start(controller) {
          controller.enqueue(new Uint8Array([1, 2]));
          controller.enqueue(new Uint8Array([3]));
          controller.close();
        },
      });
    const objects = Readable.from(make());
    strictEqual(objects.readableObjectMode, true);
    const objectChunks = [];
    for await (const chunk of objects) objectChunks.push(chunk);
    strictEqual(objectChunks.length, 2);
    strictEqual(objectChunks[0].constructor, Uint8Array);
    strictEqual(Buffer.isBuffer(objectChunks[0]), false);

    const bytes = Readable.from(make(), { objectMode: false });
    const byteChunks = [];
    for await (const chunk of bytes) byteChunks.push(chunk);
    strictEqual(Buffer.isBuffer(byteChunks[0]), true);
    deepStrictEqual([...Buffer.concat(byteChunks)], [1, 2, 3]);
  },
};

// Destroying the Readable returns the async iterator, which cancels the web
// stream (with no reason) and releases its lock.
export const fromWebStreamDestroyCancelsSource = {
  async test() {
    let cancelled = false;
    let cancelReason = 'unset';
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue(new Uint8Array([1]));
        controller.enqueue(new Uint8Array([2]));
      },
      cancel(reason) {
        cancelled = true;
        cancelReason = reason;
      },
    });
    const r = Readable.from(rs);
    await once(r, 'data');
    const closed = once(r, 'close');
    r.destroy();
    await closed;
    strictEqual(cancelled, true);
    strictEqual(cancelReason, undefined);
    strictEqual(rs.locked, false);
  },
};

// An error from the web source surfaces as the Readable's 'error'.
export const fromWebStreamErrorPropagates = {
  async test() {
    const boom = new Error('pull failed');
    const rs = new ReadableStream({
      pull() {
        throw boom;
      },
    });
    const r = Readable.from(rs);
    const errored = once(r, 'error');
    r.resume();
    strictEqual(await errored, boom);
    strictEqual(r.destroyed, true);
  },
};
