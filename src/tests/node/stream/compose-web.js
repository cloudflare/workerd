// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// stream.compose() with web streams among the stages. Validation and the
// internal pipeline work under both implementations; a composed stream
// whose writable side must learn when a WEB tail finishes needs the Node.js
// interop hook (see finished-and-abort.js), so that shape is refused with
// ERR_WEB_STREAM_INTEROP_UNSUPPORTED under the C++ implementation.

import {
  compose,
  promises,
  Readable,
  Writable,
  PassThrough,
} from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws, rejects } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

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

// The composed stream's end() completes without a consumer: the tail's
// output is drained into the composed stream's own buffer as it is
// produced, so the writable side finishes on its own and the output is
// still there to be read afterwards. With a node tail this holds whether
// the head is a node stream or a web transform.
export const composeEndCompletesBeforeReading = {
  async test() {
    const nodeHead = compose(new PassThrough(), new PassThrough());
    await new Promise((resolve) => nodeHead.end('x', resolve));
    strictEqual(nodeHead.writableFinished, true);
    strictEqual(await collect(nodeHead), 'x');

    const webHead = compose(upperTransform(), new PassThrough());
    await new Promise((resolve) => webHead.end('y', resolve));
    strictEqual(webHead.writableFinished, true);
    strictEqual(await collect(webHead), 'Y');
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

// A node head with a web WritableStream tail: the composition has a
// writable side only; its end() completes once the sink has closed, and it
// closes cleanly (needs the interop hook to observe the sink).
export const composeNodeHeadWebWritableTail = {
  async test() {
    if (!usingTsImpl) return;
    const seen = [];
    const sink = new WritableStream({
      write(chunk) {
        seen.push(dec.decode(chunk));
      },
      close() {
        seen.push('close');
      },
    });
    const composed = compose(new PassThrough(), sink);
    strictEqual(composed.writable, true);
    strictEqual(composed.readable, false);
    composed.on('error', (err) => {
      throw err;
    });
    const closed = once(composed, 'close');
    composed.write('kl');
    await new Promise((resolve) => composed.end('mn', resolve));
    await closed;
    // The head may hand the pump both writes as one chunk.
    strictEqual(seen.at(-1), 'close');
    strictEqual(seen.slice(0, -1).join(''), 'klmn');
  },
};

// A web readable head into a web writable tail: a Duplex with neither side
// that closes cleanly once the pipeline completes.
export const composeWebReadableIntoWebWritable = {
  async test() {
    const seen = [];
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('op'));
        controller.close();
      },
    });
    const sink = new WritableStream({
      write(chunk) {
        seen.push(dec.decode(chunk));
      },
      close() {
        seen.push('close');
      },
    });
    const composed = compose(source, sink);
    strictEqual(composed.readable, false);
    strictEqual(composed.writable, false);
    composed.on('error', (err) => {
      throw err;
    });
    await once(composed, 'close');
    deepStrictEqual(seen, ['op', 'close']);
  },
};

// Destroying a composition with a web tail tears the pipeline down: the
// composed stream reports the destroy error and closes, the node head is
// destroyed and the web tail's writable side is aborted with that same error
// — before anything was written, with the pump waiting on the head.
export const composeWebTailDestroyBeforeWrite = {
  async test() {
    if (!usingTsImpl) return;
    const head = new PassThrough();
    const tail = new TransformStream();
    const composed = compose(head, tail);
    const boom = new Error('torn down before the first write');
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    composed.destroy(boom);
    strictEqual(await errored, boom);
    await closed;
    strictEqual(composed.destroyed, true);
    strictEqual(head.destroyed, true);
    await rejects(promises.finished(tail.writable), (err) => err === boom);
  },
};

// The same with the pump parked on the web tail's backpressure: the
// transform's readable is never read, so its writable side stops accepting
// and the pump waits on the writer. Destroying aborts that writer and the
// composition completes its teardown.
export const composeWebTailDestroyUnderBackpressure = {
  async test() {
    if (!usingTsImpl) return;
    const head = new PassThrough();
    const tail = new TransformStream();
    const composed = compose(head, tail);
    composed.write('a');
    composed.write('b');
    composed.write('c');
    await scheduler.wait(5);
    const boom = new Error('torn down under backpressure');
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    composed.destroy(boom);
    strictEqual(await errored, boom);
    await closed;
    strictEqual(head.destroyed, true);
    await rejects(promises.finished(tail.writable), (err) => err === boom);
  },
};

// A web tail whose readable side has already closed while its writable side
// stays open (an accepted { readable, writable } pair): the pipeline is still
// waiting on the head, and destroying the composition tears it down all the
// same — the head is destroyed, the pair's writable aborted with the error.
export const composeWebTailClosedReadableDestroy = {
  async test() {
    if (!usingTsImpl) return;
    const head = new PassThrough();
    const aborts = [];
    const pair = {
      readable: new ReadableStream({
        start(controller) {
          controller.close();
        },
      }),
      writable: new WritableStream({
        abort(reason) {
          aborts.push(reason);
        },
      }),
    };
    const composed = compose(head, pair);
    // Let the composition observe the closed readable.
    await scheduler.wait(5);
    const boom = new Error('torn down behind a closed readable');
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    composed.destroy(boom);
    strictEqual(await errored, boom);
    await closed;
    strictEqual(head.destroyed, true);
    strictEqual(aborts.length, 1);
    strictEqual(aborts[0], boom);
  },
};

// A web tail yielding a chunk the composed stream cannot take (a view over
// a detached ArrayBuffer) fails the composition with the conversion's
// TypeError: the composed stream errors and closes, the head is destroyed.
// A writable head with a web tail needs the interop hooks (ledger #5), so
// this shape runs under TypeScript only; the readable-only head below
// covers the same failure on both implementations.
export const composeWebTailUnconvertibleChunkFails = {
  async test() {
    if (!usingTsImpl) return;
    const head = new PassThrough();
    const tail = new TransformStream({
      transform(chunk, controller) {
        const gone = new Uint8Array(chunk);
        structuredClone(gone.buffer, { transfer: [gone.buffer] });
        controller.enqueue(gone);
      },
    });
    const composed = compose(head, tail);
    composed.resume();
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    composed.write('x');
    const err = await errored;
    await closed;
    strictEqual(err.name, 'TypeError');
    strictEqual(composed.destroyed, true);
    strictEqual(head.destroyed, true);
  },
};

// The same failure behind a readable-only head (Readable.from), which
// needs no interop hook: on both implementations the composition errors
// with the conversion's TypeError, closes, and destroys the head.
export const composeWebTailUnconvertibleChunkFailsReadableHead = {
  async test() {
    const head = Readable.from([Buffer.from('x')]);
    const tail = new TransformStream({
      transform(chunk, controller) {
        const gone = new Uint8Array(chunk);
        structuredClone(gone.buffer, { transfer: [gone.buffer] });
        controller.enqueue(gone);
      },
    });
    const composed = compose(head, tail);
    composed.resume();
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    const err = await errored;
    await closed;
    strictEqual(err.name, 'TypeError');
    strictEqual(composed.destroyed, true);
    strictEqual(head.destroyed, true);
  },
};

// A web tail whose close() closes its readable side at once but settles
// later (an accepted { readable, writable } pair): the composition's
// writable side finishes only once that close has settled, so a
// composition consumed to its end still closes cleanly — with a consumer
// draining it, and readable-only.
export const composeWebTailDeferredCloseCompletesCleanly = {
  async test() {
    if (!usingTsImpl) return;
    const deferredClosePair = () => {
      let readableController;
      let finishClose;
      const pair = {
        readable: new ReadableStream({
          start(controller) {
            readableController = controller;
          },
        }),
        writable: new WritableStream({
          write(chunk) {
            readableController.enqueue(chunk);
          },
          close() {
            readableController.close();
            return new Promise((resolve) => {
              finishClose = resolve;
            });
          },
        }),
      };
      return { pair, finishClose: () => finishClose() };
    };

    // Consumed through 'data' (as with resume()): the automatic destroy then
    // waits for both sides, whereas async iteration destroys the stream as
    // soon as its readable side ends.
    const drain = (stream) => {
      let out = '';
      stream.on('data', (chunk) => (out += chunk));
      return once(stream, 'end').then(() => out);
    };

    const { pair, finishClose } = deferredClosePair();
    const composed = compose(new PassThrough(), pair);
    composed.on('error', (err) => {
      throw err;
    });
    const closed = once(composed, 'close');
    const output = drain(composed);
    composed.end('qr');
    strictEqual(await output, 'qr');
    // The tail's readable has closed and been drained; the pipeline still
    // awaits the deferred close, and so does the composition's finish.
    await scheduler.wait(10);
    strictEqual(composed.writableFinished, false);
    strictEqual(composed.destroyed, false);
    finishClose();
    await closed;
    strictEqual(composed.writableFinished, true);
    strictEqual(composed.errored, null);

    const readableOnly = deferredClosePair();
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('st'));
        controller.close();
      },
    });
    const readOnly = compose(source, readableOnly.pair);
    readOnly.on('error', (err) => {
      throw err;
    });
    let readOnlyClosed = false;
    readOnly.once('close', () => (readOnlyClosed = true));
    strictEqual(await drain(readOnly), 'st');
    // Both sides are done (there is no writable side): the automatic
    // destroy begins, but leaves the still-running pipeline to complete
    // and follows its outcome.
    await scheduler.wait(10);
    strictEqual(readOnlyClosed, false);
    readableOnly.finishClose();
    await once(readOnly, 'close');
    strictEqual(readOnly.errored, null);
  },
};

// A bare destroy() of a running composition reports an AbortError, as it
// does with a node tail.
export const composeWebTailBareDestroyIsAbortError = {
  async test() {
    if (!usingTsImpl) return;
    const composed = compose(new PassThrough(), new TransformStream());
    const errored = once(composed, 'error');
    const closed = once(composed, 'close');
    composed.destroy();
    strictEqual((await errored).name, 'AbortError');
    await closed;
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
