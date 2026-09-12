// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// stream.compose() with web streams among the stages. Validation and the
// internal pipeline work under both implementations; a composed stream
// whose writable side must learn when a WEB tail finishes needs the Node.js
// interop hook (see finished-and-abort.js), so that shape is refused with
// ERR_WEB_STREAM_INTEROP_UNSUPPORTED under the C++ implementation.

import { compose, Readable, Writable, PassThrough } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

function upperTransform() {
  return new TransformStream({
    transform(chunk, controller) {
      controller.enqueue(enc.encode(dec.decode(chunk).toUpperCase()));
    },
  });
}

async function collect(readable) {
  const chunks = [];
  for await (const chunk of readable) chunks.push(chunk);
  return Buffer.concat(chunks).toString();
}

// Position validation applies to web streams too: a WritableStream cannot
// lead, a ReadableStream cannot follow.
export const composeValidatesWebStreamPositions = {
  test() {
    throws(() => compose(new WritableStream(), new PassThrough()), {
      code: 'ERR_INVALID_ARG_VALUE',
      message: /streams\[0\].*must be readable/,
    });
    throws(() => compose(new PassThrough(), new ReadableStream()), {
      code: 'ERR_INVALID_ARG_VALUE',
      message: /streams\[1\].*must be writable/,
    });
  },
};

// A single web stream composes into a Duplex over it (Duplex.from).
export const composeSingleWebStream = {
  async test() {
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('solo'));
        controller.close();
      },
    });
    const readable = compose(source);
    strictEqual(readable.readable, true);
    strictEqual(readable.writable, false);
    strictEqual(await collect(readable), 'solo');

    const seen = [];
    const writable = compose(
      new WritableStream({
        write(chunk) {
          seen.push(dec.decode(chunk));
        },
      })
    );
    strictEqual(writable.writable, true);
    strictEqual(writable.readable, false);
    await new Promise((resolve) => writable.end('sink', resolve));
    deepStrictEqual(seen, ['sink']);
  },
};

// A web TransformStream at the head with a node tail: writes go through the
// transform's writer, reads come from the node tail. The head's completion
// is observed on the node tail, so this works under both implementations.
export const composeWebHeadNodeTail = {
  async test() {
    const composed = compose(upperTransform(), new PassThrough());
    strictEqual(composed.writable, true);
    strictEqual(composed.readable, true);
    const collected = collect(composed);
    composed.write('ab');
    composed.end('cd');
    strictEqual(await collected, 'ABCD');
  },
};

// A web readable head into a node writable tail yields a Duplex with
// neither side (the pipeline runs inside it); it closes once the pipeline
// completes.
export const composeWebReadableIntoNodeWritable = {
  async test() {
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('through'));
        controller.close();
      },
    });
    const chunks = [];
    const sink = new Writable({
      write(chunk, encoding, callback) {
        chunks.push(chunk);
        callback();
      },
    });
    const composed = compose(source, sink);
    strictEqual(composed.readable, false);
    strictEqual(composed.writable, false);
    await new Promise((resolve) => composed.once('close', resolve));
    strictEqual(Buffer.concat(chunks).toString(), 'through');
    strictEqual(sink.writableFinished, true);
  },
};

// A node head with a web TransformStream tail: the composed writable side
// must observe the web tail's completion — available only with the interop
// hook. Where the hook is missing the shape is refused before anything is
// started: the head keeps its buffered input and gains no listeners, and
// the web stream's sides stay unlocked.
export const composeNodeHeadWebTail = {
  async test() {
    const make = () => compose(new PassThrough(), upperTransform());
    if (!usingTsImpl) {
      const unsupported = {
        name: 'TypeError',
        code: 'ERR_WEB_STREAM_INTEROP_UNSUPPORTED',
        message:
          'compose() is not supported for web streams by the streams implementation in use',
      };
      throws(make, unsupported);

      const head = new PassThrough();
      head.write('kept');
      const tail = upperTransform();
      const listeners = Object.fromEntries(
        head.eventNames().map((name) => [name, head.listenerCount(name)])
      );
      throws(() => compose(head, tail), unsupported);
      strictEqual(tail.writable.locked, false);
      strictEqual(tail.readable.locked, false);
      deepStrictEqual(
        Object.fromEntries(
          head.eventNames().map((name) => [name, head.listenerCount(name)])
        ),
        listeners
      );
      strictEqual(head.readableLength, 4);
      strictEqual(head.read().toString(), 'kept');
      return;
    }
    const composed = make();
    strictEqual(composed.writable, true);
    strictEqual(composed.readable, true);
    const collected = collect(composed);
    composed.write('ef');
    composed.end('gh');
    strictEqual(await collected, 'EFGH');
    strictEqual(composed.writableFinished, true);
  },
};

// Readable.prototype.compose() reaches the same machinery. Its head is a
// readable, not a writable, so no tail observation is needed and this works
// under both implementations.
export const readableComposeWithWebTransform = {
  async test() {
    const source = Readable.from([Buffer.from('ij')], { objectMode: false });
    strictEqual(await collect(source.compose(upperTransform())), 'IJ');
  },
};
