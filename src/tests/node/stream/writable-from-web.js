// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.fromWeb(): a node Writable forwarding to a web WritableStream.
// The adapter takes a writer at construction; each _write() awaits
// writer.ready then writer.write(), _final() calls writer.close(), and
// _destroy() aborts (with an error) or closes (without one).

import { Writable } from 'node:stream';
import { strictEqual } from 'node:assert';

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
