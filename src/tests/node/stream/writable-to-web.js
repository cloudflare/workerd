// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Writable.toWeb(): a web WritableStream whose sink forwards to a node
// Writable. Sink writes call the node write(), sink close() calls end(),
// sink abort() destroys, and a node-side error or 'drain' settles the
// corresponding web promises.

import { Writable } from 'node:stream';
import { strictEqual, rejects } from 'node:assert';

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
