// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// node:stream/consumers over web ReadableStreams. The consumers use async
// iteration, so they accept a web ReadableStream, a node Readable, or any
// async iterable alike, and release the web stream's lock when done.

import { text, json, buffer, arrayBuffer, blob } from 'node:stream/consumers';
import { Readable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects } from 'node:assert';

const enc = new TextEncoder();

function bytesStream(...parts) {
  return new ReadableStream({
    start(controller) {
      for (const part of parts) controller.enqueue(enc.encode(part));
      controller.close();
    },
  });
}

// Every consumer drains a web byte stream.
export const consumersDrainWebStream = {
  async test() {
    strictEqual(await text(bytesStream('hé', 'llo')), 'héllo');
    deepStrictEqual(await json(bytesStream('{"a":', '1}')), { a: 1 });
    const buf = await buffer(bytesStream('ab', 'c'));
    strictEqual(Buffer.isBuffer(buf), true);
    strictEqual(buf.toString(), 'abc');
    const ab = await arrayBuffer(bytesStream('abcd'));
    strictEqual(ab instanceof ArrayBuffer, true);
    strictEqual(ab.byteLength, 4);
    const b = await blob(bytesStream('ab', 'cde'));
    strictEqual(b instanceof Blob, true);
    strictEqual(b.size, 5);
    strictEqual(await b.text(), 'abcde');
  },
};

// text() decodes across chunk boundaries and accepts string chunks as well.
export const textDecodesAcrossChunksAndStrings = {
  async test() {
    const split = new ReadableStream({
      start(controller) {
        const bytes = enc.encode('é');
        controller.enqueue(bytes.subarray(0, 1));
        controller.enqueue(bytes.subarray(1));
        controller.close();
      },
    });
    strictEqual(await text(split), 'é');
    const strings = new ReadableStream({
      start(controller) {
        controller.enqueue('s1');
        controller.enqueue('s2');
        controller.close();
      },
    });
    strictEqual(await text(strings), 's1s2');
  },
};

// Consumption iterates the stream and leaves it unlocked afterwards.
export const consumersReleaseLock = {
  async test() {
    const rs = bytesStream('x');
    await text(rs);
    strictEqual(rs.locked, false);
    const { done } = await rs.getReader().read();
    strictEqual(done, true);
  },
};

// An erroring stream rejects the consumer with its error.
export const consumersPropagateStreamError = {
  async test() {
    const boom = new Error('source failed');
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('partial'));
        controller.error(boom);
      },
    });
    await rejects(text(rs), (err) => err === boom);
  },
};

// The same consumers accept node Readables and plain async generators.
export const consumersAcceptNodeAndAsyncIterables = {
  async test() {
    strictEqual(await text(Readable.from(['a', 'b'])), 'ab');
    strictEqual(
      await text(
        Readable.from([Buffer.from('c'), Buffer.from('d')], {
          objectMode: false,
        })
      ),
      'cd'
    );
    async function* gen() {
      yield enc.encode('e');
      yield 'f';
    }
    strictEqual(await text(gen()), 'ef');
  },
};
