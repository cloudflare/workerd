// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Duplex.fromWeb(): a node Duplex over a { readable, writable } pair of web
// streams, taking a reader and a writer at construction.

import { Duplex, Readable, Writable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Collects unhandled promise rejections while a test runs; the adapter's
// error paths must not leak any.
async function withRejectionGuard(fn) {
  const unhandled = [];
  const onUnhandled = (event) => {
    unhandled.push(event.reason);
    event.preventDefault();
  };
  globalThis.addEventListener('unhandledrejection', onUnhandled);
  try {
    await fn();
    await scheduler.wait(5);
  } finally {
    globalThis.removeEventListener('unhandledrejection', onUnhandled);
  }
  strictEqual(unhandled.length, 0, `unhandled: ${unhandled.join(', ')}`);
}

// Data written to the duplex reaches the web writable's sink; chunks from
// the web readable surface as 'data'.
export const fromWebPairRoundTrip = {
  async test() {
    const dataToRead = Buffer.from('hello');
    const dataToWrite = Buffer.from('world');
    const sinkWrote = Promise.withResolvers();

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(dataToRead);
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        strictEqual(chunk, dataToWrite);
        sinkWrote.resolve();
      },
    });

    const duplex = Duplex.fromWeb({ readable, writable });
    strictEqual(duplex instanceof Duplex, true);

    duplex.write(dataToWrite);
    const read = Promise.withResolvers();
    duplex.once('data', (chunk) => {
      strictEqual(chunk, dataToRead);
      read.resolve();
    });
    await Promise.all([read.promise, sinkWrote.promise]);
  },
};

// With objectMode and an encoding, string chunks pass through both
// directions untouched.
export const fromWebObjectModeStrings = {
  async test() {
    const dataToRead = 'hello';
    const dataToWrite = 'world';
    const sinkWrote = Promise.withResolvers();

    const readable = new ReadableStream({
      start(controller) {
        controller.enqueue(dataToRead);
      },
    });
    const writable = new WritableStream({
      write(chunk) {
        strictEqual(chunk, dataToWrite);
        sinkWrote.resolve();
      },
    });

    const duplex = Duplex.fromWeb(
      { readable, writable },
      { encoding: 'utf8', objectMode: true }
    );

    duplex.write(dataToWrite);
    const read = Promise.withResolvers();
    duplex.once('data', (chunk) => {
      strictEqual(chunk, dataToRead);
      read.resolve();
    });
    await Promise.all([read.promise, sinkWrote.promise]);
  },
};

// The pair is validated with instanceof before either lock is taken: a
// non-object pair, or a half that is missing, a plain object, a node stream,
// or the other kind of web stream, is ERR_INVALID_ARG_TYPE, and the valid
// half is left unlocked.
export const fromWebPairRejectsNonStreamPair = {
  test() {
    for (const pair of [5, null]) {
      throws(() => Duplex.fromWeb(pair), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message: /"pair" argument must be of type object/,
      });
    }
    const rs = new ReadableStream();
    const ws = new WritableStream();
    for (const readable of [undefined, {}, new Readable(), ws]) {
      throws(() => Duplex.fromWeb({ readable, writable: ws }), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message:
          /"pair\.readable" property must be an instance of ReadableStream/,
      });
    }
    for (const writable of [undefined, {}, new Writable(), rs]) {
      throws(() => Duplex.fromWeb({ readable: rs, writable }), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message:
          /"pair\.writable" property must be an instance of WritableStream/,
      });
    }
    strictEqual(rs.locked, false);
    strictEqual(ws.locked, false);
  },
};

// The adapter takes the writer and then the reader. A pair whose readable
// is already locked throws that lock error (ledger #1) out of fromWeb, with
// the writable left locked to a writer nobody holds.
export const fromWebPairLockedReadableLeavesWriterLocked = {
  test() {
    const readable = new ReadableStream();
    const writable = new WritableStream();
    readable.getReader();
    throws(() => Duplex.fromWeb({ readable, writable }), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot get a reader for a stream that is locked'
        : 'This ReadableStream is currently locked to a reader.',
    });
    strictEqual(writable.locked, true);
  },
};

// Corked writes are batched through _writev() and reach the web sink as
// the chunks themselves, in order.
export const fromWebPairCorkedWritesDeliverChunks = {
  async test() {
    const seen = [];
    const duplex = Duplex.fromWeb({
      readable: new ReadableStream(),
      writable: new WritableStream({
        write(chunk) {
          seen.push(chunk);
        },
      }),
    });
    duplex.cork();
    duplex.write('a');
    duplex.write('b');
    duplex.write('c');
    duplex.uncork();
    await new Promise((resolve) => duplex.end(resolve));
    strictEqual(seen.length, 3);
    for (const chunk of seen) {
      strictEqual(chunk instanceof Uint8Array, true);
    }
    strictEqual(Buffer.concat(seen).toString(), 'abc');
  },
};

// A failing batched write fails every callback in the batch with the sink's
// error and errors the duplex once, with no unhandled rejection.
export const fromWebPairBatchedWriteRejectionFailsCallbacks = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('second chunk rejected');
      let writes = 0;
      const duplex = Duplex.fromWeb({
        readable: new ReadableStream(),
        writable: new WritableStream({
          write() {
            if (++writes === 2) throw boom;
          },
        }),
      });
      const errors = [];
      duplex.on('error', (err) => errors.push(err));
      const closed = once(duplex, 'close');
      duplex.cork();
      const results = ['a', 'b', 'c'].map(
        (text) => new Promise((resolve) => duplex.write(text, resolve))
      );
      duplex.uncork();
      const errs = await Promise.all(results);
      for (const err of errs) strictEqual(err, boom);
      await closed;
      strictEqual(errors.length, 1);
      strictEqual(errors[0], boom);
      strictEqual(duplex.destroyed, true);
    });
  },
};

// A web readable that is already errored destroys the duplex with that
// error. The web writable is left untouched: its abort() is not invoked,
// because the adapter records both halves as closed before destroying.
export const fromWebPairErroredReadableDestroysDuplex = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('readable errored');
      let aborted = false;
      const duplex = Duplex.fromWeb({
        readable: new ReadableStream({
          start(controller) {
            controller.error(boom);
          },
        }),
        writable: new WritableStream({
          abort() {
            aborted = true;
          },
        }),
      });
      const [err] = await Promise.all([
        once(duplex, 'error'),
        once(duplex, 'close'),
      ]);
      strictEqual(err, boom);
      strictEqual(duplex.destroyed, true);
      strictEqual(aborted, false);
    });
  },
};

// A web readable that errors after delivering data surfaces through the
// pending read: the duplex errors with that error and is destroyed.
export const fromWebPairLaterReadableErrorDestroysDuplex = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('readable errored later');
      let controller;
      const duplex = Duplex.fromWeb({
        readable: new ReadableStream({
          start(c) {
            controller = c;
          },
        }),
        writable: new WritableStream(),
      });
      const chunks = [];
      duplex.on('data', (chunk) => chunks.push(chunk));
      const errored = once(duplex, 'error');
      const closed = once(duplex, 'close');
      controller.enqueue(new Uint8Array([1, 2, 3]));
      await once(duplex, 'data');
      controller.error(boom);
      strictEqual(await errored, boom);
      await closed;
      strictEqual(chunks.length, 1);
      strictEqual(duplex.destroyed, true);
    });
  },
};

// A web writable that is already errored destroys the duplex with that
// error, with no write ever issued. The web readable is left locked and
// uncancelled: its cancel() is not invoked.
export const fromWebPairErroredWritableDestroysDuplex = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('writable errored');
      let cancelled = false;
      const readable = new ReadableStream({
        cancel() {
          cancelled = true;
        },
      });
      const duplex = Duplex.fromWeb({
        readable,
        writable: new WritableStream({
          start(controller) {
            controller.error(boom);
          },
        }),
      });
      const [err] = await Promise.all([
        once(duplex, 'error'),
        once(duplex, 'close'),
      ]);
      strictEqual(err, boom);
      strictEqual(duplex.destroyed, true);
      strictEqual(cancelled, false);
      strictEqual(readable.locked, true);
    });
  },
};

// allowHalfOpen defaults to false: once the web readable closes and the
// duplex has emitted 'end', the duplex ends its own writable side on the
// next tick, which closes the web writable cleanly. Consumed through events;
// the for await form below destroys the duplex before that end() runs.
export const fromWebPairReadableEofEndsWritable = {
  async test() {
    await withRejectionGuard(async () => {
      const seen = [];
      const duplex = Duplex.fromWeb({
        readable: new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([1]));
            controller.close();
          },
        }),
        writable: new WritableStream({
          close() {
            seen.push('close');
          },
          abort(reason) {
            seen.push(`abort:${reason?.name}`);
          },
        }),
      });
      strictEqual(duplex.allowHalfOpen, false);
      const ended = once(duplex, 'end');
      const finished = once(duplex, 'finish');
      const closed = once(duplex, 'close');
      const chunks = [];
      duplex.on('data', (chunk) => chunks.push(chunk));
      await ended;
      strictEqual(chunks.length, 1);
      await finished;
      await closed;
      strictEqual(duplex.writableFinished, true);
      strictEqual(duplex.destroyed, true);
      deepStrictEqual(seen, ['close']);
    });
  },
};

// Consuming the duplex to completion with for await destroys it once the
// readable side ends. The writable half has not finished by then, so the
// destroy carries an AbortError and the web writable is aborted with it;
// nothing is left as an unhandled rejection.
export const fromWebPairIterationToCompletionIsClean = {
  async test() {
    await withRejectionGuard(async () => {
      const seen = [];
      const duplex = Duplex.fromWeb({
        readable: new ReadableStream({
          start(controller) {
            controller.enqueue(new Uint8Array([1]));
            controller.enqueue(new Uint8Array([2]));
            controller.close();
          },
        }),
        writable: new WritableStream({
          close() {
            seen.push('close');
          },
          abort(reason) {
            seen.push(`abort:${reason?.name}:${reason?.code}`);
          },
        }),
      });
      const chunks = [];
      for await (const chunk of duplex) chunks.push(chunk);
      strictEqual(chunks.length, 2);
      strictEqual(duplex.destroyed, true);
      strictEqual(seen.join(','), 'abort:AbortError:ABORT_ERR');
    });
  },
};

// Destroying the duplex aborts the web writable and cancels the web
// readable, both with the destroy reason; a bare destroy() passes null to
// both (where Writable.fromWeb closes its stream instead).
export const fromWebPairDestroyAbortsWriterAndCancelsReader = {
  async test() {
    await withRejectionGuard(async () => {
      const record = () => {
        const seen = {};
        const pair = {
          readable: new ReadableStream({
            cancel(reason) {
              seen.cancel = reason;
            },
          }),
          writable: new WritableStream({
            close() {
              seen.close = true;
            },
            abort(reason) {
              seen.abort = reason;
            },
          }),
        };
        return { pair, seen };
      };

      const withReason = record();
      const d1 = Duplex.fromWeb(withReason.pair);
      d1.on('error', () => {});
      const closed1 = once(d1, 'close');
      const boom = new Error('going away');
      d1.destroy(boom);
      await closed1;
      strictEqual(withReason.seen.abort, boom);
      strictEqual(withReason.seen.cancel, boom);
      strictEqual(withReason.seen.close, undefined);

      const bare = record();
      const d2 = Duplex.fromWeb(bare.pair);
      const closed2 = once(d2, 'close');
      d2.destroy();
      await closed2;
      strictEqual('abort' in bare.seen, true);
      strictEqual(bare.seen.abort, null);
      strictEqual('cancel' in bare.seen, true);
      strictEqual(bare.seen.cancel, null);
      strictEqual(bare.seen.close, undefined);
    });
  },
};
