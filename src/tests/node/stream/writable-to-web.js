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
import { once, withUncaughtGuard } from 'helpers';

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

// Collects uncaught errors and unhandled rejections while fn runs; the
// adapter must leak neither.
// The node side ended directly and the writer closed before it finishes:
// close() waits for the finish (a slow _final()) rather than resolving at
// once, and the stream closes cleanly — nothing escapes the adapter.
export const toWebCloseAfterNodeEndWaitsForFinish = {
  async test() {
    await withUncaughtGuard(async () => {
      let finishFinal;
      const writable = new Writable({
        write(chunk, encoding, callback) {
          callback();
        },
        final(callback) {
          finishFinal = callback;
        },
      });
      const writer = Writable.toWeb(writable).getWriter();
      writable.end();
      let closeSettled = false;
      const closing = writer.close().then(() => (closeSettled = true));
      await scheduler.wait(10);
      strictEqual(closeSettled, false);
      strictEqual(writable.writableFinished, false);
      finishFinal();
      await closing;
      strictEqual(writable.writableFinished, true);
      await writer.closed;
    });
  },
};

// The same with a _final() that fails after the writer's close(): close()
// and writer.closed reject with that error, the node side reports it, and
// nothing escapes the adapter. A _final() failing synchronously in the
// direct end() is reported the same way.
export const toWebCloseAfterNodeEndRejectsWithFinalError = {
  async test() {
    await withUncaughtGuard(async () => {
      const boom = new Error('late final failed');
      let failFinal;
      const writable = new Writable({
        write(chunk, encoding, callback) {
          callback();
        },
        final(callback) {
          failFinal = () => callback(boom);
        },
      });
      const errored = once(writable, 'error');
      const writer = Writable.toWeb(writable).getWriter();
      writable.end();
      const closing = writer.close();
      await scheduler.wait(10);
      failFinal();
      await rejects(closing, (err) => err === boom);
      await rejects(writer.closed, (err) => err === boom);
      strictEqual(await errored, boom);
    });
    await withUncaughtGuard(async () => {
      const boom = new Error('final failed');
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
      writable.end();
      await rejects(writer.close(), (err) => err === boom);
      await rejects(writer.closed, (err) => err === boom);
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

// A duck that also claims `writable: true` is a live legacy writable:
// end-of-stream subscribes to its 'end'/'close'/'finish'/'error' through
// plain on() calls (whatever on() returns), and the adapter takes the duck
// at its word on backpressure — a `writableNeedDrain` that stays true keeps
// the web write pending without even calling write(); a truthy non-boolean
// return from write() counts as accepted, a falsy one as backpressure that
// only a 'drain' the duck never emits would release.
export const toWebLiveDuckIsTakenAtItsWord = {
  async test() {
    const liveDuck = (overrides) => ({
      writable: true,
      writableNeedDrain: false,
      writes: [],
      write(chunk) {
        this.writes.push(chunk);
        return true;
      },
      // A legacy emitter's on() need not return anything.
      on() {},
      once() {},
      removeListener() {},
      end() {},
      ...overrides,
    });
    const outcome = (promise) =>
      Promise.race([
        promise.then(
          () => 'settled',
          () => 'rejected'
        ),
        scheduler.wait(50).then(() => 'pending'),
      ]);

    const liar = liveDuck({ writableNeedDrain: true });
    const liarWriter = Writable.toWeb(liar).getWriter();
    strictEqual(await outcome(liarWriter.write(enc.encode('x'))), 'pending');
    strictEqual(liar.writes.length, 0);

    const truthy = liveDuck({
      write(chunk) {
        this.writes.push(chunk);
        return 'yes';
      },
    });
    const truthyWriter = Writable.toWeb(truthy).getWriter();
    strictEqual(await outcome(truthyWriter.write(enc.encode('x'))), 'settled');
    strictEqual(truthy.writes.length, 1);

    const falsy = liveDuck({
      write(chunk) {
        this.writes.push(chunk);
        return 0;
      },
    });
    const falsyWriter = Writable.toWeb(falsy).getWriter();
    strictEqual(await outcome(falsyWriter.write(enc.encode('x'))), 'pending');
    strictEqual(falsy.writes.length, 1);
  },
};

// A writable misreporting its writableHighWaterMark (NaN, negative) makes
// the web stream's construction throw — a RangeError under TypeScript, the
// C++ implementation's TypeError (streams readable ledger #3). The web
// stream is constructed before the writable is touched, so the throw
// leaves it as it was: no listeners, and ending it afterwards finishes
// quietly rather than tripping a dangling end-of-stream bridge.
export const toWebInvalidHighWaterMarkLeavesWritableUntouched = {
  async test() {
    await withUncaughtGuard(async () => {
      for (const value of [NaN, -1]) {
        const { writable, chunks } = recordingWritable();
        Object.defineProperty(writable, 'writableHighWaterMark', {
          get: () => value,
        });
        throws(() => Writable.toWeb(writable), {
          name: usingTsImpl ? 'RangeError' : 'TypeError',
        });
        for (const event of ['drain', 'finish', 'error', 'close']) {
          strictEqual(writable.listenerCount(event), 0, event);
        }
        writable.end(enc.encode('still fine'));
        await once(writable, 'finish');
        deepStrictEqual(
          chunks.map((chunk) => dec.decode(chunk)),
          ['still fine']
        );
      }
    });
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
