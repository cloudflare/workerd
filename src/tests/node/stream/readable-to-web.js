// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Readable.toWeb(): a node Readable driving a web ReadableStream. The
// adapter subscribes to 'data' and enqueues into the stream's controller,
// pausing the source whenever desiredSize drops to zero and resuming from
// pull().

import { Readable, Writable, Duplex } from 'node:stream';
import { Buffer } from 'node:buffer';
import { strictEqual, deepStrictEqual, rejects, throws } from 'node:assert';

const enc = new TextEncoder();
const dec = new TextDecoder();

// A chunk pushed by the source arrives at the web reader.
export const toWebDeliversPushedChunk = {
  async test() {
    const r = new Readable({
      read() {
        this.push(enc.encode('ok'));
      },
    });
    const rs = Readable.toWeb(r);
    strictEqual(rs instanceof ReadableStream, true);
    const reader = rs.getReader();
    const { value, done } = await reader.read();
    strictEqual(done, false);
    strictEqual(dec.decode(value), 'ok');
  },
};

function once(emitter, event) {
  return new Promise((resolve) => emitter.once(event, resolve));
}

// Canceling the web reader destroys the node source with the cancel reason:
// the reader's cancel() resolves, and the source reports destroyed and
// errored with that reason, emitting 'error' then 'close'.
export const toWebCancelDestroysSource = {
  async test() {
    const source = new Readable({ read() {} });
    const events = [];
    source.on('error', (err) => events.push(['error', err]));
    source.on('close', () => events.push(['close']));
    const reader = Readable.toWeb(source).getReader();
    const closed = once(source, 'close');
    const reason = new Error('no longer needed');
    await reader.cancel(reason);
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored, reason);
    strictEqual(events.length, 2);
    strictEqual(events[0][0], 'error');
    strictEqual(events[0][1], reason);
    strictEqual(events[1][0], 'close');
  },
};

// Canceling without a reason still destroys the source; the node stream is
// destroyed with an AbortError, as stream.destroy() does for a stream that
// has not finished.
export const toWebCancelWithoutReasonDestroysWithAbortError = {
  async test() {
    const source = new Readable({ read() {} });
    source.on('error', () => {});
    const reader = Readable.toWeb(source).getReader();
    const closed = once(source, 'close');
    await reader.cancel();
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored?.name, 'AbortError');
    strictEqual(source.errored?.code, 'ABORT_ERR');
  },
};

// A failing pipeTo() destination cancels the adapted stream, which destroys
// the node source with the destination's error.
export const toWebPipeToFailureDestroysSource = {
  async test() {
    const source = new Readable({
      read() {
        this.push(enc.encode('x'));
      },
    });
    source.on('error', () => {});
    const boom = new Error('destination failed');
    const destination = new WritableStream({
      write() {
        throw boom;
      },
    });
    const closed = once(source, 'close');
    await rejects(Readable.toWeb(source).pipeTo(destination), (err) => {
      return err === boom;
    });
    await closed;
    strictEqual(source.destroyed, true);
    strictEqual(source.errored, boom);
  },
};

// Anything without a Readable's _readableState is rejected with
// ERR_INVALID_ARG_TYPE; a Writable, a plain object, and null all fail the
// same way.
export const toWebRejectsNonReadable = {
  test() {
    for (const input of [new Writable(), {}, null, undefined, 'text']) {
      throws(() => Readable.toWeb(input), {
        name: 'TypeError',
        code: 'ERR_INVALID_ARG_TYPE',
        message: /"streamReadable" argument must be an stream\.Readable/,
      });
    }
  },
};

// Byte chunks are copied on the way in: the reader receives a plain
// Uint8Array (not a Buffer) over a fresh ArrayBuffer, so later mutation of
// the source's buffer is invisible. A pushed Uint8Array is copied too, as
// the Readable itself wraps it in a Buffer before the adapter sees it.
export const toWebCopiesByteChunks = {
  async test() {
    const source = Buffer.from('hello');
    const pushed = new Uint8Array([1, 2, 3]);
    const r = new Readable({
      read() {
        this.push(source);
        this.push(pushed);
        this.push(null);
      },
    });
    const reader = Readable.toWeb(r).getReader();
    const first = (await reader.read()).value;
    strictEqual(first.constructor, Uint8Array);
    strictEqual(Buffer.isBuffer(first), false);
    strictEqual(first.buffer === source.buffer, false);
    strictEqual(dec.decode(first), 'hello');
    source[0] = 0x58;
    strictEqual(first[0], 0x68);
    const second = (await reader.read()).value;
    strictEqual(second === pushed, false);
    deepStrictEqual([...second], [1, 2, 3]);
    strictEqual((await reader.read()).done, true);
  },
};

// In objectMode chunks pass through by identity, Buffers included.
export const toWebObjectModePassesChunksByIdentity = {
  async test() {
    const object = { id: 1 };
    const buffer = Buffer.from('raw');
    const r = new Readable({
      objectMode: true,
      read() {
        this.push(object);
        this.push(buffer);
        this.push(null);
      },
    });
    const reader = Readable.toWeb(r).getReader();
    strictEqual((await reader.read()).value, object);
    strictEqual((await reader.read()).value, buffer);
    strictEqual((await reader.read()).done, true);
  },
};

// The adapter pauses the source at construction and lets the web stream's
// pull() resume it; the queuing strategy is derived from the source, so an
// objectMode Readable counts chunks against its readableHighWaterMark. Once
// that many chunks sit unread in the web queue the source is paused again,
// and draining the queue resumes it.
export const toWebObjectModeBackpressureCountsChunks = {
  async test() {
    const r = new Readable({ objectMode: true, highWaterMark: 2, read() {} });
    const rs = Readable.toWeb(r);
    strictEqual(r.isPaused(), true);
    await scheduler.wait(0);
    strictEqual(r.isPaused(), false);
    r.push('a');
    await scheduler.wait(0);
    strictEqual(r.isPaused(), false);
    r.push('b');
    await scheduler.wait(0);
    strictEqual(r.isPaused(), true);
    r.push('c');
    r.push('d');
    await scheduler.wait(0);
    strictEqual(r.isPaused(), true);
    strictEqual(r.readableLength, 2);
    const reader = rs.getReader();
    const values = [];
    for (let i = 0; i < 4; i++) values.push((await reader.read()).value);
    deepStrictEqual(values, ['a', 'b', 'c', 'd']);
    await scheduler.wait(0);
    strictEqual(r.isPaused(), false);
    strictEqual(r.readableLength, 0);
  },
};

// A byte-mode Readable gets a ByteLengthQueuingStrategy: the pause point is
// reached by bytes, not chunk count.
export const toWebByteModeBackpressureCountsBytes = {
  async test() {
    const r = new Readable({ highWaterMark: 4, read() {} });
    Readable.toWeb(r);
    await scheduler.wait(0);
    r.push(Buffer.alloc(2));
    await scheduler.wait(0);
    strictEqual(r.isPaused(), false);
    r.push(Buffer.alloc(2));
    await scheduler.wait(0);
    strictEqual(r.isPaused(), true);

    const big = new Readable({ highWaterMark: 4, read() {} });
    Readable.toWeb(big);
    await scheduler.wait(0);
    big.push(Buffer.alloc(8));
    await scheduler.wait(0);
    strictEqual(big.isPaused(), true);
  },
};

// An explicit strategy replaces the derived one.
export const toWebExplicitStrategyOverridesDerived = {
  async test() {
    const r = new Readable({ highWaterMark: 100, read() {} });
    Readable.toWeb(r, {
      strategy: new CountQueuingStrategy({ highWaterMark: 1 }),
    });
    await scheduler.wait(0);
    r.push(Buffer.alloc(1));
    await scheduler.wait(0);
    strictEqual(r.isPaused(), true);
  },
};

// The source ending closes the web stream: reads resolve done and closed
// resolves.
export const toWebEndClosesStream = {
  async test() {
    const r = new Readable({
      read() {
        this.push(enc.encode('a'));
        this.push(null);
      },
    });
    const reader = Readable.toWeb(r).getReader();
    strictEqual(dec.decode((await reader.read()).value), 'a');
    const tail = await reader.read();
    strictEqual(tail.done, true);
    strictEqual(tail.value, undefined);
    await reader.closed;
  },
};

// The source being destroyed with an error errors the web stream with that
// same error instance.
export const toWebSourceErrorRejectsRead = {
  async test() {
    const r = new Readable({ read() {} });
    const reader = Readable.toWeb(r).getReader();
    const pending = reader.read();
    const boom = new Error('source boom');
    r.destroy(boom);
    await rejects(pending, (err) => err === boom);
    await rejects(reader.closed, (err) => err === boom);
  },
};

// The source being destroyed without an error is a premature close, which
// surfaces as an AbortError whose cause is the premature-close error.
export const toWebSourceDestroyBecomesAbortError = {
  async test() {
    const r = new Readable({ read() {} });
    const reader = Readable.toWeb(r).getReader();
    const pending = reader.read();
    r.destroy();
    for (const p of [pending, reader.closed]) {
      await rejects(p, (err) => {
        strictEqual(err.name, 'AbortError');
        strictEqual(err.code, 'ABORT_ERR');
        strictEqual(err.cause?.code, 'ERR_STREAM_PREMATURE_CLOSE');
        return true;
      });
    }
  },
};

// A source that is already destroyed, already ended, or a Duplex created
// without a readable side yields a stream that is already cancelled: unlocked,
// with reads resolving done.
export const toWebUnreadableSourceYieldsCancelledStream = {
  async test() {
    const destroyed = new Readable({ read() {} });
    destroyed.destroy();
    await new Promise((resolve) => destroyed.once('close', resolve));

    const ended = new Readable({
      read() {
        this.push(null);
      },
    });
    ended.resume();
    await new Promise((resolve) => ended.once('end', resolve));

    const halfDuplex = new Duplex({
      readable: false,
      write(chunk, encoding, callback) {
        callback();
      },
    });

    for (const source of [destroyed, ended, halfDuplex]) {
      const rs = Readable.toWeb(source);
      strictEqual(rs.locked, false);
      const { done, value } = await rs.getReader().read();
      strictEqual(done, true);
      strictEqual(value, undefined);
    }
  },
};
