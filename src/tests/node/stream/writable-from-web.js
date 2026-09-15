// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.fromWeb(): a node Writable forwarding to a web WritableStream.
// The adapter takes a writer at construction; each _write() awaits
// writer.ready then writer.write(), _final() calls writer.close(), and
// _destroy() aborts (with an error) or closes (without one).

import { Writable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, throws } from 'node:assert';
import { usingTsImpl } from 'which-impl';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk written to the node Writable reaches the web sink's write().
export const fromWebWritesReachWebSink = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const ws = new WritableStream({
      write(chunk) {
        strictEqual(dec.decode(chunk), 'ok');
        resolve();
      },
    });
    const w = Writable.fromWeb(ws);
    strictEqual(w instanceof Writable, true);
    const written = Promise.withResolvers();
    w.write(enc.encode('ok'), (err) => {
      strictEqual(err, undefined);
      written.resolve();
    });
    await Promise.all([promise, written.promise]);
  },
};

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

// A web stream that errors on its own (its controller errors it) destroys
// the node Writable with that error, with no write ever issued.
export const fromWebWebErrorDestroysNodeWritable = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('controller error');
      const ws = new WritableStream({
        start(controller) {
          controller.error(boom);
        },
      });
      const w = Writable.fromWeb(ws);
      const [err] = await Promise.all([once(w, 'error'), once(w, 'close')]);
      strictEqual(err, boom);
      strictEqual(w.destroyed, true);
      strictEqual(w.errored, boom);
    });
  },
};

// A rejected sink write fails the node write callback with the error and
// errors the node Writable exactly once, leaving no unhandled rejection.
export const fromWebSinkRejectionErrorsNodeWritableOnce = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('sink rejected');
      const ws = new WritableStream({
        write() {
          return Promise.reject(boom);
        },
      });
      const w = Writable.fromWeb(ws);
      const errors = [];
      w.on('error', (err) => errors.push(err));
      const { promise, resolve } = Promise.withResolvers();
      w.write(enc.encode('x'), resolve);
      strictEqual(await promise, boom);
      await once(w, 'close');
      strictEqual(errors.length, 1);
      strictEqual(errors[0], boom);
      strictEqual(w.destroyed, true);
    });
  },
};

// A sink close() that rejects fails the node stream's finish with that
// error, again without unhandled rejections.
export const fromWebSinkCloseRejectionErrorsNodeWritable = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('close rejected');
      const ws = new WritableStream({
        close() {
          throw boom;
        },
      });
      const w = Writable.fromWeb(ws);
      let finished = false;
      w.on('finish', () => {
        finished = true;
      });
      w.end();
      const [err] = await Promise.all([once(w, 'error'), once(w, 'close')]);
      strictEqual(err, boom);
      strictEqual(finished, false);
      strictEqual(w.destroyed, true);
    });
  },
};

// Writes issued back to back, without waiting for the first to complete,
// are batched by the node Writable into _writev(); every batched chunk
// reaches the web sink as the chunk itself, in order.
export const fromWebBackToBackWritesDeliverChunks = {
  async test() {
    const seen = [];
    const ws = new WritableStream({
      write(chunk) {
        seen.push(chunk);
      },
    });
    const w = Writable.fromWeb(ws);
    w.write(enc.encode('one'));
    w.write(enc.encode('two'));
    w.write(enc.encode('three'));
    await new Promise((resolve) => w.end(resolve));
    strictEqual(seen.length, 3);
    for (const chunk of seen) {
      strictEqual(chunk instanceof Uint8Array, true);
    }
    strictEqual(seen.map((c) => dec.decode(c)).join(','), 'one,two,three');
  },
};

// Corked writes take the same _writev() path.
export const fromWebCorkedWritesDeliverChunks = {
  async test() {
    const seen = [];
    const ws = new WritableStream({
      write(chunk) {
        seen.push(chunk);
      },
    });
    const w = Writable.fromWeb(ws);
    w.cork();
    w.write('a');
    w.write('b');
    w.write('c');
    w.uncork();
    await new Promise((resolve) => w.end(resolve));
    strictEqual(seen.length, 3);
    strictEqual(seen.map((c) => dec.decode(c)).join(''), 'abc');
  },
};

// When a batched (_writev) write fails in the web sink, every callback in
// the batch receives the sink's error, the node Writable errors once with
// it, and no rejection is left unhandled.
export const fromWebBatchedWriteRejectionFailsCallbacks = {
  async test() {
    await withRejectionGuard(async () => {
      const boom = new Error('second chunk rejected');
      let writes = 0;
      const ws = new WritableStream({
        write() {
          if (++writes === 2) throw boom;
        },
      });
      const w = Writable.fromWeb(ws);
      const errors = [];
      w.on('error', (err) => errors.push(err));
      const closed = once(w, 'close');
      w.cork();
      const results = ['a', 'b', 'c'].map(
        (text) => new Promise((resolve) => w.write(text, resolve))
      );
      w.uncork();
      const errs = await Promise.all(results);
      strictEqual(errs.length, 3);
      for (const err of errs) strictEqual(err, boom);
      await closed;
      strictEqual(errors.length, 1);
      strictEqual(errors[0], boom);
      strictEqual(w.destroyed, true);
    });
  },
};

// Anything that is not a WritableStream (by duck type: getWriter and abort
// functions, and not a node stream) is rejected with ERR_INVALID_ARG_TYPE.
export const fromWebRejectsNonWritableStream = {
  test() {
    for (const input of [{}, new ReadableStream(), new Writable(), null]) {
      throws(() => Writable.fromWeb(input), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message:
          /"writableStream" argument must be an instance of WritableStream/,
      });
    }
  },
};

// Options are validated before the writer is acquired, so a rejected call
// leaves the stream unlocked.
export const writableFromWebValidatesOptionsBeforeLocking = {
  test() {
    const ws = new WritableStream();
    throws(() => Writable.fromWeb(ws, 5), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options" argument must be of type object/,
    });
    throws(() => Writable.fromWeb(ws, { objectMode: 1 }), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options\.objectMode" property must be of type boolean/,
    });
    throws(() => Writable.fromWeb(ws, { decodeStrings: 'y' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options\.decodeStrings" property must be of type boolean/,
    });
    strictEqual(ws.locked, false);
  },
};

// The adapter takes a writer at construction and keeps it: the web stream is
// locked for as long as the Writable lives, and an already locked stream
// cannot be adapted.
export const writableFromWebLocksTheStream = {
  test() {
    const ws = new WritableStream();
    Writable.fromWeb(ws);
    strictEqual(ws.locked, true);
    const lockedMessage = usingTsImpl
      ? 'Cannot get a writer for a stream that is locked'
      : 'This WritableStream is currently locked to a writer.';
    throws(() => ws.getWriter(), { name: 'TypeError', message: lockedMessage });
    const locked = new WritableStream();
    locked.getWriter();
    throws(() => Writable.fromWeb(locked), {
      name: 'TypeError',
      message: lockedMessage,
    });
  },
};

// Chunks reach the web sink as the node Writable hands them to _write():
// strings decoded to Buffers (per the encoding argument), a Buffer by
// identity, a Uint8Array as a Buffer over the same memory.
export const fromWebChunksReachSinkAsNodeChunks = {
  async test() {
    const seen = [];
    const w = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          seen.push(chunk);
        },
      })
    );
    const buffer = Buffer.from('buf');
    const u8 = new Uint8Array([1, 2, 3]);
    for (const [chunk, encoding] of [
      ['héllo'],
      ['68656c6c6f', 'hex'],
      [buffer],
      [u8],
    ]) {
      await new Promise((resolve) => w.write(chunk, encoding, resolve));
    }
    strictEqual(seen.length, 4);
    strictEqual(Buffer.isBuffer(seen[0]), true);
    strictEqual(seen[0].toString(), 'héllo');
    strictEqual(seen[1].toString(), 'hello');
    strictEqual(seen[2], buffer);
    strictEqual(Buffer.isBuffer(seen[3]), true);
    strictEqual(seen[3] === u8, false);
    strictEqual(seen[3].buffer, u8.buffer);
  },
};

// decodeStrings: false hands strings to the sink untouched; objectMode
// passes any value by identity.
export const fromWebDecodeStringsAndObjectMode = {
  async test() {
    const strings = [];
    const w = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          strings.push(chunk);
        },
      }),
      { decodeStrings: false }
    );
    await new Promise((resolve) => w.write('raw', resolve));
    deepStrictEqual(strings, ['raw']);

    const objects = [];
    const object = { id: 1 };
    const wo = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          objects.push(chunk);
        },
      }),
      { objectMode: true }
    );
    await new Promise((resolve) => wo.write(object, resolve));
    await new Promise((resolve) => wo.write(42, resolve));
    strictEqual(objects[0], object);
    strictEqual(objects[1], 42);
  },
};

// end() closes the web stream: the sink's close() runs before the node
// stream finishes.
export const fromWebEndClosesWebStream = {
  async test() {
    const events = [];
    const w = Writable.fromWeb(
      new WritableStream({
        close() {
          events.push('sink close');
        },
      })
    );
    w.on('finish', () => events.push('finish'));
    await new Promise((resolve) => w.end(resolve));
    deepStrictEqual(events, ['sink close', 'finish']);
    strictEqual(w.writableFinished, true);
  },
};

// destroy(error) aborts the web stream with the error; destroy() without an
// error closes it instead.
export const fromWebDestroyAbortsOrClosesWebStream = {
  async test() {
    const record = () => {
      const events = [];
      const ws = new WritableStream({
        close() {
          events.push('close');
        },
        abort(reason) {
          events.push(`abort:${reason?.message}`);
        },
      });
      return { ws, events };
    };
    const withError = record();
    const w1 = Writable.fromWeb(withError.ws);
    w1.on('error', () => {});
    const closed1 = once(w1, 'close');
    w1.destroy(new Error('gone'));
    await closed1;
    deepStrictEqual(withError.events, ['abort:gone']);

    const withoutError = record();
    const w2 = Writable.fromWeb(withoutError.ws);
    const closed2 = once(w2, 'close');
    w2.destroy();
    await closed2;
    deepStrictEqual(withoutError.events, ['close']);
  },
};

// Each node write completes only when the web sink has accepted the chunk:
// a slow sink holds the node callbacks, and the sink sees the chunks in
// order.
export const fromWebWritesCompleteWhenSinkAccepts = {
  async test() {
    const pending = [];
    const seen = [];
    const w = Writable.fromWeb(
      new WritableStream({
        write(chunk) {
          seen.push(dec.decode(chunk));
          return new Promise((resolve) => pending.push(resolve));
        },
      })
    );
    const done = [];
    w.write('a', () => done.push('a'));
    w.write('b', () => done.push('b'));
    await scheduler.wait(5);
    deepStrictEqual(seen, ['a']);
    deepStrictEqual(done, []);
    pending.shift()();
    await scheduler.wait(5);
    deepStrictEqual(done, ['a']);
    deepStrictEqual(seen, ['a', 'b']);
    pending.shift()();
    await scheduler.wait(5);
    deepStrictEqual(done, ['a', 'b']);
  },
};
