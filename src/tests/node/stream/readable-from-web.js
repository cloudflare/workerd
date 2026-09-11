// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.fromWeb(): a node Readable pulling from a web ReadableStream. The
// adapter takes a default reader at construction and issues one read() per
// _read() call, pushing each chunk and translating done into EOF and reader
// errors into destroy().

import { Readable } from 'node:stream';
import { strictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// Chunks enqueued by the web source surface as 'data' events.
export const fromWebDeliversDataEvents = {
  async test() {
    const rs = new ReadableStream({
      pull(c) {
        c.enqueue(enc.encode('ok'));
        c.close();
      },
    });
    const r = Readable.fromWeb(rs);
    strictEqual(r instanceof Readable, true);
    const { promise, resolve } = Promise.withResolvers();
    r.on('data', (chunk) => {
      strictEqual(dec.decode(chunk), 'ok');
      resolve();
    });
    await promise;
  },
};

// A web source errored from start() rejects the async iteration of the
// adapted Readable with the original error. (The source errors through its
// controller: a start() that throws makes the spec-conformant constructor
// itself throw, which never reaches the adapter.)
export const fromWebErroredAtStartRejectsAsyncIteration = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.error(new Error('boom'));
      },
    });
    const r = Readable.fromWeb(rs);
    await rejects(
      (async () => {
        for await (const _chunk of r) {
          // Nothing is ever delivered.
        }
      })(),
      { message: 'boom' }
    );
  },
};

// An error thrown from a delayed pull() surfaces the same way, after the
// adapter has already started reading.
export const fromWebPullErrorRejectsAsyncIteration = {
  async test() {
    const rs = new ReadableStream({
      async pull() {
        await scheduler.wait(10);
        throw new Error('boom');
      },
    });
    const r = Readable.fromWeb(rs);
    await rejects(
      (async () => {
        for await (const _chunk of r) {
          // Nothing is ever delivered.
        }
      })(),
      { message: 'boom' }
    );
  },
};
