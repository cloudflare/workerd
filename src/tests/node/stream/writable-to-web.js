// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.toWeb(): a web WritableStream whose sink forwards to a node
// Writable. Sink writes call the node write(), sink close() calls end(),
// sink abort() destroys, and a node-side error or 'drain' settles the
// corresponding web promises.

import { Writable, Readable, Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A Writable whose sink records chunks and completes each write
// immediately.
function recordingWritable(options = {}) {
  const chunks = [];
  const writable = new Writable({
    ...options,
    write(chunk, encoding, callback) {
      chunks.push(chunk);
      callback();
    },
  });
  return { writable, chunks };
}

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// A chunk written through the web writer reaches the node _write().
export const toWebWritesReachNodeSink = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const w = new Writable({
      write(chunk, encoding, callback) {
        strictEqual(dec.decode(chunk), 'ok');
        resolve();
        callback();
      },
    });
    const ws = Writable.toWeb(w);
    strictEqual(ws instanceof WritableStream, true);
    const writer = ws.getWriter();
    await Promise.all([writer.write(enc.encode('ok')), promise]);
  },
};

// writer.close() ends the node Writable and settles once it has finished:
// the close promise and writer.closed both resolve after 'finish'.
export const toWebCloseEndsNodeWritable = {
  async test() {
    const { writable, chunks } = recordingWritable();
    const writer = Writable.toWeb(writable).getWriter();
    await writer.write(enc.encode('a'));
    let finished = false;
    writable.on('finish', () => {
      finished = true;
    });
    const closing = writer.close();
    strictEqual(writable.writableEnded, true);
    await closing;
    strictEqual(finished, true);
    strictEqual(writable.writableFinished, true);
    await writer.closed;
    strictEqual(chunks.length, 1);
  },
};

// A pipeTo() into the adapted stream runs to completion: every chunk
// reaches the node sink and the pipe promise resolves after the node side
// finishes.
export const toWebPipeToCompletes = {
  async test() {
    const { writable, chunks } = recordingWritable();
    const source = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('one'));
        controller.enqueue(enc.encode('two'));
        controller.close();
      },
    });
    await source.pipeTo(Writable.toWeb(writable));
    strictEqual(writable.writableFinished, true);
    strictEqual(chunks.map((c) => dec.decode(c)).join(','), 'one,two');
  },
};

// A node-side write error errors the web stream with the same error
// instance. A sink that fails synchronously makes the node write() return
// false, so the pending web write is parked on the drain promise and then
// rejected with the error; writer.closed and later writes reject with it
// too.
export const toWebSyncNodeErrorRejectsPendingWrite = {
  async test() {
    const boom = new Error('sink boom');
    const writable = new Writable({
      write(chunk, encoding, callback) {
        callback(boom);
      },
    });
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    await rejects(writer.write(enc.encode('a')), (err) => err === boom);
    await rejects(writer.closed, (err) => err === boom);
    await rejects(writer.write(enc.encode('b')), (err) => err === boom);
    strictEqual(writable.destroyed, true);
  },
};

// A sink that fails asynchronously lets the web write settle first (the
// node write() returned true); the error then surfaces through
// writer.closed and rejects any later write.
export const toWebAsyncNodeErrorErrorsStream = {
  async test() {
    const boom = new Error('sink boom');
    const writable = new Writable({
      write(chunk, encoding, callback) {
        setTimeout(() => callback(boom), 1);
      },
    });
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    await writer.write(enc.encode('a'));
    await rejects(writer.closed, (err) => err === boom);
    await rejects(writer.write(enc.encode('b')), (err) => err === boom);
    strictEqual(writable.destroyed, true);
  },
};

// An error from the node _final() rejects the pending close() and
// writer.closed with that error.
export const toWebFinalErrorRejectsClose = {
  async test() {
    const boom = new Error('final boom');
    const writable = new Writable({
      write(chunk, encoding, callback) {
        callback();
      },
      final(callback) {
        callback(boom);
      },
    });
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    await rejects(writer.close(), (err) => err === boom);
    await rejects(writer.closed, (err) => err === boom);
  },
};

// The node side ending on its own (end() called directly, not through the
// writer) is a premature finish from the web stream's point of view: the
// stream errors with an AbortError.
export const toWebNodeEndWithoutCloseAbortsStream = {
  async test() {
    const { writable } = recordingWritable();
    const writer = Writable.toWeb(writable).getWriter();
    writable.end();
    await once(writable, 'finish');
    await rejects(writer.closed, { name: 'AbortError', code: 'ABORT_ERR' });
    await rejects(writer.write(enc.encode('late')), {
      name: 'AbortError',
      code: 'ABORT_ERR',
    });
  },
};

// The node side being destroyed without an error surfaces as an AbortError
// whose cause is the premature-close error.
export const toWebNodeDestroyBecomesAbortError = {
  async test() {
    const { writable } = recordingWritable();
    const writer = Writable.toWeb(writable).getWriter();
    writable.destroy();
    await once(writable, 'close');
    await rejects(writer.closed, (err) => {
      strictEqual(err.name, 'AbortError');
      strictEqual(err.code, 'ABORT_ERR');
      strictEqual(err.cause?.code, 'ERR_STREAM_PREMATURE_CLOSE');
      return true;
    });
  },
};

// The node side being destroyed with an error errors the web stream with
// that same error instance.
export const toWebNodeDestroyWithErrorErrorsStream = {
  async test() {
    const { writable } = recordingWritable();
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    const boom = new Error('destroyed');
    writable.destroy(boom);
    await rejects(writer.closed, (err) => err === boom);
  },
};

// writer.abort(reason) destroys the node Writable with that reason: abort()
// resolves, and the node stream reports destroyed and errored with the
// reason, emitting 'error' then 'close'.
export const toWebAbortDestroysNodeWritable = {
  async test() {
    const { writable } = recordingWritable();
    const events = [];
    writable.on('error', (err) => events.push(['error', err]));
    writable.on('close', () => events.push(['close']));
    const writer = Writable.toWeb(writable).getWriter();
    const closed = once(writable, 'close');
    const reason = new Error('abandoned');
    await writer.abort(reason);
    await closed;
    strictEqual(writable.destroyed, true);
    strictEqual(writable.errored, reason);
    strictEqual(events.length, 2);
    strictEqual(events[0][0], 'error');
    strictEqual(events[0][1], reason);
    strictEqual(events[1][0], 'close');
  },
};

// Aborting without a reason destroys the node Writable with an AbortError.
export const toWebAbortWithoutReasonDestroysWithAbortError = {
  async test() {
    const { writable } = recordingWritable();
    writable.on('error', () => {});
    const writer = Writable.toWeb(writable).getWriter();
    const closed = once(writable, 'close');
    await writer.abort();
    await closed;
    strictEqual(writable.destroyed, true);
    strictEqual(writable.errored?.name, 'AbortError');
    strictEqual(writable.errored?.code, 'ABORT_ERR');
  },
};

// Anything without write() and on() functions is rejected with
// ERR_INVALID_ARG_TYPE: a Readable (which has on() but no write()), a plain
// object, and null all fail the same way.
export const toWebRejectsNonWritable = {
  test() {
    for (const input of [new Readable(), {}, null, undefined, 'text']) {
      throws(() => Writable.toWeb(input), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message: /"streamWritable" argument must be an stream\.Writable/,
      });
    }
  },
};

// The type check is a duck check (write and on functions), so a
// Writable-shaped object that is not a node stream is accepted; but since it
// is not a writable node stream either, the result is a stream that is
// already closed.
export const toWebDuckTypedInputYieldsClosedStream = {
  async test() {
    let writes = 0;
    const duck = {
      write() {
        writes++;
        return true;
      },
      on() {},
    };
    const writer = Writable.toWeb(duck).getWriter();
    await writer.closed;
    await rejects(writer.write('x'), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot write to a stream that is closing or closed'
        : 'This WritableStream has been closed.',
    });
    strictEqual(writes, 0);
  },
};

// A Writable that is already destroyed or ended, or a Duplex created without
// a writable side, yields a stream that is already closed.
export const toWebUnwritableSourceYieldsClosedStream = {
  async test() {
    const destroyed = recordingWritable().writable;
    destroyed.destroy();
    await once(destroyed, 'close');

    const ended = recordingWritable().writable;
    ended.end();
    await once(ended, 'finish');

    const halfDuplex = new Duplex({ writable: false, read() {} });

    for (const source of [destroyed, ended, halfDuplex]) {
      const ws = Writable.toWeb(source);
      strictEqual(ws.locked, false);
      const writer = ws.getWriter();
      await writer.closed;
      await rejects(writer.write(enc.encode('x')), { name: 'TypeError' });
    }
  },
};

// The queuing strategy follows the node stream: a byte-mode Writable gets a
// strategy with its writableHighWaterMark and the default size of 1 per
// chunk, an objectMode Writable a CountQueuingStrategy with its (chunk
// count) writableHighWaterMark.
export const toWebStrategyFollowsWritable = {
  test() {
    const bytes = recordingWritable({ highWaterMark: 5 }).writable;
    const bytesWriter = Writable.toWeb(bytes).getWriter();
    strictEqual(bytesWriter.desiredSize, 5);
    const objects = recordingWritable({ objectMode: true }).writable;
    const objectsWriter = Writable.toWeb(objects).getWriter();
    strictEqual(objectsWriter.desiredSize, objects.writableHighWaterMark);
    const defaults = recordingWritable().writable;
    strictEqual(
      Writable.toWeb(defaults).getWriter().desiredSize,
      defaults.writableHighWaterMark
    );
  },
};

// Backpressure crosses the adapter through 'drain': a web write whose node
// write() returns false stays pending until the node side drains, and
// writes queued behind it wait their turn.
export const toWebBackpressureFollowsDrain = {
  async test() {
    const callbacks = [];
    const writable = new Writable({
      highWaterMark: 1,
      write(chunk, encoding, callback) {
        callbacks.push(callback);
      },
    });
    const writer = Writable.toWeb(writable).getWriter();
    const settled = [];
    const first = writer
      .write(enc.encode('1'))
      .then(() => settled.push('first'));
    await scheduler.wait(0);
    strictEqual(writable.writableNeedDrain, true);
    strictEqual(writer.desiredSize, 0);
    deepStrictEqual(settled, []);
    const second = writer
      .write(enc.encode('2'))
      .then(() => settled.push('second'));
    await scheduler.wait(0);
    strictEqual(writer.desiredSize, -1);
    deepStrictEqual(settled, []);
    strictEqual(callbacks.length, 1);

    callbacks.shift()();
    await first;
    deepStrictEqual(settled, ['first']);
    await scheduler.wait(0);
    strictEqual(callbacks.length, 1);

    callbacks.shift()();
    await second;
    deepStrictEqual(settled, ['first', 'second']);
    await writer.ready;
    strictEqual(writer.desiredSize, 1);
  },
};

// Chunks reach the node sink the way Writable.prototype.write() would
// deliver them: a Buffer by identity, a Uint8Array as a Buffer over the same
// memory, a string as its UTF-8 bytes; in objectMode everything by identity.
export const toWebChunksReachSinkAsNodeWrites = {
  async test() {
    const { writable, chunks } = recordingWritable();
    const writer = Writable.toWeb(writable).getWriter();
    const buffer = Buffer.from('buf');
    const u8 = new Uint8Array([1, 2, 3]);
    await writer.write(buffer);
    await writer.write(u8);
    await writer.write('str');
    strictEqual(chunks[0], buffer);
    strictEqual(Buffer.isBuffer(chunks[1]), true);
    strictEqual(chunks[1] === u8, false);
    strictEqual(chunks[1].buffer, u8.buffer);
    strictEqual(chunks[2].toString(), 'str');

    const objects = recordingWritable({ objectMode: true });
    const objectWriter = Writable.toWeb(objects.writable).getWriter();
    const object = { id: 1 };
    await objectWriter.write(object);
    await objectWriter.write(u8);
    strictEqual(objects.chunks[0], object);
    strictEqual(objects.chunks[1], u8);
  },
};
