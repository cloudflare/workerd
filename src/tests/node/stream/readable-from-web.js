// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.fromWeb(): a node Readable pulling from a web ReadableStream. The
// adapter takes a default reader at construction and issues one read() per
// _read() call, pushing each chunk and translating done into EOF and reader
// errors into destroy().

import { Readable } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects, throws, ok } from 'node:assert';
import { usingTsImpl } from 'which-impl';

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

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Anything that is not a ReadableStream (by duck type: pipeThrough,
// getReader, and cancel functions, and not a node stream) is rejected with
// ERR_INVALID_ARG_TYPE.
export const fromWebRejectsNonReadableStream = {
  test() {
    for (const input of [{}, new WritableStream(), new Readable(), null]) {
      throws(() => Readable.fromWeb(input), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message:
          /"readableStream" argument must be an instance of ReadableStream/,
      });
    }
  },
};

// Options are validated before the reader is acquired, so a rejected call
// leaves the stream unlocked.
export const fromWebValidatesOptionsBeforeLocking = {
  test() {
    const rs = new ReadableStream();
    throws(() => Readable.fromWeb(rs, 5), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options" argument must be of type object/,
    });
    throws(() => Readable.fromWeb(rs, { encoding: 'nope' }), {
      code: 'ERR_INVALID_ARG_VALUE',
      message: /'options\.encoding' is invalid\. Received 'nope'/,
    });
    throws(() => Readable.fromWeb(rs, { objectMode: 'x' }), {
      code: 'ERR_INVALID_ARG_TYPE',
      message: /"options\.objectMode" property must be of type boolean/,
    });
    strictEqual(rs.locked, false);
  },
};

// The adapter takes a default reader at construction and keeps it: the web
// stream is locked for as long as the Readable lives.
export const fromWebLocksTheStream = {
  test() {
    const rs = new ReadableStream();
    Readable.fromWeb(rs);
    strictEqual(rs.locked, true);
    throws(() => rs.getReader(), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot get a reader for a stream that is locked'
        : 'This ReadableStream is currently locked to a reader.',
    });
  },
};

// A stream that is already locked cannot be adapted; the lock error escapes
// fromWeb synchronously.
export const fromWebLockedInputThrows = {
  test() {
    const rs = new ReadableStream();
    rs.getReader();
    throws(() => Readable.fromWeb(rs), {
      name: 'TypeError',
      message: usingTsImpl
        ? 'Cannot get a reader for a stream that is locked'
        : 'This ReadableStream is currently locked to a reader.',
    });
  },
};

// Reads are issued only on demand: with the web stream's own queue disabled
// (highWaterMark 0) the source is not pulled until the Readable is read.
export const fromWebPullsOnlyOnDemand = {
  async test() {
    let pulls = 0;
    const rs = new ReadableStream(
      {
        pull(controller) {
          pulls++;
          controller.enqueue(enc.encode(`chunk${pulls}`));
        },
      },
      { highWaterMark: 0 }
    );
    const r = Readable.fromWeb(rs);
    await scheduler.wait(5);
    strictEqual(pulls, 0);
    const chunk = await once(r, 'data');
    strictEqual(dec.decode(chunk), 'chunk1');
    r.pause();
    ok(pulls >= 1);
  },
};

// The web stream closing ends the Readable: 'end' then 'close'.
export const fromWebCloseEmitsEndThenClose = {
  async test() {
    const rs = new ReadableStream({
      start(controller) {
        controller.enqueue(enc.encode('a'));
        controller.close();
      },
    });
    const r = Readable.fromWeb(rs);
    const events = [];
    r.on('data', () => events.push('data'));
    r.on('end', () => events.push('end'));
    const closed = once(r, 'close');
    await closed;
    deepStrictEqual(events, ['data', 'end']);
    strictEqual(r.readableEnded, true);
    strictEqual(r.destroyed, true);
  },
};

// A web stream error with no read in flight reaches the Readable through the
// reader's closed promise: it is destroyed with that error instance.
export const fromWebErrorWithoutPendingReadDestroys = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const r = Readable.fromWeb(rs);
    const boom = new Error('errored idle');
    const errored = once(r, 'error');
    const closed = once(r, 'close');
    controller.error(boom);
    strictEqual(await errored, boom);
    await closed;
    strictEqual(r.destroyed, true);
    strictEqual(r.errored, boom);
  },
};

// A web stream error while a read is in flight rejects that read and
// destroys the Readable with the error.
export const fromWebErrorWithPendingReadDestroys = {
  async test() {
    let controller;
    const rs = new ReadableStream({
      start(c) {
        controller = c;
      },
    });
    const r = Readable.fromWeb(rs);
    const chunks = [];
    r.on('data', (chunk) => chunks.push(chunk));
    controller.enqueue(enc.encode('a'));
    await once(r, 'data');
    const boom = new Error('errored mid-stream');
    const errored = once(r, 'error');
    controller.error(boom);
    strictEqual(await errored, boom);
    strictEqual(chunks.length, 1);
    strictEqual(r.destroyed, true);
  },
};

// Destroying the Readable cancels the web stream with the destroy reason;
// destroy() without a reason cancels with null.
export const fromWebDestroyCancelsWebStream = {
  async test() {
    const cancels = [];
    const make = () =>
      new ReadableStream({
        cancel(reason) {
          cancels.push(reason);
        },
      });
    const withReason = Readable.fromWeb(make());
    withReason.on('error', () => {});
    const reason = new Error('going away');
    withReason.destroy(reason);
    await once(withReason, 'close');
    const withoutReason = Readable.fromWeb(make());
    withoutReason.destroy();
    await once(withoutReason, 'close');
    strictEqual(cancels.length, 2);
    strictEqual(cancels[0], reason);
    strictEqual(cancels[1], null);
  },
};

// Once the web stream has closed, destroying the Readable does not cancel
// it again.
export const fromWebDestroyAfterCloseSkipsCancel = {
  async test() {
    let cancelled = false;
    const rs = new ReadableStream({
      start(controller) {
        controller.close();
      },
      cancel() {
        cancelled = true;
      },
    });
    const r = Readable.fromWeb(rs);
    r.resume();
    await once(r, 'end');
    r.destroy();
    await scheduler.wait(5);
    strictEqual(cancelled, false);
  },
};

// The encoding option decodes byte chunks into strings.
export const fromWebEncodingOption = {
  async test() {
    const make = () =>
      new ReadableStream({
        start(controller) {
          controller.enqueue(enc.encode('héllo'));
          controller.close();
        },
      });
    const utf8 = [];
    for await (const chunk of Readable.fromWeb(make(), { encoding: 'utf8' })) {
      utf8.push(chunk);
    }
    deepStrictEqual(utf8, ['héllo']);
    const hex = [];
    for await (const chunk of Readable.fromWeb(make(), { encoding: 'hex' })) {
      hex.push(chunk);
    }
    deepStrictEqual(hex, [Buffer.from('héllo').toString('hex')]);
  },
};

// Without objectMode the Readable accepts only byte-like chunks: strings
// are converted to Buffers and anything else errors the stream with
// ERR_INVALID_ARG_TYPE. With objectMode any value passes by identity.
export const fromWebObjectModeOption = {
  async test() {
    const stringSource = new ReadableStream({
      start(controller) {
        controller.enqueue('text');
        controller.close();
      },
    });
    const strings = [];
    for await (const chunk of Readable.fromWeb(stringSource)) {
      strings.push(chunk);
    }
    strictEqual(strings.length, 1);
    strictEqual(Buffer.isBuffer(strings[0]), true);
    strictEqual(strings[0].toString(), 'text');

    const numberSource = new ReadableStream({
      start(controller) {
        controller.enqueue(42);
      },
    });
    const bytes = Readable.fromWeb(numberSource);
    const errored = once(bytes, 'error');
    bytes.resume();
    const err = await errored;
    strictEqual(err.code, 'ERR_INVALID_ARG_TYPE');
    strictEqual(err.name, 'TypeError');

    const object = { id: 1 };
    const objectSource = new ReadableStream({
      start(controller) {
        controller.enqueue(object);
        controller.enqueue(42);
        controller.close();
      },
    });
    const objects = [];
    for await (const chunk of Readable.fromWeb(objectSource, {
      objectMode: true,
    })) {
      objects.push(chunk);
    }
    strictEqual(objects[0], object);
    strictEqual(objects[1], 42);
  },
};

// The highWaterMark option is the Readable's.
export const fromWebHighWaterMarkOption = {
  test() {
    const r = Readable.fromWeb(new ReadableStream(), { highWaterMark: 3 });
    strictEqual(r.readableHighWaterMark, 3);
  },
};

// An aborted signal destroys the Readable with an AbortError and cancels the
// web stream with that AbortError.
export const fromWebSignalOption = {
  async test() {
    let cancelReason;
    const rs = new ReadableStream({
      cancel(reason) {
        cancelReason = reason;
      },
    });
    const controller = new AbortController();
    const r = Readable.fromWeb(rs, { signal: controller.signal });
    const errored = once(r, 'error');
    const closed = once(r, 'close');
    controller.abort();
    const err = await errored;
    strictEqual(err.name, 'AbortError');
    strictEqual(err.code, 'ABORT_ERR');
    await closed;
    strictEqual(cancelReason, err);
  },
};
